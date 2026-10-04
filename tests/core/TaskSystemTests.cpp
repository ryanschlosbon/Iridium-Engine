// M7R R5b.1: the engine task system (ADR-0015). Priorities and ordering, the
// main-thread join rule, the background admission gate, the pinned I/O thread,
// shutdown with pending background work, thread-scoped allocation counters,
// per-worker profiler scopes and steady-state allocation freedom.
#include "core/tasks/TaskSystem.h"

#include "profiling/CpuAllocationProfile.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

    using namespace Iridium;
    using namespace Iridium::Tasks;

    // A failed check exits at once: unwinding past a task system with blocked
    // tasks would hang instead of reporting.
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")" \
                      << std::endl; \
            std::_Exit(1); \
        } \
    } while (false)

    constexpr auto Timeout = std::chrono::seconds(30);

    template <class Predicate>
    bool waitUntil(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + Timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    // Records the order in which tasks start.
    struct OrderLog {
        std::atomic<uint32_t> next{ 0 };
        std::array<std::atomic<int>, 64> ids{};
        void record(int id) {
            const uint32_t slot = next.fetch_add(1);
            if (slot < ids.size()) {
                ids[slot].store(id);
            }
        }
    };

    // Starts, records, then holds its thread until released.
    class GateTask final : public TaskSet {
    public:
        GateTask(TaskPriority priority, int id, OrderLog* log = nullptr)
            : TaskSet(priority, 1, 1), id_(id), log_(log) {}

        std::atomic<bool> started{ false };
        std::atomic<bool> release{ false };
        std::atomic<uint32_t> thread{ NoThreadIndex };

    private:
        void execute(TaskRange, uint32_t threadIndex) override {
            thread.store(threadIndex);
            if (log_ != nullptr) {
                log_->record(id_);
            }
            started.store(true);
            while (!release.load()) {
                std::this_thread::yield();
            }
        }

        int id_;
        OrderLog* log_;
    };

    bool testPrioritiesAndMainThreadJoin() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 1, .pinnedIoThread = false });
        CHECK(system.threadCount() == 2);
        CHECK(system.currentThreadIndex() == 0);

        GateTask blocker(TaskPriority::FrameCritical, 0);
        system.submit(blocker);
        CHECK(waitUntil([&] { return blocker.started.load(); }));
        CHECK(blocker.thread.load() == 1);

        OrderLog log;
        GateTask background(TaskPriority::Background, 3, &log);
        GateTask normal(TaskPriority::Normal, 2, &log);
        GateTask frame(TaskPriority::FrameCritical, 1, &log);
        background.release = true;
        normal.release = true;
        frame.release = true;
        system.submit(background);
        system.submit(normal);
        system.submit(frame);

        // The main thread joins frame-critical work only: it runs the frame task
        // itself (the worker is blocked) and never touches normal/background.
        system.wait(frame);
        CHECK(frame.isComplete());
        CHECK(frame.thread.load() == 0);
        CHECK(!normal.started.load() && !background.started.load());

        // Released, the worker runs the queue in priority order although the
        // background task was submitted first.
        blocker.release = true;
        system.wait(normal);     // main waits without running it
        system.wait(background);
        system.wait(blocker);
        CHECK(log.next.load() == 3);
        CHECK(log.ids[0].load() == 1 && log.ids[1].load() == 2 && log.ids[2].load() == 3);
        CHECK(normal.thread.load() == 1 && background.thread.load() == 1);
        return true;
    }

    bool testParallelForCoverage() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4, .reservedFrameWorkers = 1 });
        for (const TaskPriority priority : { TaskPriority::FrameCritical,
                 TaskPriority::Normal, TaskPriority::Background }) {
            for (const uint32_t count : { 1u, 7u, 64u, 1000u, 4099u }) {
                for (const uint32_t grain : { 1u, 3u, 64u }) {
                    std::vector<std::atomic<uint32_t>> visits(count);
                    std::atomic<bool> grainRespected{ true };
                    system.parallelFor(priority, count, grain,
                        [&](TaskRange range, uint32_t) {
                            if (range.end <= range.begin ||
                                (range.end - range.begin < grain && range.end != count)) {
                                grainRespected = false;
                            }
                            for (uint32_t index = range.begin; index < range.end; ++index) {
                                visits[index].fetch_add(1);
                            }
                        });
                    CHECK(grainRespected.load());
                    CHECK(std::all_of(visits.begin(), visits.end(),
                        [](const std::atomic<uint32_t>& value) { return value.load() == 1; }));
                }
            }
        }
        // An empty task completes at once.
        system.parallelFor(TaskPriority::Normal, 0, 1, [](TaskRange, uint32_t) { CHECK(false); });
        return true;
    }

    bool testBackgroundAdmissionGate() {
        // 4 workers, 2 reserved: at most 2 threads in background work.
        TaskSystem system(TaskSystemConfig{
            .workerThreadCount = 4, .reservedFrameWorkers = 2, .pinnedIoThread = false });
        CHECK(system.backgroundSlotLimit() == 2);

        OrderLog log;
        std::vector<std::unique_ptr<GateTask>> tasks;
        for (int id = 0; id < 6; ++id) {
            tasks.push_back(std::make_unique<GateTask>(TaskPriority::Background, id, &log));
        }
        for (auto& task : tasks) {
            system.submit(*task);
        }
        CHECK(waitUntil([&] { return log.next.load() == 2; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(log.next.load() == 2); // the gate holds the other four
        TaskSystemStatistics statistics = system.takeStatistics();
        CHECK(statistics.backgroundActiveSlots == 2);
        CHECK(statistics.backgroundPendingCount == 4);

        // Frame work still runs while background saturates its slots: the two
        // reserved workers and the main thread are free.
        std::atomic<uint32_t> frameIterations{ 0 };
        std::atomic<uint64_t> frameThreads{ 0 };
        system.parallelFor(TaskPriority::FrameCritical, 256, 1,
            [&](TaskRange range, uint32_t thread) {
                frameThreads.fetch_or(uint64_t{ 1 } << thread);
                frameIterations.fetch_add(range.end - range.begin);
            });
        CHECK(frameIterations.load() == 256);
        for (int id = 0; id < 2; ++id) {
            const uint32_t thread = tasks[static_cast<size_t>(id)]->thread.load();
            CHECK((frameThreads.load() & (uint64_t{ 1 } << thread)) == 0);
        }

        // Releasing one slot admits exactly the oldest pending task (FIFO).
        for (int released = 0; released < 4; ++released) {
            tasks[static_cast<size_t>(released)]->release = true;
            system.wait(*tasks[static_cast<size_t>(released)]);
            CHECK(waitUntil([&] { return log.next.load() == static_cast<uint32_t>(released) + 3; }));
            CHECK(log.ids[static_cast<size_t>(released) + 2].load() == released + 2);
        }
        for (auto& task : tasks) {
            task->release = true;
            system.wait(*task);
        }
        statistics = system.takeStatistics();
        CHECK(statistics.backgroundPeakActiveSlots == 2);
        CHECK(statistics.backgroundActiveSlots == 0);

        // A background task set never runs on more threads than its slots.
        std::atomic<uint32_t> concurrent{ 0 };
        std::atomic<uint32_t> peak{ 0 };
        std::atomic<uint32_t> covered{ 0 };
        system.parallelFor(TaskPriority::Background, 1000, 1,
            [&](TaskRange range, uint32_t) {
                const uint32_t now = concurrent.fetch_add(1) + 1;
                uint32_t previous = peak.load();
                while (now > previous && !peak.compare_exchange_weak(previous, now)) {
                }
                const auto until = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(5);
                while (std::chrono::steady_clock::now() < until) {
                }
                covered.fetch_add(range.end - range.begin);
                concurrent.fetch_sub(1);
            });
        CHECK(covered.load() == 1000);
        CHECK(peak.load() <= 2);
        return true;
    }

    bool testNestedBackgroundForkJoin() {
        // One background slot: a background task's own fork-join must not wait
        // for admission behind itself.
        TaskSystem system(TaskSystemConfig{
            .workerThreadCount = 2, .reservedFrameWorkers = 8, .pinnedIoThread = false });
        CHECK(system.backgroundSlotLimit() == 1);
        std::atomic<uint64_t> sum{ 0 };
        FunctionTaskSet parent(TaskPriority::Background, 4, 1,
            [&](TaskRange range, uint32_t) {
                for (uint32_t outer = range.begin; outer < range.end; ++outer) {
                    system.parallelFor(TaskPriority::Background, 128, 4,
                        [&](TaskRange inner, uint32_t) {
                            uint64_t local = 0;
                            for (uint32_t index = inner.begin; index < inner.end; ++index) {
                                local += index;
                            }
                            sum.fetch_add(local);
                        });
                }
            });
        system.submit(parent);
        CHECK(waitUntil([&] { return parent.isComplete(); }));
        system.wait(parent);
        CHECK(sum.load() == 4ull * (127ull * 128ull / 2ull));
        return true;
    }

    class RecordingPinnedTask final : public PinnedTask {
    public:
        RecordingPinnedTask() : PinnedTask("task.test.io") {}
        TaskSystem* system = nullptr;
        OrderLog* log = nullptr;
        int id = 0;
        bool block = false;
        std::atomic<uint32_t> thread{ NoThreadIndex };

    private:
        void execute() override {
            thread.store(system->currentThreadIndex());
            log->record(id);
            if (block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    };

    bool testPinnedIoThread() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 3 });
        const uint32_t io = system.pinnedIoThreadIndex();
        CHECK(io == 4 && system.threadCount() == 5);

        OrderLog log;
        std::array<RecordingPinnedTask, 32> pinned;
        for (size_t index = 0; index < pinned.size(); ++index) {
            pinned[index].system = &system;
            pinned[index].log = &log;
            pinned[index].id = static_cast<int>(index);
            pinned[index].block = index == 0; // blocking I/O does not stall work
            system.submitPinnedIo(pinned[index]);
        }

        std::atomic<uint64_t> threadMask{ 0 };
        for (int repeat = 0; repeat < 50; ++repeat) {
            for (const TaskPriority priority : { TaskPriority::FrameCritical,
                     TaskPriority::Normal, TaskPriority::Background }) {
                system.parallelFor(priority, 512, 1, [&](TaskRange, uint32_t thread) {
                    threadMask.fetch_or(uint64_t{ 1 } << thread);
                    std::this_thread::yield();
                });
            }
        }
        for (RecordingPinnedTask& task : pinned) {
            system.wait(task);
            CHECK(task.thread.load() == io);
        }
        for (size_t index = 0; index < pinned.size(); ++index) {
            CHECK(log.ids[index].load() == static_cast<int>(index)); // FIFO
        }
        CHECK((threadMask.load() & (uint64_t{ 1 } << io)) == 0); // never task sets
        CHECK(threadMask.load() != 0);
        return true;
    }

    class CountingItem final : public StrandItem {
    public:
        std::atomic<uint32_t>* active = nullptr;
        std::atomic<bool>* overlapped = nullptr;
        OrderLog* log = nullptr;
        int id = 0;

    private:
        void run() override {
            if (active->fetch_add(1) != 0) {
                overlapped->store(true);
            }
            log->record(id);
            std::this_thread::yield();
            active->fetch_sub(1);
        }
    };

    bool testStrandSerialFifo() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4, .pinnedIoThread = false });
        for (const TaskPriority priority : { TaskPriority::Normal, TaskPriority::Background }) {
            Strand strand(system, priority, "task.test.strand");
            std::atomic<uint32_t> active{ 0 };
            std::atomic<bool> overlapped{ false };
            OrderLog log;
            std::array<CountingItem, 48> items;
            for (size_t index = 0; index < items.size(); ++index) {
                items[index].active = &active;
                items[index].overlapped = &overlapped;
                items[index].log = &log;
                items[index].id = static_cast<int>(index);
                CHECK(strand.post(items[index]));
                if ((index % 7) == 0) {
                    std::this_thread::yield(); // let the pump drain and restart
                }
            }
            strand.waitIdle();
            CHECK(strand.isIdle());
            CHECK(!overlapped.load());
            for (size_t index = 0; index < items.size(); ++index) {
                CHECK(items[index].isComplete() && !items[index].wasCancelled());
                CHECK(log.ids[index].load() == static_cast<int>(index));
            }
        }
        return true;
    }

    class LongBackgroundTask final : public TaskSet {
    public:
        explicit LongBackgroundTask(TaskSystem& system)
            : TaskSet(TaskPriority::Background, 1, 1), system_(system) {}
        std::atomic<bool> started{ false };
        std::atomic<bool> sawShutdown{ false };

    private:
        void execute(TaskRange, uint32_t) override {
            started = true;
            while (!system_.isShutdownRequested()) {
                std::this_thread::yield();
            }
            sawShutdown = true;
        }
        TaskSystem& system_;
    };

    class NoopItem final : public StrandItem {
    public:
        std::atomic<bool> ran{ false };

    private:
        void run() override { ran = true; }
    };

    bool testShutdownWithPendingBackgroundWork() {
        auto system = std::make_unique<TaskSystem>(TaskSystemConfig{
            .workerThreadCount = 2, .reservedFrameWorkers = 8 });
        CHECK(system->backgroundSlotLimit() == 1);
        LongBackgroundTask running(*system);
        system->submit(running);
        CHECK(waitUntil([&] { return running.started.load(); }));

        GateTask pendingA(TaskPriority::Background, 0);
        GateTask pendingB(TaskPriority::Background, 1);
        pendingA.release = true;
        pendingB.release = true;
        system->submit(pendingA);
        system->submit(pendingB);
        auto strand = std::make_unique<Strand>(*system, TaskPriority::Background);
        std::array<NoopItem, 3> items;
        for (NoopItem& item : items) {
            CHECK(strand->post(item)); // queued behind the gate
        }
        CHECK(!pendingA.isComplete() && !pendingB.isComplete());
        CHECK(system->takeStatistics().backgroundPendingCount == 3); // A, B, pump

        system->shutdown();
        CHECK(running.isComplete() && running.sawShutdown.load() && !running.wasCancelled());
        CHECK(pendingA.isComplete() && pendingA.wasCancelled() && !pendingA.started.load());
        CHECK(pendingB.isComplete() && pendingB.wasCancelled() && !pendingB.started.load());
        strand->waitIdle();
        for (NoopItem& item : items) {
            CHECK(item.isComplete() && item.wasCancelled() && !item.ran.load());
        }

        // Everything submitted after shutdown is cancelled, never run.
        GateTask late(TaskPriority::FrameCritical, 2);
        late.release = true;
        system->submit(late);
        system->wait(late);
        CHECK(late.wasCancelled() && !late.started.load());
        NoopItem lateItem;
        CHECK(!strand->post(lateItem));
        CHECK(lateItem.wasCancelled());
        CHECK(system->takeStatistics().cancelledCount == 4); // A, B, pump, late
        strand.reset();
        system->shutdown(); // idempotent
        system.reset();
        return true;
    }

    class ThrowingTask final : public TaskSet {
    public:
        ThrowingTask() : TaskSet(TaskPriority::Normal, 16, 1) {}
        std::atomic<uint32_t> ranges{ 0 };

    private:
        void execute(TaskRange range, uint32_t) override {
            ranges.fetch_add(range.end - range.begin);
            throw std::runtime_error("task failure");
        }
    };

    bool testTaskExceptionsDoNotKillWorkers() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 2, .pinnedIoThread = false });
        ThrowingTask task;
        system.submit(task);
        system.wait(task);
        CHECK(task.isComplete() && task.ranges.load() == 16);
        CHECK(system.takeStatistics().exceptionCount >= 1);
        std::atomic<uint32_t> after{ 0 };
        system.parallelFor(TaskPriority::Normal, 64, 1,
            [&](TaskRange range, uint32_t) { after.fetch_add(range.end - range.begin); });
        CHECK(after.load() == 64);
        return true;
    }

    void allocateOnce() {
        void* memory = ::operator new(16);
        ::operator delete(memory);
    }

    bool testThreadScopedAllocationCounters() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4 });
        std::atomic<int> foreignPhase{ 0 };
        std::thread foreign([&] {
            while (foreignPhase.load() != 1) {
                std::this_thread::yield();
            }
            allocateOnce();
            foreignPhase.store(2);
        });
        class AllocatingPinnedTask final : public PinnedTask {
            void execute() override { allocateOnce(); }
        } pinned;
        // Warm the paths once outside the measured frame.
        system.parallelFor(TaskPriority::FrameCritical, 8, 1, [](TaskRange, uint32_t) {});

        beginCpuAllocationFrame();
        allocateOnce(); // main thread: frame
        system.parallelFor(TaskPriority::FrameCritical, 64, 1,
            [](TaskRange range, uint32_t) {
                for (uint32_t index = range.begin; index < range.end; ++index) {
                    allocateOnce(); // frame-critical on any thread: frame
                }
            });
        system.parallelFor(TaskPriority::Background, 32, 1,
            [](TaskRange range, uint32_t) {
                for (uint32_t index = range.begin; index < range.end; ++index) {
                    allocateOnce(); // background workers: background
                }
            });
        system.parallelFor(TaskPriority::Normal, 16, 1,
            [](TaskRange range, uint32_t) {
                for (uint32_t index = range.begin; index < range.end; ++index) {
                    allocateOnce(); // normal on workers (main never helps): background
                }
            });
        system.submitPinnedIo(pinned);
        system.wait(pinned); // I/O thread: background
        foreignPhase.store(1);
        CHECK(waitUntil([&] { return foreignPhase.load() == 2; })); // other thread
        const CpuAllocationFrameSample sample = endCpuAllocationFrame();
        foreign.join();

        std::cout << "  frame " << sample.allocationCount << " / "
                  << sample.requestedBytes << " B, background "
                  << sample.backgroundAllocationCount << " / "
                  << sample.backgroundRequestedBytes << " B\n";
        CHECK(sample.allocationCount == 1 + 64);
        CHECK(sample.requestedBytes == 16 * (1 + 64));
        CHECK(sample.backgroundAllocationCount == 32 + 16 + 1 + 1);
        CHECK(sample.backgroundRequestedBytes == 16 * (32 + 16 + 1 + 1));
        return true;
    }

    bool testWorkerProfilerScopes() {
        CpuProfiler profiler(true);
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4, .profiler = &profiler });
        CHECK(profiler.workerStreamCount() == system.threadCount());

        class NestedScopeTask final : public TaskSet {
        public:
            NestedScopeTask(CpuProfiler& profiler)
                : TaskSet(TaskPriority::Normal, 1, 1, "task.test.normal"), profiler_(profiler) {}

        private:
            void execute(TaskRange, uint32_t) override {
                CpuScope inner(profiler_, "task.test.normal.inner");
            }
            CpuProfiler& profiler_;
        };

        GateTask spanning(TaskPriority::Background, 0);
        spanning.configure(TaskPriority::Background, 1, 1, "task.test.spanning");

        CHECK(profiler.beginFrame());
        {
            CpuScope frameScope(profiler, "task.test.frame");
            NestedScopeTask normal(profiler);
            system.submit(normal);
            system.wait(normal); // runs on a worker: the main thread never helps
            system.parallelFor(TaskPriority::FrameCritical, 64, 8,
                [](TaskRange, uint32_t) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }, "task.test.extract");
            system.submit(spanning);
            CHECK(waitUntil([&] { return spanning.started.load(); }));
            system.recordFrameCounters(profiler);
        }
        CHECK(profiler.endFrame()); // the open background scope does not drop it
        CHECK(profiler.beginFrame());
        spanning.release = true;
        system.wait(spanning);
        CHECK(profiler.endFrame());
        CHECK(profiler.droppedFrameCount() == 0);

        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 2);
        const CpuProfileEvent* normal = nullptr;
        const CpuProfileEvent* inner = nullptr;
        for (const CpuProfileEvent& event : frames[0].workerEvents) {
            CHECK(event.workerIndex >= 1 && event.workerIndex < system.threadCount());
            if (std::string(event.name) == "task.test.normal") {
                normal = &event;
            }
            if (std::string(event.name) == "task.test.normal.inner") {
                inner = &event;
            }
        }
        CHECK(normal != nullptr && inner != nullptr);
        CHECK(inner->parentEventId == normal->eventId);
        // Main-thread scopes (including frame-critical ranges the main thread ran)
        // stay in the shared table: the critical path.
        for (const CpuProfileEvent& event : frames[0].events) {
            CHECK(event.workerIndex == 0);
        }
        bool counted = false;
        for (const FrameProfileCounter& counter : frames[0].counters) {
            if (std::string(counter.name) == "task.frame.count") {
                counted = counter.value == 1;
            }
        }
        CHECK(counted);
        bool spanningMerged = false;
        for (const CpuProfileEvent& event : frames[1].workerEvents) {
            spanningMerged = spanningMerged ||
                std::string(event.name) == "task.test.spanning";
        }
        CHECK(spanningMerged);
        const ProfileRunStatistics statistics = profiler.snapshotRunStatistics();
        bool workerRange = false;
        for (const ProfileRangeRunStatistics& range : statistics.workerRanges) {
            workerRange = workerRange || std::string(range.name) == "task.test.normal";
        }
        CHECK(workerRange);
        return true;
    }

    struct HookCounters {
        std::atomic<uint64_t> allocations{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
    };

    void* hookAllocate(size_t alignment, size_t size, void* userData) {
        auto* counters = static_cast<HookCounters*>(userData);
        counters->allocations.fetch_add(1);
        counters->bytes.fetch_add(size);
        return ::operator new(size, std::align_val_t{ alignment });
    }

    void hookDeallocate(void* memory, size_t, void*) {
        ::operator delete(memory, std::align_val_t{ 16 });
    }

    class SumTask final : public TaskSet {
    public:
        SumTask(TaskPriority priority, uint32_t count, uint32_t grain)
            : TaskSet(priority, count, grain) {}
        std::atomic<uint64_t> sum{ 0 };

    private:
        void execute(TaskRange range, uint32_t) override {
            uint64_t local = 0;
            for (uint32_t index = range.begin; index < range.end; ++index) {
                local += index;
            }
            sum.fetch_add(local, std::memory_order_relaxed);
        }
    };

    class IncrementPinned final : public PinnedTask {
    public:
        std::atomic<uint32_t> runs{ 0 };

    private:
        void execute() override { runs.fetch_add(1); }
    };

    class IncrementItem final : public StrandItem {
    public:
        std::atomic<uint32_t> runs{ 0 };

    private:
        void run() override { runs.fetch_add(1); }
    };

    bool testNoSteadyAllocations() {
        HookCounters hook;
        CpuProfiler profiler(true);
        TaskSystem system(TaskSystemConfig{
            .workerThreadCount = 4,
            .reservedFrameWorkers = 2,
            .profiler = &profiler,
            .allocate = hookAllocate,
            .deallocate = hookDeallocate,
            .allocatorUserData = &hook,
        });
        CHECK(hook.allocations.load() > 0);
        Strand strand(system, TaskPriority::Background, "task.test.strand");

        uint64_t expectedSum = 0;
        auto iteration = [&](uint32_t index) {
            SumTask frame(TaskPriority::FrameCritical, 512, 16);
            SumTask normal(TaskPriority::Normal, 256, 8);
            SumTask background(TaskPriority::Background, 256, 8);
            IncrementPinned io;
            IncrementItem item;
            std::atomic<uint64_t> nested{ 0 };
            FunctionTaskSet forkJoin(TaskPriority::Background, 2, 1,
                [&](TaskRange range, uint32_t) {
                    for (uint32_t outer = range.begin; outer < range.end; ++outer) {
                        system.parallelFor(TaskPriority::Background, 64, 4,
                            [&](TaskRange inner, uint32_t) {
                                nested.fetch_add(inner.end - inner.begin);
                            });
                    }
                }, "task.test.forkjoin");
            system.submit(background);
            system.submit(normal);
            system.submit(forkJoin);
            system.submitPinnedIo(io);
            (void)strand.post(item);
            system.submit(frame);
            system.wait(frame);
            system.parallelFor(TaskPriority::FrameCritical, 128, 8,
                [&](TaskRange range, uint32_t) {
                    CpuScope scope(profiler, "task.test.scope");
                    nested.fetch_add(range.end - range.begin);
                }, "task.test.parallel");
            system.wait(normal);
            system.wait(background);
            system.wait(forkJoin);
            system.wait(io);
            item.waitPassively();
            expectedSum = 511ull * 512ull / 2ull;
            return frame.sum.load() == expectedSum &&
                normal.sum.load() == 255ull * 256ull / 2ull &&
                background.sum.load() == 255ull * 256ull / 2ull &&
                nested.load() == 2 * 64 + 128 && io.runs.load() == 1 &&
                item.runs.load() == 1 && index != 0xFFFF'FFFFu;
        };

        // Warm-up: profiler frames, every thread's first task, strand restart.
        for (uint32_t index = 0; index < 64; ++index) {
            CHECK(profiler.beginFrame());
            CHECK(iteration(index));
            CHECK(profiler.endFrame());
        }
        const uint64_t hookAfterWarmup = hook.allocations.load();

        constexpr uint32_t SteadyFrames = 500;
        uint64_t frameAllocations = 0;
        uint64_t backgroundAllocations = 0;
        bool correct = true;
        for (uint32_t index = 0; index < SteadyFrames; ++index) {
            CHECK(profiler.beginFrame());
            beginCpuAllocationFrame();
            correct = iteration(index) && correct;
            system.recordFrameCounters(profiler);
            const CpuAllocationFrameSample sample = endCpuAllocationFrame();
            CHECK(profiler.endFrame());
            frameAllocations += sample.allocationCount;
            backgroundAllocations += sample.backgroundAllocationCount;
        }
        const uint64_t hookSteady = hook.allocations.load() - hookAfterWarmup;
        std::cout << "  " << SteadyFrames << " steady frames (frame-critical submit+join, "
                  << "parallelFor, normal, background, nested background fork-join, "
                  << "pinned I/O, strand, worker scopes): frame allocations "
                  << frameAllocations << ", background allocations "
                  << backgroundAllocations << ", enkiTS hook allocations " << hookSteady
                  << "\n";
        CHECK(correct);
        CHECK(frameAllocations == 0);
        CHECK(backgroundAllocations == 0);
        CHECK(hookSteady == 0);
        CHECK(profiler.droppedFrameCount() == 0);
        return true;
    }

    // M7R R5b.2: Periodic runs on the frame tick, never overlaps, honours its
    // interval, runs on the pinned I/O thread when asked, and stop() waits.
    bool testPeriodic() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 3 });
        std::atomic<uint32_t> backgroundRuns{ 0 };
        std::atomic<uint32_t> active{ 0 };
        std::atomic<bool> overlapped{ false };
        std::atomic<bool> hold{ true };
        Periodic background(system, std::chrono::nanoseconds(0),
            Periodic::Target::Background, [&] {
                if (active.fetch_add(1) != 0) overlapped = true;
                while (hold.load()) std::this_thread::yield();
                backgroundRuns.fetch_add(1);
                active.fetch_sub(1);
            }, "task.test.periodic");
        std::atomic<uint32_t> ioThread{ NoThreadIndex };
        std::atomic<uint32_t> ioRuns{ 0 };
        Periodic pinned(system, std::chrono::hours(1), Periodic::Target::PinnedIo,
            [&] {
                ioThread = system.currentThreadIndex();
                ioRuns.fetch_add(1);
            });
        // Nothing runs before the first tick.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(background.runCount() == 0 && pinned.runCount() == 0);
        system.tickPeriodic();
        CHECK(waitUntil([&] { return active.load() == 1; }));
        CHECK(waitUntil([&] { return ioRuns.load() == 1; }));
        CHECK(ioThread.load() == system.pinnedIoThreadIndex());
        // A tick while the previous run is in flight does not start another.
        for (int tick = 0; tick < 50; ++tick) system.tickPeriodic();
        CHECK(background.runCount() == 1);
        hold = false;
        CHECK(waitUntil([&] { return backgroundRuns.load() == 1; }));
        CHECK(waitUntil([&] {
            system.tickPeriodic();
            return backgroundRuns.load() >= 3;
        }));
        CHECK(!overlapped.load());
        // The one-hour pinned interval has not elapsed.
        CHECK(ioRuns.load() == 1);
        background.stop();
        const uint64_t stopped = background.runCount();
        for (int tick = 0; tick < 20; ++tick) system.tickPeriodic();
        CHECK(background.runCount() == stopped);
        pinned.stop();
        return true;
    }

    // M7R R5b.2: FunctionStrand runs posted callables serially in post order,
    // accepts posts from inside its own work and reclaims completed items.
    bool testFunctionStrand() {
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4, .pinnedIoThread = false });
        CHECK(TaskSystem::current() == &system);
        CHECK(TaskSystem::forCurrentThread() == &system);
        FunctionStrand strand(system, TaskPriority::Background, "task.test.function_strand");
        OrderLog log;
        std::atomic<uint32_t> active{ 0 };
        std::atomic<bool> overlapped{ false };
        std::atomic<bool> workerSeesSystem{ true };
        for (int id = 0; id < 40; ++id) {
            CHECK(strand.post([&, id] {
                if (active.fetch_add(1) != 0) overlapped = true;
                log.record(id);
                if (TaskSystem::forCurrentThread() != &system) workerSeesSystem = false;
                if (id == 39) {
                    // Posting from inside the strand queues behind the current item.
                    (void)strand.post([&] { log.record(40); });
                }
                active.fetch_sub(1);
            }));
        }
        strand.waitIdle();
        CHECK(!overlapped.load());
        CHECK(workerSeesSystem.load());
        CHECK(log.next.load() == 41);
        for (int id = 0; id <= 40; ++id) CHECK(log.ids[id].load() == id);
        // A foreign thread is not one the system runs.
        std::atomic<bool> foreignSeesNone{ false };
        std::thread([&] {
            foreignSeesNone = TaskSystem::forCurrentThread() == nullptr;
        }).join();
        CHECK(foreignSeesNone.load());
        system.shutdown();
        CHECK(TaskSystem::current() == nullptr);
        CHECK(!strand.post([&] { log.record(99); }));
        return true;
    }

    // M7R R5b.3: background ranges run below normal OS priority and frame work
    // at normal priority, also when a background waiter helps with it.
    bool testBackgroundOsPriority() {
#if defined(_WIN32)
        TaskSystem system(TaskSystemConfig{ .workerThreadCount = 4,
            .reservedFrameWorkers = 1, .pinnedIoThread = false });
        std::atomic<int> backgroundPriority{ 99 };
        std::atomic<int> nestedPriority{ 99 };
        std::atomic<int> afterNestedPriority{ 99 };
        system.parallelFor(TaskPriority::Background, 1, 1,
            [&](TaskRange, uint32_t) {
                backgroundPriority = GetThreadPriority(GetCurrentThread());
                // A Background waiter may run this frame work itself.
                FunctionTaskSet frame(TaskPriority::FrameCritical, 1, 1,
                    [&](TaskRange, uint32_t) {
                        nestedPriority = GetThreadPriority(GetCurrentThread());
                    });
                system.submit(frame);
                system.wait(frame);
                afterNestedPriority = GetThreadPriority(GetCurrentThread());
            });
        CHECK(backgroundPriority.load() == THREAD_PRIORITY_BELOW_NORMAL);
        CHECK(nestedPriority.load() == THREAD_PRIORITY_NORMAL);
        CHECK(afterNestedPriority.load() == THREAD_PRIORITY_BELOW_NORMAL);
        std::atomic<int> framePriority{ 99 };
        system.parallelFor(TaskPriority::FrameCritical, 64, 1,
            [&](TaskRange, uint32_t threadIndex) {
                if (threadIndex != 0) framePriority = GetThreadPriority(GetCurrentThread());
            });
        CHECK(framePriority.load() == 99 ||
            framePriority.load() == THREAD_PRIORITY_NORMAL);
        CHECK(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL);
#endif
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        { "priorities and the main-thread join rule", testPrioritiesAndMainThreadJoin },
        { "parallelFor coverage at every priority", testParallelForCoverage },
        { "background admission gate", testBackgroundAdmissionGate },
        { "nested background fork-join under a full gate", testNestedBackgroundForkJoin },
        { "pinned I/O thread", testPinnedIoThread },
        { "strand is serial and FIFO", testStrandSerialFifo },
        { "shutdown with pending background work", testShutdownWithPendingBackgroundWork },
        { "task exceptions do not kill workers", testTaskExceptionsDoNotKillWorkers },
        { "thread-scoped allocation counters", testThreadScopedAllocationCounters },
        { "per-worker profiler scopes", testWorkerProfilerScopes },
        { "no steady allocations", testNoSteadyAllocations },
        { "periodic work on the frame tick", testPeriodic },
        { "function strand", testFunctionStrand },
        { "background OS priority", testBackgroundOsPriority },
    };
    for (const auto& test : tests) {
        std::cout << test.name << std::endl;
        if (!test.run()) {
            std::cerr << "FAIL: " << test.name << '\n';
            return 1;
        }
    }
    std::cout << "TaskSystemTests passed\n";
    return 0;
}
