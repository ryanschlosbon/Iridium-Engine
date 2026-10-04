// M7R R5b.0 spike: the enkiTS v1.12 properties the ADR-0015 task model relies on,
// checked directly against the library on MSVC/C++20 (no engine wrapper):
//
//   1. three priorities, and a waiter that filters by priority never runs a
//      lower-priority task (the main-thread join rule);
//   2. pinned tasks on a dedicated I/O thread that never runs task sets;
//   3. waiting from inside a task (fork-join on a worker);
//   4. no allocation after Initialize, measured two ways: the enkiTS custom
//      allocator hook and the engine's global operator new counter.
#include "profiling/CpuAllocationProfile.h"

#include <TaskScheduler.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

    int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
            ++failures; \
            return false; \
        } \
    } while (false)

    struct AllocatorCounters {
        std::atomic<uint64_t> allocations{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<uint64_t> frees{ 0 };
    };

    void* countingAlloc(size_t alignment, size_t size, void* userData,
        const char*, int) {
        auto* counters = static_cast<AllocatorCounters*>(userData);
        counters->allocations.fetch_add(1, std::memory_order_relaxed);
        counters->bytes.fetch_add(size, std::memory_order_relaxed);
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        void* memory = nullptr;
        return posix_memalign(&memory, alignment, size) == 0 ? memory : nullptr;
#endif
    }

    void countingFree(void* memory, size_t, void* userData, const char*, int) {
        auto* counters = static_cast<AllocatorCounters*>(userData);
        counters->frees.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

    enki::TaskSchedulerConfig makeConfig(uint32_t workers, AllocatorCounters& counters) {
        enki::TaskSchedulerConfig config;
        config.numTaskThreadsToCreate = workers;
        config.customAllocator.alloc = countingAlloc;
        config.customAllocator.free = countingFree;
        config.customAllocator.userData = &counters;
        return config;
    }

    // Spins on one worker until released, so the test controls which tasks are
    // queued behind it.
    struct BlockerTask final : enki::ITaskSet {
        std::atomic<bool> started{ false };
        std::atomic<bool> release{ false };
        void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
            started.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    };

    struct OrderRecorder {
        std::atomic<uint32_t> next{ 0 };
        std::array<std::atomic<int>, 8> order{};
    };

    struct RecordingTask final : enki::ITaskSet {
        OrderRecorder* recorder = nullptr;
        int id = 0;
        std::atomic<uint32_t> threadNum{ enki::NO_THREAD_NUM };
        void ExecuteRange(enki::TaskSetPartition, uint32_t thread) override {
            threadNum.store(thread, std::memory_order_relaxed);
            const uint32_t slot = recorder->next.fetch_add(1, std::memory_order_acq_rel);
            if (slot < recorder->order.size()) {
                recorder->order[slot].store(id, std::memory_order_release);
            }
        }
    };

    void waitPassively(const enki::ICompletable& task) {
        while (!task.GetIsComplete()) {
            std::this_thread::yield();
        }
    }

    bool testPriorities() {
        static_assert(enki::TASK_PRIORITY_NUM == 3, "the engine relies on three priorities");
        AllocatorCounters counters;
        enki::TaskScheduler scheduler;
        scheduler.Initialize(makeConfig(1, counters)); // main (0) + one worker (1)
        CHECK(scheduler.GetNumTaskThreads() == 2);

        BlockerTask blocker;
        blocker.m_Priority = enki::TASK_PRIORITY_HIGH;
        scheduler.AddTaskSetToPipe(&blocker);
        while (!blocker.started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Queue low, normal and high work behind the blocked worker, low first.
        OrderRecorder recorder;
        RecordingTask low;
        low.recorder = &recorder;
        low.id = 3;
        low.m_Priority = enki::TASK_PRIORITY_LOW;
        RecordingTask normal;
        normal.recorder = &recorder;
        normal.id = 2;
        normal.m_Priority = enki::TASK_PRIORITY_MED;
        RecordingTask high;
        high.recorder = &recorder;
        high.id = 1;
        high.m_Priority = enki::TASK_PRIORITY_HIGH;
        scheduler.AddTaskSetToPipe(&low);
        scheduler.AddTaskSetToPipe(&normal);

        // Main-thread join rule: a wait filtered to HIGH runs the HIGH task on the
        // main thread but never touches the queued normal or low task.
        scheduler.AddTaskSetToPipe(&high);
        scheduler.WaitforTask(&high, enki::TASK_PRIORITY_HIGH);
        CHECK(high.GetIsComplete());
        CHECK(high.threadNum.load() == 0);
        CHECK(!low.GetIsComplete() && !normal.GetIsComplete());
        CHECK(recorder.next.load() == 1);

        // Release the worker: it drains the remaining queue in priority order even
        // though the low task was queued first.
        blocker.release.store(true, std::memory_order_release);
        waitPassively(normal);
        waitPassively(low);
        waitPassively(blocker);
        CHECK(recorder.next.load() == 3);
        CHECK(recorder.order[0].load() == 1);
        CHECK(recorder.order[1].load() == 2);
        CHECK(recorder.order[2].load() == 3);
        CHECK(normal.threadNum.load() == 1 && low.threadNum.load() == 1);

        scheduler.WaitforAllAndShutdown();
        return true;
    }

    enki::TaskScheduler* g_ioScheduler = nullptr;

    struct PinnedLoopTask final : enki::IPinnedTask {
        std::atomic<bool> running{ false };
        void Execute() override {
            running.store(true, std::memory_order_release);
            while (!g_ioScheduler->GetIsShutdownRequested()) {
                g_ioScheduler->WaitForNewPinnedTasks();
                g_ioScheduler->RunPinnedTasks();
            }
        }
    };

    struct IoTask final : enki::IPinnedTask {
        std::atomic<uint32_t> executedOn{ enki::NO_THREAD_NUM };
        bool simulateBlockingIo = true;
        void Execute() override {
            executedOn.store(g_ioScheduler->GetThreadNum(), std::memory_order_relaxed);
            if (simulateBlockingIo) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    };

    struct ThreadMaskTask final : enki::ITaskSet {
        std::atomic<uint64_t> threadMask{ 0 };
        ThreadMaskTask() : enki::ITaskSet(4096, 1) {}
        void ExecuteRange(enki::TaskSetPartition range, uint32_t thread) override {
            threadMask.fetch_or(uint64_t{ 1 } << thread, std::memory_order_relaxed);
            volatile uint32_t sink = 0;
            for (uint32_t index = range.start; index < range.end; ++index) {
                sink = sink + index;
            }
        }
    };

    bool testPinnedIoThread() {
        AllocatorCounters counters;
        enki::TaskScheduler scheduler;
        g_ioScheduler = &scheduler;
        scheduler.Initialize(makeConfig(3, counters)); // main + 2 workers + I/O
        const uint32_t ioThread = scheduler.GetNumTaskThreads() - 1;
        CHECK(ioThread == 3);

        PinnedLoopTask loop;
        loop.threadNum = ioThread;
        scheduler.AddPinnedTask(&loop);
        while (!loop.running.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int repeat = 0; repeat < 16; ++repeat) {
            IoTask io;
            io.threadNum = ioThread;
            ThreadMaskTask work;
            scheduler.AddPinnedTask(&io);
            scheduler.AddTaskSetToPipe(&work);
            scheduler.WaitforTask(&work);
            waitPassively(io);
            CHECK(io.executedOn.load() == ioThread);
            // The I/O thread sits in WaitForNewPinnedTasks and never runs task sets.
            CHECK((work.threadMask.load() & (uint64_t{ 1 } << ioThread)) == 0);
        }

        scheduler.WaitforAllAndShutdown();
        g_ioScheduler = nullptr;
        return true;
    }

    struct ChildTask final : enki::ITaskSet {
        std::atomic<uint64_t> sum{ 0 };
        ChildTask() : enki::ITaskSet(1024, 16) {}
        void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
            uint64_t local = 0;
            for (uint32_t index = range.start; index < range.end; ++index) {
                local += index;
            }
            sum.fetch_add(local, std::memory_order_relaxed);
        }
    };

    struct ParentTask final : enki::ITaskSet {
        enki::TaskScheduler* scheduler = nullptr;
        std::atomic<uint64_t> total{ 0 };
        std::atomic<uint32_t> parentThread{ enki::NO_THREAD_NUM };
        ParentTask() : enki::ITaskSet(4, 1) {}
        void ExecuteRange(enki::TaskSetPartition range, uint32_t thread) override {
            parentThread.store(thread, std::memory_order_relaxed);
            for (uint32_t index = range.start; index < range.end; ++index) {
                ChildTask child;
                child.m_Priority = enki::TASK_PRIORITY_LOW;
                scheduler->AddTaskSetToPipe(&child);
                // Waiting inside a task: the worker runs other tasks (including
                // the child's partitions) until the child completes.
                scheduler->WaitforTask(&child, enki::TASK_PRIORITY_LOW);
                total.fetch_add(child.sum.load(), std::memory_order_relaxed);
            }
        }
    };

    bool testWaitInsideTask() {
        AllocatorCounters counters;
        enki::TaskScheduler scheduler;
        scheduler.Initialize(makeConfig(4, counters));
        ParentTask parent;
        parent.scheduler = &scheduler;
        parent.m_Priority = enki::TASK_PRIORITY_LOW;
        scheduler.AddTaskSetToPipe(&parent);
        // A main-thread wait that may run background work is allowed here (it is
        // the parent's owner), and nested waits must still complete.
        scheduler.WaitforTask(&parent);
        CHECK(parent.GetIsComplete());
        CHECK(parent.total.load() == 4ull * (1023ull * 1024ull / 2ull));
        scheduler.WaitforAllAndShutdown();
        return true;
    }

    struct SteadyTask final : enki::ITaskSet {
        std::atomic<uint64_t> sum{ 0 };
        SteadyTask() : enki::ITaskSet(256, 8) {}
        void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
            uint64_t local = 0;
            for (uint32_t index = range.start; index < range.end; ++index) {
                local += index;
            }
            sum.fetch_add(local, std::memory_order_relaxed);
        }
    };

    bool testNoAllocationAfterInitialize() {
        AllocatorCounters counters;
        enki::TaskScheduler scheduler;
        g_ioScheduler = &scheduler;
        scheduler.Initialize(makeConfig(4, counters)); // main + 3 workers + I/O
        const uint64_t initAllocations = counters.allocations.load();
        const uint64_t initBytes = counters.bytes.load();
        CHECK(initAllocations > 0); // the hook is live
        const uint32_t ioThread = scheduler.GetNumTaskThreads() - 1;
        PinnedLoopTask loop;
        loop.threadNum = ioThread;
        scheduler.AddPinnedTask(&loop);
        while (!loop.running.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Warm every path once (thread-local first use, pipes, semaphores).
        auto runIteration = [&scheduler, ioThread](uint32_t iteration) {
            SteadyTask high;
            high.m_Priority = enki::TASK_PRIORITY_HIGH;
            SteadyTask low;
            low.m_Priority = enki::TASK_PRIORITY_LOW;
            ParentTask nested;
            nested.scheduler = &scheduler;
            nested.m_Priority = enki::TASK_PRIORITY_MED;
            nested.m_SetSize = 1;
            IoTask io;
            io.threadNum = ioThread;
            io.simulateBlockingIo = false;
            scheduler.AddTaskSetToPipe(&low);
            scheduler.AddTaskSetToPipe(&high);
            if ((iteration % 8) == 0) {
                scheduler.AddTaskSetToPipe(&nested);
                scheduler.AddPinnedTask(&io);
            }
            scheduler.WaitforTask(&high, enki::TASK_PRIORITY_HIGH);
            waitPassively(low);
            if ((iteration % 8) == 0) {
                waitPassively(nested);
                waitPassively(io);
            }
            return high.sum.load() == 255ull * 256ull / 2ull &&
                low.sum.load() == 255ull * 256ull / 2ull;
        };
        for (uint32_t iteration = 0; iteration < 64; ++iteration) {
            CHECK(runIteration(iteration));
        }

        const uint64_t warmAllocations = counters.allocations.load();
        constexpr uint32_t SteadyIterations = 2000;
        Iridium::beginCpuAllocationFrame();
        bool resultsCorrect = true;
        for (uint32_t iteration = 0; iteration < SteadyIterations; ++iteration) {
            resultsCorrect = runIteration(iteration) && resultsCorrect;
        }
        const Iridium::CpuAllocationFrameSample engine =
            Iridium::endCpuAllocationFrame();
        const uint64_t steadyHookAllocations =
            counters.allocations.load() - warmAllocations;

        std::cout << "  enkiTS init: " << initAllocations << " allocations, "
                  << initBytes << " bytes (custom allocator hook)\n"
                  << "  warm-up (64 iterations): "
                  << (warmAllocations - initAllocations) << " hook allocations\n"
                  << "  steady (" << SteadyIterations
                  << " iterations: HIGH+LOW task sets, nested MED wait, pinned I/O): "
                  << steadyHookAllocations << " hook allocations; engine operator new "
                  << engine.allocationCount << " calls / " << engine.requestedBytes
                  << " B on the main thread, " << engine.backgroundAllocationCount
                  << " calls / " << engine.backgroundRequestedBytes
                  << " B on the enkiTS threads\n";
        CHECK(resultsCorrect);
        CHECK(warmAllocations == initAllocations);
        CHECK(steadyHookAllocations == 0);
        CHECK(engine.allocationCount == 0);
        // R5b.1 made the counters thread-scoped; worker threads count here.
        CHECK(engine.backgroundAllocationCount == 0);

        scheduler.WaitforAllAndShutdown();
        CHECK(counters.frees.load() == counters.allocations.load());
        g_ioScheduler = nullptr;
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        { "priorities", testPriorities },
        { "pinned I/O thread", testPinnedIoThread },
        { "wait inside a task", testWaitInsideTask },
        { "no allocation after Initialize", testNoAllocationAfterInitialize },
    };
    for (const auto& test : tests) {
        std::cout << test.name << '\n';
        if (!test.run()) {
            std::cerr << "FAIL: " << test.name << '\n';
        }
    }
    if (failures == 0) {
        std::cout << "EnkiTsSpikeTests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
