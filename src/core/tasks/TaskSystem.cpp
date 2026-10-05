#include "core/tasks/TaskSystem.h"

#include "profiling/CpuAllocationProfile.h"
#include "profiling/CpuProfiler.h"

#include <TaskScheduler.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cwchar>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Iridium::Tasks {

    namespace {

        // Task, pinned-task and strand-item states. Pending and Running are
        // waited on with atomic wait/notify. Settling is a short transitional
        // state: the completing thread notifies while it holds it and then
        // publishes Done/Cancelled, so an owner that sees a final state can
        // destroy the object without racing the notification.
        enum TaskState : uint32_t {
            StateIdle = 0,
            StatePending = 1,
            StateRunning = 2,
            StateDone = 3,
            StateCancelled = 4,
            StateSettling = 5,
        };

        constexpr uint32_t MaxBackgroundSlots = 128;

        // The TaskSystem whose per-thread setup (profiler stream) this thread did.
        thread_local const void* tlsBoundSystem = nullptr;
        // Priority of the task this thread is executing, -1 outside any task.
        thread_local int tlsCurrentPriority = -1;

        [[nodiscard]] bool isFinalState(uint32_t state) noexcept {
            return state == StateIdle || state == StateDone || state == StateCancelled;
        }

        void settle(std::atomic<uint32_t>& state, uint32_t finalState) noexcept {
            state.store(StateSettling, std::memory_order_release);
            state.notify_all();
            state.store(finalState, std::memory_order_release);
        }

        void waitForFinalState(const std::atomic<uint32_t>& state) noexcept {
            for (;;) {
                const uint32_t current = state.load(std::memory_order_acquire);
                if (isFinalState(current)) {
                    return;
                }
                if (current == StateSettling) {
                    std::this_thread::yield();
                }
                else {
                    state.wait(current, std::memory_order_acquire);
                }
            }
        }

        [[nodiscard]] enki::TaskPriority toEnki(TaskPriority priority) noexcept {
            switch (priority) {
            case TaskPriority::FrameCritical:
                return enki::TASK_PRIORITY_HIGH;
            case TaskPriority::Normal:
                return enki::TASK_PRIORITY_MED;
            case TaskPriority::Background:
                break;
            }
            return enki::TASK_PRIORITY_LOW;
        }

        [[nodiscard]] uint32_t ceilDivide(uint32_t value, uint32_t divisor) noexcept {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(value) + divisor - 1) / divisor);
        }

        [[nodiscard]] uint64_t nowNanoseconds() noexcept {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void atomicMax(std::atomic<uint64_t>& target, uint64_t value) noexcept {
            uint64_t current = target.load(std::memory_order_relaxed);
            while (value > current &&
                !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
            }
        }

        // The live TaskSystem (at most one exists) and its I/O thread index, for
        // current() and the thread-start hook (enkiTS callbacks carry no user data).
        std::atomic<TaskSystem*> liveSystem{ nullptr };
        std::atomic<uint32_t> liveIoThread{ NoThreadIndex };

        // enkiTS thread-start hook (M7R R5b.2): names the thread so profilers,
        // debuggers and the thread inventory can tell workers and the I/O thread
        // apart. SetThreadDescription is resolved at run time (Windows 10 1607+).
        void onTaskThreadStart(uint32_t threadNum) {
#if defined(_WIN32)
            using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
            static const SetDescription setDescription = [] {
                const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
                return kernel != nullptr
                    ? reinterpret_cast<SetDescription>(reinterpret_cast<void*>(
                        GetProcAddress(kernel, "SetThreadDescription")))
                    : nullptr;
            }();
            if (setDescription == nullptr) {
                return;
            }
            wchar_t name[48]{};
            if (threadNum == liveIoThread.load(std::memory_order_acquire)) {
                (void)swprintf(name, std::size(name), L"iridium.task.io");
            }
            else {
                (void)swprintf(name, std::size(name), L"iridium.task.worker.%u", threadNum);
            }
            (void)setDescription(GetCurrentThread(), name);
#else
            (void)threadNum;
#endif
        }

        // The OS priority this worker currently runs at (Windows; 0 = normal).
        thread_local int tlsOsPriority = 0;

        // M7R R5b.3: runs one task range at the OS priority of its task class
        // (Background below normal, everything else normal) and restores the
        // previous one, so nested ranges (a background waiter helping with frame
        // work) keep the right priority. The main thread is never retuned.
        class OsPriorityScope final {
        public:
            OsPriorityScope(bool enabled, TaskPriority priority,
                uint32_t threadNum) noexcept {
#if defined(_WIN32)
                if (!enabled || threadNum == 0) {
                    return;
                }
                const int wanted = priority == TaskPriority::Background
                    ? THREAD_PRIORITY_BELOW_NORMAL
                    : THREAD_PRIORITY_NORMAL;
                previous_ = tlsOsPriority;
                if (wanted != previous_ &&
                    SetThreadPriority(GetCurrentThread(), wanted) != 0) {
                    tlsOsPriority = wanted;
                    changed_ = true;
                }
#else
                (void)enabled;
                (void)priority;
                (void)threadNum;
#endif
            }

            ~OsPriorityScope() {
#if defined(_WIN32)
                if (changed_ &&
                    SetThreadPriority(GetCurrentThread(), previous_) != 0) {
                    tlsOsPriority = previous_;
                }
#endif
            }

            OsPriorityScope(const OsPriorityScope&) = delete;
            OsPriorityScope& operator=(const OsPriorityScope&) = delete;

        private:
            int previous_ = 0;
            bool changed_ = false;
        };

    } // namespace

    namespace detail {

        struct TaskSetAdapter final : enki::ITaskSet {
            TaskSet* owner = nullptr;
            void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override;
        };

        struct PinnedAdapter final : enki::IPinnedTask {
            PinnedTask* owner = nullptr;
            void Execute() override;
        };

        struct IoLoopTask final : enki::IPinnedTask {
            TaskSystem* system = nullptr;
            void Execute() override;
        };

        static_assert(sizeof(TaskSetAdapter) <= TaskBackendStorageSize &&
            alignof(TaskSetAdapter) <= 16, "TaskSet backend storage is too small");
        static_assert(sizeof(PinnedAdapter) <= TaskBackendStorageSize &&
            alignof(PinnedAdapter) <= 16, "PinnedTask backend storage is too small");

    } // namespace detail

    struct TaskSystem::Impl {
        // Allocator hooks first: the scheduler (destroyed last) frees through them.
        void* (*allocate)(size_t, size_t, void*) = nullptr;
        void (*deallocate)(void*, size_t, void*) = nullptr;
        void* allocatorUserData = nullptr;

        enki::TaskScheduler scheduler;
        CpuProfiler* profiler = nullptr;
        uint32_t workerCount = 0;
        uint32_t threadCount = 0;
        uint32_t ioThread = NoThreadIndex;
        uint32_t backgroundLimit = 1;
        bool lowerBackgroundOsPriority = true;
        detail::IoLoopTask ioLoop;
        std::atomic<uint32_t> ioLoopRunning{ 0 };

        std::atomic<bool> shutdownRequested{ false };
        std::atomic<bool> stopped{ false };

        // Background admission gate.
        std::mutex gateMutex;
        TaskSet* pendingHead = nullptr;
        TaskSet* pendingTail = nullptr;
        uint32_t pendingCount = 0;
        uint32_t activeSlots = 0;
        uint32_t peakActiveSlots = 0;

        // Periodic registrations (M7R R5b.2), ticked by tickPeriodic.
        std::mutex periodicMutex;
        Periodic* periodicHead = nullptr;

        std::atomic<uint64_t> frameTaskCount{ 0 };
        std::atomic<uint64_t> frameStartLatencyMax{ 0 };
        std::atomic<uint64_t> frameStartLatencyTotal{ 0 };
        std::atomic<uint64_t> backgroundAdmitted{ 0 };
        std::atomic<uint64_t> cancelled{ 0 };
        std::atomic<uint64_t> exceptions{ 0 };

        static void* allocateBridge(size_t alignment, size_t size, void* userData,
            const char*, int) {
            auto* impl = static_cast<Impl*>(userData);
            return impl->allocate(alignment, size, impl->allocatorUserData);
        }

        static void deallocateBridge(void* memory, size_t size, void* userData,
            const char*, int) {
            auto* impl = static_cast<Impl*>(userData);
            impl->deallocate(memory, size, impl->allocatorUserData);
        }
    };

    namespace detail {

        struct TaskSystemAccess {
            using Impl = TaskSystem::Impl;

            static TaskSetAdapter& adapter(TaskSet& task) noexcept {
                return *std::launder(reinterpret_cast<TaskSetAdapter*>(task.backend_));
            }
            static const TaskSetAdapter& adapter(const TaskSet& task) noexcept {
                return *std::launder(
                    reinterpret_cast<const TaskSetAdapter*>(task.backend_));
            }
            static PinnedAdapter& adapter(PinnedTask& task) noexcept {
                return *std::launder(reinterpret_cast<PinnedAdapter*>(task.backend_));
            }
            static const PinnedAdapter& adapter(const PinnedTask& task) noexcept {
                return *std::launder(
                    reinterpret_cast<const PinnedAdapter*>(task.backend_));
            }
            static Impl& impl(TaskSystem& system) noexcept { return *system.impl_; }

            static void construct(TaskSet& task) noexcept {
                auto* backend = new (task.backend_) TaskSetAdapter();
                backend->owner = &task;
            }
            static void destroy(TaskSet& task) noexcept {
                adapter(task).~TaskSetAdapter();
            }
            static void construct(PinnedTask& task) noexcept {
                auto* backend = new (task.backend_) PinnedAdapter();
                backend->owner = &task;
            }
            static void destroy(PinnedTask& task) noexcept {
                adapter(task).~PinnedAdapter();
            }

            static void bindThread(Impl& impl, uint32_t threadNum) noexcept {
                if (tlsBoundSystem == &impl) {
                    return;
                }
                tlsBoundSystem = &impl;
                if (impl.profiler != nullptr && threadNum != 0 &&
                    threadNum != enki::NO_THREAD_NUM) {
                    (void)impl.profiler->bindCurrentThreadToWorkerStream(threadNum);
                }
            }

            // Maps a task onto enkiTS chunks of chunkSize_ indices (enkiTS splits
            // and steals whole chunks, so every range but the last holds at least
            // `grain` indices). A background task becomes at most `slots` chunks,
            // so it never runs on more threads than the slots it holds.
            static void configureRanges(TaskSet& task, uint32_t slots) noexcept {
                TaskSetAdapter& backend = adapter(task);
                backend.m_Priority = toEnki(task.priority_);
                task.chunkSize_ = task.grain_;
                if (task.priority_ == TaskPriority::Background) {
                    const uint32_t wanted = ceilDivide(task.count_, task.grain_);
                    const uint32_t chunks = std::max<uint32_t>(1, std::min(wanted, slots));
                    task.chunkSize_ = std::max(task.grain_, ceilDivide(task.count_, chunks));
                }
                const uint32_t chunkCount = ceilDivide(task.count_, task.chunkSize_);
                backend.m_SetSize = chunkCount;
                backend.m_MinRange = 1;
                task.remainingChunks_.store(chunkCount, std::memory_order_relaxed);
            }

            static void launch(Impl& impl, TaskSet& task) {
                task.state_.notify_all(); // wake passive waiters parked on Pending
                if (task.priority_ == TaskPriority::Background) {
                    impl.backgroundAdmitted.fetch_add(1, std::memory_order_relaxed);
                }
                impl.scheduler.AddTaskSetToPipe(&adapter(task));
            }

            static void cancel(Impl& impl, TaskSet& task) noexcept {
                impl.cancelled.fetch_add(1, std::memory_order_relaxed);
                settle(task.state_, StateCancelled);
            }

            static void grantLocked(Impl& impl, TaskSet& task, uint32_t slots,
                uint32_t chunkSlots) noexcept {
                impl.activeSlots += slots;
                impl.peakActiveSlots = std::max(impl.peakActiveSlots, impl.activeSlots);
                task.heldSlots_ = slots;
                configureRanges(task, chunkSlots);
                task.state_.store(StateRunning, std::memory_order_release);
            }

            // Admits pending tasks in FIFO order while slots are free. Returns
            // them as a list (linked through nextPending_) to launch unlocked.
            static TaskSet* admitPendingLocked(Impl& impl) noexcept {
                TaskSet* head = nullptr;
                TaskSet** tail = &head;
                while (impl.pendingHead != nullptr &&
                    impl.activeSlots < impl.backgroundLimit &&
                    !impl.shutdownRequested.load(std::memory_order_relaxed)) {
                    TaskSet* task = impl.pendingHead;
                    impl.pendingHead = task->nextPending_;
                    if (impl.pendingHead == nullptr) {
                        impl.pendingTail = nullptr;
                    }
                    --impl.pendingCount;
                    task->nextPending_ = nullptr;
                    const uint32_t grant = std::min(task->requestedSlots_,
                        impl.backgroundLimit - impl.activeSlots);
                    grantLocked(impl, *task, grant, grant);
                    *tail = task;
                    tail = &task->nextPending_;
                }
                return head;
            }

            static void launchList(Impl& impl, TaskSet* head) {
                while (head != nullptr) {
                    TaskSet* next = head->nextPending_; // read before it can complete
                    head->nextPending_ = nullptr;
                    launch(impl, *head);
                    head = next;
                }
            }

            static void releaseSlots(Impl& impl, uint32_t slots) {
                TaskSet* admitted = nullptr;
                {
                    std::lock_guard lock(impl.gateMutex);
                    impl.activeSlots -= slots;
                    admitted = admitPendingLocked(impl);
                }
                launchList(impl, admitted);
            }

            static void finish(Impl& impl, TaskSet& task) {
                if (task.priority_ == TaskPriority::Background) {
                    const uint32_t held = task.heldSlots_;
                    task.heldSlots_ = 0;
                    releaseSlots(impl, held);
                }
                settle(task.state_, StateDone);
            }

            static void runRange(TaskSet& task, uint32_t first, uint32_t last,
                uint32_t threadNum) {
                Impl& impl = *static_cast<Impl*>(task.system_);
                bindThread(impl, threadNum);
                if (task.priority_ == TaskPriority::FrameCritical &&
                    task.started_.exchange(1, std::memory_order_acq_rel) == 0) {
                    const uint64_t now = nowNanoseconds();
                    const uint64_t latency = now >= task.submitNanoseconds_
                        ? now - task.submitNanoseconds_
                        : 0;
                    impl.frameStartLatencyTotal.fetch_add(latency, std::memory_order_relaxed);
                    atomicMax(impl.frameStartLatencyMax, latency);
                }

                const int previousPriority = tlsCurrentPriority;
                tlsCurrentPriority = static_cast<int>(task.priority_);
                // M7R R5b.3: background chunks run below normal OS priority, so a
                // worker running frame work, the main thread and the driver's
                // threads always preempt cooking (the admission gate bounds how
                // many workers cook; this bounds how they compete for cores).
                const OsPriorityScope osPriority(impl.lowerBackgroundOsPriority,
                    task.priority_, threadNum);
                // Frame-critical work counts as frame allocations wherever it runs.
                const bool frameWork = task.priority_ == TaskPriority::FrameCritical;
                if (frameWork) {
                    ++cpu_allocation_detail::frameScopeDepth;
                }
                const uint32_t begin = static_cast<uint32_t>(
                    static_cast<uint64_t>(first) * task.chunkSize_);
                const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(
                    static_cast<uint64_t>(last) * task.chunkSize_, task.count_));
                try {
                    CpuScope scope(task.scopeName_ != nullptr ? impl.profiler : nullptr,
                        task.scopeName_);
                    task.execute(TaskRange{ begin, end }, threadNum);
                }
                catch (...) {
                    // A failing task never takes down a worker; owners report
                    // failures through their own result channels.
                    impl.exceptions.fetch_add(1, std::memory_order_relaxed);
                }
                if (frameWork) {
                    --cpu_allocation_detail::frameScopeDepth;
                }
                tlsCurrentPriority = previousPriority;

                const uint32_t ran = last - first;
                if (task.remainingChunks_.fetch_sub(ran, std::memory_order_acq_rel) == ran) {
                    finish(impl, task);
                }
            }

            static void runPinned(PinnedTask& task) {
                Impl& impl = *static_cast<Impl*>(task.system_);
                bindThread(impl, impl.scheduler.GetThreadNum());
                const int previousPriority = tlsCurrentPriority;
                tlsCurrentPriority = static_cast<int>(TaskPriority::Background);
                try {
                    CpuScope scope(task.scopeName_ != nullptr ? impl.profiler : nullptr,
                        task.scopeName_);
                    task.execute();
                }
                catch (...) {
                    impl.exceptions.fetch_add(1, std::memory_order_relaxed);
                }
                tlsCurrentPriority = previousPriority;
                settle(task.state_, StateDone);
            }

            static void runIoLoop(TaskSystem& system) {
                Impl& impl = *system.impl_;
                bindThread(impl, impl.ioThread);
                tlsCurrentPriority = static_cast<int>(TaskPriority::Background);
                impl.ioLoopRunning.store(1, std::memory_order_release);
                impl.ioLoopRunning.notify_all();
                while (!impl.scheduler.GetIsShutdownRequested()) {
                    impl.scheduler.WaitForNewPinnedTasks();
                    impl.scheduler.RunPinnedTasks();
                }
                tlsCurrentPriority = -1;
            }

            static void submit(Impl& impl, TaskSet& task) {
                if (!task.isComplete()) {
                    throw std::logic_error("TaskSet submitted while pending or running");
                }
                if (impl.scheduler.GetThreadNum() == enki::NO_THREAD_NUM) {
                    throw std::logic_error("TaskSet submitted from a thread the task system does not run");
                }
                task.system_ = &impl;
                task.started_.store(0, std::memory_order_relaxed);
                task.heldSlots_ = 0;
                task.nextPending_ = nullptr;
                if (task.count_ == 0) {
                    settle(task.state_, StateDone);
                    return;
                }
                if (impl.stopped.load(std::memory_order_acquire)) {
                    cancel(impl, task);
                    return;
                }
                if (task.priority_ != TaskPriority::Background) {
                    if (task.priority_ == TaskPriority::FrameCritical) {
                        impl.frameTaskCount.fetch_add(1, std::memory_order_relaxed);
                        task.submitNanoseconds_ = nowNanoseconds();
                    }
                    configureRanges(task, 0);
                    task.state_.store(StateRunning, std::memory_order_release);
                    impl.scheduler.AddTaskSetToPipe(&adapter(task));
                    return;
                }

                task.requestedSlots_ = std::min(ceilDivide(task.count_, task.grain_),
                    impl.backgroundLimit);
                bool launchNow = false;
                {
                    std::lock_guard lock(impl.gateMutex);
                    if (impl.shutdownRequested.load(std::memory_order_relaxed)) {
                        // Cancelled below, outside the lock.
                    }
                    else if (impl.pendingHead == nullptr &&
                        impl.activeSlots < impl.backgroundLimit) {
                        const uint32_t grant = std::min(task.requestedSlots_,
                            impl.backgroundLimit - impl.activeSlots);
                        grantLocked(impl, task, grant, grant);
                        launchNow = true;
                    }
                    else {
                        task.state_.store(StatePending, std::memory_order_release);
                        if (impl.pendingTail != nullptr) {
                            impl.pendingTail->nextPending_ = &task;
                        }
                        else {
                            impl.pendingHead = &task;
                        }
                        impl.pendingTail = &task;
                        ++impl.pendingCount;
                        return;
                    }
                }
                if (launchNow) {
                    launch(impl, task);
                }
                else {
                    cancel(impl, task); // shutdown requested
                }
            }

            // A background task waiting on a pending background child: admit the
            // child now under the waiter's own slot (plus any free slots), since
            // the waiter's slot is not running its own work while it waits.
            static void admitBorrowed(Impl& impl, TaskSet& task) {
                {
                    std::lock_guard lock(impl.gateMutex);
                    if (task.state_.load(std::memory_order_relaxed) != StatePending) {
                        return;
                    }
                    TaskSet* previous = nullptr;
                    TaskSet* current = impl.pendingHead;
                    while (current != nullptr && current != &task) {
                        previous = current;
                        current = current->nextPending_;
                    }
                    if (current == nullptr) {
                        return;
                    }
                    if (previous != nullptr) {
                        previous->nextPending_ = task.nextPending_;
                    }
                    else {
                        impl.pendingHead = task.nextPending_;
                    }
                    if (impl.pendingTail == &task) {
                        impl.pendingTail = previous;
                    }
                    task.nextPending_ = nullptr;
                    --impl.pendingCount;
                    const uint32_t available = impl.backgroundLimit > impl.activeSlots
                        ? impl.backgroundLimit - impl.activeSlots
                        : 0;
                    const uint32_t extra = std::min(
                        task.requestedSlots_ > 0 ? task.requestedSlots_ - 1 : 0, available);
                    grantLocked(impl, task, extra, extra + 1);
                }
                launch(impl, task);
            }

            static void submitPinned(Impl& impl, PinnedTask& task) {
                if (impl.ioThread == NoThreadIndex) {
                    throw std::logic_error("TaskSystem has no pinned I/O thread");
                }
                if (!task.isComplete()) {
                    throw std::logic_error("PinnedTask submitted while pending or running");
                }
                if (impl.scheduler.GetThreadNum() == enki::NO_THREAD_NUM) {
                    throw std::logic_error("PinnedTask submitted from a thread the task system does not run");
                }
                task.system_ = &impl;
                if (impl.shutdownRequested.load(std::memory_order_acquire)) {
                    impl.cancelled.fetch_add(1, std::memory_order_relaxed);
                    settle(task.state_, StateCancelled);
                    return;
                }
                PinnedAdapter& backend = adapter(task);
                backend.threadNum = impl.ioThread;
                backend.m_Priority = enki::TASK_PRIORITY_LOW;
                task.state_.store(StateRunning, std::memory_order_release);
                impl.scheduler.AddPinnedTask(&backend);
            }

            static void wait(PinnedTask& task) {
                waitForFinalState(task.state_);
                while (!adapter(task).GetIsComplete()) {
                    std::this_thread::yield();
                }
            }

            // Shutdown: stop admission and cancel every pending background task.
            static void cancelPending(Impl& impl) {
                TaskSet* pending = nullptr;
                {
                    std::lock_guard lock(impl.gateMutex);
                    impl.shutdownRequested.store(true, std::memory_order_release);
                    pending = impl.pendingHead;
                    impl.pendingHead = nullptr;
                    impl.pendingTail = nullptr;
                    impl.pendingCount = 0;
                }
                while (pending != nullptr) {
                    TaskSet* next = pending->nextPending_;
                    pending->nextPending_ = nullptr;
                    cancel(impl, *pending);
                    pending = next;
                }
            }

            static void wait(Impl& impl, TaskSet& task) {
                if (task.isComplete()) {
                    return;
                }
                const uint32_t threadNum = impl.scheduler.GetThreadNum();
                if (threadNum != enki::NO_THREAD_NUM && threadNum != impl.ioThread &&
                    !impl.stopped.load(std::memory_order_acquire)) {
                    // The main thread outside tasks counts as FrameCritical.
                    const int caller = tlsCurrentPriority >= 0
                        ? tlsCurrentPriority
                        : static_cast<int>(TaskPriority::FrameCritical);
                    if (static_cast<int>(task.priority_) <= caller) {
                        if (task.state_.load(std::memory_order_acquire) == StatePending) {
                            admitBorrowed(impl, task);
                        }
                        // enkiTS runs tasks of priority >= the caller's while it
                        // waits; it never lowers below the waited task's own.
                        impl.scheduler.WaitforTask(&adapter(task),
                            toEnki(static_cast<TaskPriority>(caller)));
                    }
                }
                waitForFinalState(task.state_);
                while (!adapter(task).GetIsComplete()) {
                    std::this_thread::yield();
                }
            }
        };

        void TaskSetAdapter::ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) {
            TaskSystemAccess::runRange(*owner, range.start, range.end, threadNum);
        }

        void PinnedAdapter::Execute() {
            TaskSystemAccess::runPinned(*owner);
        }

        void IoLoopTask::Execute() {
            TaskSystemAccess::runIoLoop(*system);
        }

    } // namespace detail

    using detail::TaskSystemAccess;

    // --- TaskSet --------------------------------------------------------------------

    TaskSet::TaskSet(TaskPriority priority, uint32_t count, uint32_t grain,
        const char* scopeName) noexcept
        : count_(count), grain_(std::max<uint32_t>(grain, 1)), priority_(priority),
        scopeName_(scopeName) {
        TaskSystemAccess::construct(*this);
    }

    TaskSet::~TaskSet() {
        assert(isComplete() && "TaskSet destroyed while pending or running");
        TaskSystemAccess::destroy(*this);
    }

    void TaskSet::configure(TaskPriority priority, uint32_t count, uint32_t grain,
        const char* scopeName) noexcept {
        assert(isComplete() && "TaskSet reconfigured while pending or running");
        priority_ = priority;
        count_ = count;
        grain_ = std::max<uint32_t>(grain, 1);
        scopeName_ = scopeName;
    }

    bool TaskSet::isComplete() const noexcept {
        return isFinalState(state_.load(std::memory_order_acquire)) &&
            TaskSystemAccess::adapter(*this).GetIsComplete();
    }

    bool TaskSet::wasCancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == StateCancelled;
    }

    // --- PinnedTask -----------------------------------------------------------------

    PinnedTask::PinnedTask(const char* scopeName) noexcept
        : scopeName_(scopeName) {
        TaskSystemAccess::construct(*this);
    }

    PinnedTask::~PinnedTask() {
        assert(isComplete() && "PinnedTask destroyed while pending or running");
        TaskSystemAccess::destroy(*this);
    }

    bool PinnedTask::isComplete() const noexcept {
        return isFinalState(state_.load(std::memory_order_acquire)) &&
            TaskSystemAccess::adapter(*this).GetIsComplete();
    }

    bool PinnedTask::wasCancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == StateCancelled;
    }

    // --- TaskSystem -----------------------------------------------------------------

    TaskSystem::TaskSystem(const TaskSystemConfig& config)
        : impl_(std::make_unique<Impl>()) {
        Impl& impl = *impl_;
        const uint32_t hardwareThreads = enki::GetNumHardwareThreads();
        impl.workerCount = config.workerThreadCount != 0
            ? config.workerThreadCount
            : (hardwareThreads > 3 ? hardwareThreads - 2 : 1);
        impl.backgroundLimit = impl.workerCount > config.reservedFrameWorkers
            ? impl.workerCount - config.reservedFrameWorkers
            : 1;
        impl.backgroundLimit = std::clamp<uint32_t>(impl.backgroundLimit, 1,
            MaxBackgroundSlots);
        impl.lowerBackgroundOsPriority = config.lowerBackgroundOsPriority;

        enki::TaskSchedulerConfig schedulerConfig;
        schedulerConfig.numTaskThreadsToCreate =
            impl.workerCount + (config.pinnedIoThread ? 1u : 0u);
        if (config.allocate != nullptr && config.deallocate != nullptr) {
            impl.allocate = config.allocate;
            impl.deallocate = config.deallocate;
            impl.allocatorUserData = config.allocatorUserData;
            schedulerConfig.customAllocator.alloc = &Impl::allocateBridge;
            schedulerConfig.customAllocator.free = &Impl::deallocateBridge;
            schedulerConfig.customAllocator.userData = &impl;
        }
        impl.threadCount = schedulerConfig.numTaskThreadsToCreate + 1;
        schedulerConfig.profilerCallbacks.threadStart = &onTaskThreadStart;
        liveIoThread.store(config.pinnedIoThread ? impl.threadCount - 1 : NoThreadIndex,
            std::memory_order_release);

        impl.profiler = config.profiler;
        if (impl.profiler != nullptr) {
            // Threads past the profiler's stream capacity record no scopes.
            impl.profiler->prepareWorkerStreams(static_cast<uint32_t>((std::min)(
                static_cast<size_t>(impl.threadCount), CpuProfiler::MaxWorkerStreams)));
        }

        impl.scheduler.Initialize(schedulerConfig);
        TaskSystemAccess::bindThread(impl, 0);
        liveSystem.store(this, std::memory_order_release);

        if (config.pinnedIoThread) {
            // The I/O thread enters its pinned loop before construction returns,
            // so it never picks up task sets afterwards.
            impl.ioThread = impl.threadCount - 1;
            impl.ioLoop.system = this;
            impl.ioLoop.threadNum = impl.ioThread;
            impl.scheduler.AddPinnedTask(&impl.ioLoop);
            while (impl.ioLoopRunning.load(std::memory_order_acquire) == 0) {
                impl.ioLoopRunning.wait(0, std::memory_order_acquire);
            }
        }
    }

    TaskSystem::~TaskSystem() {
        shutdown();
    }

    void TaskSystem::submit(TaskSet& task) {
        TaskSystemAccess::submit(*impl_, task);
    }

    void TaskSystem::wait(TaskSet& task) {
        TaskSystemAccess::wait(*impl_, task);
    }

    void TaskSystem::submitPinnedIo(PinnedTask& task) {
        TaskSystemAccess::submitPinned(*impl_, task);
    }

    void TaskSystem::wait(PinnedTask& task) {
        TaskSystemAccess::wait(task);
    }

    void TaskSystem::shutdown() {
        Impl& impl = *impl_;
        if (impl.stopped.load(std::memory_order_acquire)) {
            return;
        }
        if (impl.scheduler.GetThreadNum() != 0) {
            throw std::logic_error("TaskSystem::shutdown must run on the main thread");
        }
        TaskSystem* self = this;
        (void)liveSystem.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
        TaskSystemAccess::cancelPending(impl);
        // Finishes started work (the main thread helps with every priority here)
        // and stops the workers and the I/O loop.
        impl.scheduler.WaitforAllAndShutdown();
        impl.stopped.store(true, std::memory_order_release);
        liveIoThread.store(NoThreadIndex, std::memory_order_release);
        tlsBoundSystem = nullptr;
    }

    TaskSystem* TaskSystem::current() noexcept {
        return liveSystem.load(std::memory_order_acquire);
    }

    TaskSystem* TaskSystem::forCurrentThread() noexcept {
        TaskSystem* system = current();
        if (system == nullptr || system->isShutdownRequested() ||
            system->currentThreadIndex() == NoThreadIndex) {
            return nullptr;
        }
        return system;
    }

    void TaskSystem::tickPeriodic() noexcept {
        Impl& impl = *impl_;
        const uint64_t now = nowNanoseconds();
        std::lock_guard lock(impl.periodicMutex);
        for (Periodic* periodic = impl.periodicHead; periodic != nullptr;
            periodic = periodic->next_) {
            try {
                periodic->tick(now);
            }
            catch (...) {
                impl.exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool TaskSystem::isShutdownRequested() const noexcept {
        return impl_->shutdownRequested.load(std::memory_order_acquire);
    }

    uint32_t TaskSystem::workerThreadCount() const noexcept {
        return impl_->workerCount;
    }

    uint32_t TaskSystem::threadCount() const noexcept {
        return impl_->threadCount;
    }

    uint32_t TaskSystem::pinnedIoThreadIndex() const noexcept {
        return impl_->ioThread;
    }

    uint32_t TaskSystem::backgroundSlotLimit() const noexcept {
        return impl_->backgroundLimit;
    }

    uint32_t TaskSystem::currentThreadIndex() const noexcept {
        const uint32_t threadNum = impl_->scheduler.GetThreadNum();
        return threadNum == enki::NO_THREAD_NUM ? NoThreadIndex : threadNum;
    }

    TaskSystemStatistics TaskSystem::takeStatistics() noexcept {
        Impl& impl = *impl_;
        TaskSystemStatistics statistics{};
        statistics.frameTaskCount = impl.frameTaskCount.exchange(0, std::memory_order_relaxed);
        statistics.frameStartLatencyMaxNs =
            impl.frameStartLatencyMax.exchange(0, std::memory_order_relaxed);
        statistics.frameStartLatencyTotalNs =
            impl.frameStartLatencyTotal.exchange(0, std::memory_order_relaxed);
        statistics.backgroundAdmittedCount =
            impl.backgroundAdmitted.exchange(0, std::memory_order_relaxed);
        statistics.cancelledCount = impl.cancelled.exchange(0, std::memory_order_relaxed);
        statistics.exceptionCount = impl.exceptions.exchange(0, std::memory_order_relaxed);
        std::lock_guard lock(impl.gateMutex);
        statistics.backgroundActiveSlots = impl.activeSlots;
        statistics.backgroundPendingCount = impl.pendingCount;
        statistics.backgroundPeakActiveSlots = impl.peakActiveSlots;
        impl.peakActiveSlots = impl.activeSlots;
        return statistics;
    }

    void TaskSystem::recordFrameCounters(CpuProfiler& profiler) noexcept {
        const TaskSystemStatistics statistics = takeStatistics();
        profiler.recordCounter("task.frame.count", statistics.frameTaskCount);
        profiler.recordCounter("task.frame.start_latency_us",
            statistics.frameStartLatencyMaxNs / 1000);
        profiler.recordCounter("task.background.active",
            statistics.backgroundActiveSlots);
    }

    // --- Strand ---------------------------------------------------------------------

    bool StrandItem::isComplete() const noexcept {
        return isFinalState(state_.load(std::memory_order_acquire));
    }

    bool StrandItem::wasCancelled() const noexcept {
        return state_.load(std::memory_order_acquire) == StateCancelled;
    }

    void StrandItem::waitPassively() const noexcept {
        waitForFinalState(state_);
    }

    Strand::Strand(TaskSystem& system, TaskPriority priority,
        const char* scopeName) noexcept
        : system_(system), pump_(*this, priority, scopeName) {}

    Strand::~Strand() {
        waitIdle();
    }

    bool Strand::post(StrandItem& item) {
        item.next_ = nullptr;
        if (system_.isShutdownRequested()) {
            settle(item.state_, StateCancelled);
            return false;
        }
        bool schedule = false;
        {
            std::lock_guard lock(mutex_);
            item.state_.store(StatePending, std::memory_order_release);
            if (tail_ != nullptr) {
                tail_->next_ = &item;
            }
            else {
                head_ = &item;
            }
            tail_ = &item;
            if (!scheduled_) {
                scheduled_ = true;
                schedule = true;
            }
        }
        if (!schedule) {
            return true;
        }
        // The previous drain cleared scheduled_ and may still be returning from
        // its range; the pump becomes complete within that window.
        while (!pump_.isComplete()) {
            std::this_thread::yield();
        }
        system_.submit(pump_);
        if (pump_.wasCancelled()) {
            std::lock_guard lock(mutex_);
            cancelQueuedLocked();
            scheduled_ = false;
            return false;
        }
        return true;
    }

    void Strand::Pump::execute(TaskRange, uint32_t) {
        owner_.drain();
    }

    void Strand::drain() {
        for (;;) {
            StrandItem* item = nullptr;
            {
                std::lock_guard lock(mutex_);
                item = head_;
                if (item == nullptr) {
                    scheduled_ = false;
                    return;
                }
                head_ = item->next_;
                if (head_ == nullptr) {
                    tail_ = nullptr;
                }
            }
            item->next_ = nullptr;
            if (system_.isShutdownRequested()) {
                settle(item->state_, StateCancelled);
                continue;
            }
            item->state_.store(StateRunning, std::memory_order_release);
            try {
                item->run();
            }
            catch (...) {
                // The strand keeps draining; the item reports its own failure.
            }
            settle(item->state_, StateDone);
        }
    }

    void Strand::cancelQueuedLocked() noexcept {
        StrandItem* item = head_;
        head_ = nullptr;
        tail_ = nullptr;
        while (item != nullptr) {
            StrandItem* next = item->next_;
            item->next_ = nullptr;
            settle(item->state_, StateCancelled);
            item = next;
        }
    }

    void Strand::waitIdle() {
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (!scheduled_) {
                    break;
                }
                if (pump_.wasCancelled()) {
                    // The pump was still waiting for admission at shutdown.
                    cancelQueuedLocked();
                    scheduled_ = false;
                    break;
                }
            }
            if (!pump_.isComplete()) {
                system_.wait(pump_);
            }
            else {
                std::this_thread::yield();
            }
        }
        while (!pump_.isComplete()) {
            std::this_thread::yield();
        }
    }

    bool Strand::isIdle() const {
        std::lock_guard lock(mutex_);
        return !scheduled_;
    }

    // --- Periodic (M7R R5b.2) -------------------------------------------------------

    Periodic::Periodic(TaskSystem& system, std::chrono::nanoseconds interval,
        Target target, std::function<void()> work, const char* scopeName)
        : system_(system),
          intervalNanoseconds_(static_cast<uint64_t>((std::max)(
              interval.count(), std::chrono::nanoseconds::rep{ 0 }))),
          target_(target),
          work_(std::move(work)),
          setRun_(*this, target == Target::Normal
              ? TaskPriority::Normal
              : TaskPriority::Background, scopeName),
          pinnedRun_(*this, scopeName) {
        if (!work_) {
            throw std::invalid_argument("Periodic work must be callable.");
        }
        if (target_ == Target::PinnedIo &&
            system_.pinnedIoThreadIndex() == NoThreadIndex) {
            throw std::logic_error("A pinned Periodic needs the pinned I/O thread.");
        }
        // The first run starts on the first tick.
        TaskSystem::Impl& impl = *system_.impl_;
        std::lock_guard lock(impl.periodicMutex);
        next_ = impl.periodicHead;
        impl.periodicHead = this;
        registered_ = true;
    }

    Periodic::~Periodic() {
        stop();
    }

    void Periodic::stop() {
        {
            TaskSystem::Impl& impl = *system_.impl_;
            std::lock_guard lock(impl.periodicMutex);
            if (registered_) {
                Periodic** link = &impl.periodicHead;
                while (*link != nullptr && *link != this) {
                    link = &(*link)->next_;
                }
                if (*link == this) {
                    *link = next_;
                }
                next_ = nullptr;
                registered_ = false;
            }
        }
        // No tick submits after the unlink; wait for a run in flight.
        if (target_ == Target::PinnedIo) {
            if (!pinnedRun_.isComplete()) {
                system_.wait(pinnedRun_);
            }
        }
        else if (!setRun_.isComplete()) {
            system_.wait(setRun_);
        }
    }

    bool Periodic::idle() const noexcept {
        return target_ == Target::PinnedIo
            ? pinnedRun_.isComplete()
            : setRun_.isComplete();
    }

    void Periodic::tick(uint64_t nowNanoseconds) {
        if (nowNanoseconds < nextDueNanoseconds_ || !idle()) {
            return;
        }
        nextDueNanoseconds_ = nowNanoseconds + intervalNanoseconds_;
        if (target_ == Target::PinnedIo) {
            system_.submitPinnedIo(pinnedRun_);
        }
        else {
            system_.submit(setRun_);
        }
    }

    void Periodic::run() {
        runs_.fetch_add(1, std::memory_order_relaxed);
        work_();
    }

    void Periodic::SetRun::execute(TaskRange, uint32_t) {
        owner_.run();
    }

    void Periodic::PinnedRun::execute() {
        owner_.run();
    }

    // --- FunctionStrand (M7R R5b.2) -------------------------------------------------

    struct FunctionStrand::Item final : StrandItem {
        explicit Item(std::function<void()> callable) : work(std::move(callable)) {}
        std::function<void()> work;

    protected:
        void run() override {
            work();
        }
    };

    FunctionStrand::FunctionStrand(TaskSystem& system, TaskPriority priority,
        const char* scopeName)
        : system_(system),
          strand_(std::make_unique<Strand>(system, priority, scopeName)) {}

    FunctionStrand::~FunctionStrand() {
        waitIdle();
    }

    bool FunctionStrand::post(std::function<void()> work) {
        auto item = std::make_unique<Item>(std::move(work));
        // Posted before it is listed: an item is reclaimed only once it reached a
        // final state, and a fresh item already reads as final (idle).
        const bool posted = strand_->post(*item);
        std::lock_guard lock(mutex_);
        reclaimLocked();
        items_.push_back(std::move(item));
        return posted;
    }

    void FunctionStrand::waitIdle() {
        strand_->waitIdle();
        std::lock_guard lock(mutex_);
        reclaimLocked();
    }

    void FunctionStrand::reclaimLocked() {
        std::erase_if(items_, [](const std::unique_ptr<Item>& item) {
            return item->isComplete();
        });
    }

} // namespace Iridium::Tasks
