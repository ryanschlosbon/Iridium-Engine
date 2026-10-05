#pragma once

// Engine task system (M7R R5b, ADR-0015). The only owner of worker threads. It runs
// on enkiTS, whose headers stay inside TaskSystem.cpp; this API is engine types.
//
// Model:
//   - Three priorities: FrameCritical (work the main thread joins this frame),
//     Normal (editor responsiveness) and Background (cooking, thumbnails, hashing).
//   - Task objects are caller-owned (TaskSet, PinnedTask, StrandItem); the object
//     is its own handle. Submitting, running and waiting allocate nothing.
//   - The main thread joins only frame-critical work: wait() from the main thread
//     helps run FrameCritical tasks and otherwise blocks without running work.
//     In general a waiter helps only with tasks at or above its own priority.
//   - Background admission gate: at most backgroundSlotLimit() threads run
//     background work at once (workers minus reservedFrameWorkers), so the
//     reserved workers are always free to pick up frame work.
//   - An optional pinned I/O thread runs PinnedTasks (blocking file and SQLite
//     I/O) and never runs task sets.
//   - Worker threads record CpuScopes into per-worker profiler streams.
//
// Threads: construct, shut down and destroy the TaskSystem on the main thread.
// submit() may be called from the main thread, from inside any task, or from the
// I/O thread; wait() and the passive waits work from any thread.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace Iridium {
    class CpuProfiler;
}

namespace Iridium::Tasks {

    enum class TaskPriority : uint8_t {
        FrameCritical = 0,
        Normal = 1,
        Background = 2,
    };

    inline constexpr uint32_t TaskPriorityCount = 3;
    inline constexpr uint32_t NoThreadIndex = 0xFFFF'FFFFu;

    // A half-open index range [begin, end) of a TaskSet's count.
    struct TaskRange {
        uint32_t begin = 0;
        uint32_t end = 0;
    };

    namespace detail {
        struct TaskSystemAccess;
        inline constexpr size_t TaskBackendStorageSize = 96;
    }

    // Caller-owned parallel work: execute() is called with disjoint ranges that
    // together cover [0, count). A TaskSet is reusable once complete and must be
    // complete when destroyed. While it is pending or running, configure() and
    // the destructor are not allowed.
    class TaskSet {
    public:
        explicit TaskSet(TaskPriority priority = TaskPriority::Normal,
            uint32_t count = 1, uint32_t grain = 1,
            const char* scopeName = nullptr) noexcept;
        virtual ~TaskSet();

        TaskSet(const TaskSet&) = delete;
        TaskSet& operator=(const TaskSet&) = delete;
        TaskSet(TaskSet&&) = delete;
        TaskSet& operator=(TaskSet&&) = delete;

        // grain is the smallest range execute() receives (except the last one).
        // scopeName (static lifetime, or null for none) names a CpuScope around
        // every execute() call.
        void configure(TaskPriority priority, uint32_t count, uint32_t grain = 1,
            const char* scopeName = nullptr) noexcept;

        [[nodiscard]] TaskPriority priority() const noexcept { return priority_; }
        [[nodiscard]] uint32_t count() const noexcept { return count_; }
        [[nodiscard]] uint32_t grain() const noexcept { return grain_; }
        [[nodiscard]] bool isComplete() const noexcept;
        // True when the last submission was cancelled (shutdown) before it ran.
        [[nodiscard]] bool wasCancelled() const noexcept;

    protected:
        virtual void execute(TaskRange range, uint32_t threadIndex) = 0;

    private:
        friend struct detail::TaskSystemAccess;

        std::atomic<uint32_t> state_{ 0 };
        std::atomic<uint32_t> remainingChunks_{ 0 };
        std::atomic<uint32_t> started_{ 0 };
        uint32_t count_ = 1;
        uint32_t grain_ = 1;
        uint32_t chunkSize_ = 1;
        uint32_t requestedSlots_ = 0;
        uint32_t heldSlots_ = 0;
        TaskPriority priority_ = TaskPriority::Normal;
        const char* scopeName_ = nullptr;
        uint64_t submitNanoseconds_ = 0;
        TaskSet* nextPending_ = nullptr;
        void* system_ = nullptr;
        alignas(16) unsigned char backend_[detail::TaskBackendStorageSize];
    };

    // A TaskSet over a callable `void(TaskRange, uint32_t threadIndex)`.
    template <class Fn>
    class FunctionTaskSet final : public TaskSet {
    public:
        FunctionTaskSet(TaskPriority priority, uint32_t count, uint32_t grain, Fn fn,
            const char* scopeName = nullptr)
            : TaskSet(priority, count, grain, scopeName), fn_(std::move(fn)) {}

    private:
        void execute(TaskRange range, uint32_t threadIndex) override {
            fn_(range, threadIndex);
        }

        Fn fn_;
    };

    // Caller-owned work for the pinned I/O thread. Reusable once complete.
    class PinnedTask {
    public:
        explicit PinnedTask(const char* scopeName = nullptr) noexcept;
        virtual ~PinnedTask();

        PinnedTask(const PinnedTask&) = delete;
        PinnedTask& operator=(const PinnedTask&) = delete;
        PinnedTask(PinnedTask&&) = delete;
        PinnedTask& operator=(PinnedTask&&) = delete;

        [[nodiscard]] bool isComplete() const noexcept;
        [[nodiscard]] bool wasCancelled() const noexcept;

    protected:
        virtual void execute() = 0;

    private:
        friend struct detail::TaskSystemAccess;

        std::atomic<uint32_t> state_{ 0 };
        const char* scopeName_ = nullptr;
        void* system_ = nullptr;
        alignas(16) unsigned char backend_[detail::TaskBackendStorageSize];
    };

    struct TaskSystemConfig {
        // Worker threads that run task sets. 0 selects hardware threads - 2 (one
        // hardware thread for the main thread, one for the I/O thread), min 1.
        uint32_t workerThreadCount = 0;
        // R in ADR-0015: workers kept free of background work. The background
        // slot limit is workerThreadCount - R, at least 1.
        uint32_t reservedFrameWorkers = 8;
        bool pinnedIoThread = true;
        // M7R R5b.3: workers run background chunks below normal OS priority
        // (Windows), so frame work, the main thread and driver threads preempt
        // cooking. The starvation test measures with it on.
        bool lowerBackgroundOsPriority = true;
        // Optional: per-worker CpuScope streams. Must outlive the TaskSystem and
        // must not be inside a frame when the TaskSystem is constructed.
        CpuProfiler* profiler = nullptr;
        // Optional allocator for enkiTS's own memory (all of it is allocated at
        // construction). Null uses enkiTS's default aligned malloc/free.
        void* (*allocate)(size_t alignment, size_t size, void* userData) = nullptr;
        void (*deallocate)(void* memory, size_t size, void* userData) = nullptr;
        void* allocatorUserData = nullptr;
    };

    struct TaskSystemStatistics {
        // Since the previous takeStatistics() call.
        uint64_t frameTaskCount = 0;           // task.frame.count
        uint64_t frameStartLatencyMaxNs = 0;   // submit -> first range starts
        uint64_t frameStartLatencyTotalNs = 0;
        uint64_t backgroundAdmittedCount = 0;
        uint32_t backgroundPeakActiveSlots = 0;
        uint64_t cancelledCount = 0;
        uint64_t exceptionCount = 0;           // execute() threw (task completes)
        // Snapshots at the call.
        uint32_t backgroundActiveSlots = 0;    // task.background.active
        uint32_t backgroundPendingCount = 0;
    };

    class TaskSystem final {
    public:
        explicit TaskSystem(const TaskSystemConfig& config = {});
        ~TaskSystem();

        TaskSystem(const TaskSystem&) = delete;
        TaskSystem& operator=(const TaskSystem&) = delete;

        // Starts the task. Background tasks pass the admission gate: they start
        // when a slot is free, in FIFO order, and are cancelled if the system
        // shuts down first. After shutdown every submission is cancelled.
        void submit(TaskSet& task);
        // Returns once the task is complete (or cancelled). A waiter helps run
        // tasks at or above its own priority (the main thread counts as
        // FrameCritical); otherwise it blocks without running work. A
        // background task waiting on a pending background child admits it under
        // its own slot, so fork-join inside background work cannot deadlock.
        void wait(TaskSet& task);

        // submit + wait for a callable `void(TaskRange, uint32_t threadIndex)`.
        template <class Fn>
        void parallelFor(TaskPriority priority, uint32_t count, uint32_t grain,
            Fn&& fn, const char* scopeName = nullptr) {
            using Callable = std::remove_reference_t<Fn>;
            struct Reference {
                Callable* fn;
                void operator()(TaskRange range, uint32_t threadIndex) const {
                    (*fn)(range, threadIndex);
                }
            };
            FunctionTaskSet<Reference> task(priority, count, grain,
                Reference{ &fn }, scopeName);
            submit(task);
            wait(task);
        }

        // Runs the task on the pinned I/O thread (FIFO). Requires pinnedIoThread.
        void submitPinnedIo(PinnedTask& task);
        void wait(PinnedTask& task);

        // M7R R5b.2: runs `fn()` on the pinned I/O thread and waits for it
        // (blocking, without running other work). Returns false when it was
        // cancelled by shutdown before it ran. Exceptions from fn are caught
        // by the task system; callers that need them capture them in fn.
        template <class Fn>
        bool runOnPinnedIo(Fn&& fn, const char* scopeName = nullptr) {
            using Callable = std::remove_reference_t<Fn>;
            class Task final : public PinnedTask {
            public:
                Task(Callable& callable, const char* name)
                    : PinnedTask(name), callable_(callable) {}

            private:
                void execute() override { callable_(); }
                Callable& callable_;
            };
            Task task(fn, scopeName);
            submitPinnedIo(task);
            wait(task);
            return !task.wasCancelled();
        }

        // Stops admission, cancels pending background tasks, finishes everything
        // already started (the main thread helps) and joins every thread.
        // Long-running tasks should poll isShutdownRequested(). Idempotent.
        void shutdown();
        [[nodiscard]] bool isShutdownRequested() const noexcept;

        [[nodiscard]] uint32_t workerThreadCount() const noexcept;
        // Main thread (index 0) + workers + the I/O thread.
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t pinnedIoThreadIndex() const noexcept;
        [[nodiscard]] uint32_t backgroundSlotLimit() const noexcept;
        // This thread's index (0 = main), or NoThreadIndex for foreign threads.
        [[nodiscard]] uint32_t currentThreadIndex() const noexcept;

        [[nodiscard]] TaskSystemStatistics takeStatistics() noexcept;
        // Records task.frame.count, task.frame.start_latency_us (max) and
        // task.background.active into the open profiler frame, resetting the
        // since-last statistics. Main thread.
        void recordFrameCounters(CpuProfiler& profiler) noexcept;

        // M7R R5b.2: runs every registered Periodic whose interval elapsed and
        // whose previous run completed. The frame orchestrator calls it once per
        // frame on the main thread; it allocates nothing.
        void tickPeriodic() noexcept;

        // The live TaskSystem (ADR-0015: at most one exists at a time), or null.
        [[nodiscard]] static TaskSystem* current() noexcept;
        // current(), when the calling thread is one it runs (main, worker or
        // I/O) and it is not shutting down; otherwise null. Library kernels use
        // it to run parallel and fall back to a serial loop without one.
        [[nodiscard]] static TaskSystem* forCurrentThread() noexcept;

    private:
        friend struct detail::TaskSystemAccess;
        friend class Periodic;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // M7R R5b.2: work that the frame tick (TaskSystem::tickPeriodic) starts again
    // once `interval` has passed since its previous start and that run completed.
    // Runs never overlap. Target selects a Background or Normal task set, or the
    // pinned I/O thread. Construct and stop on the main thread.
    class Periodic final {
    public:
        enum class Target : uint8_t {
            Background,
            Normal,
            PinnedIo,
        };

        Periodic(TaskSystem& system, std::chrono::nanoseconds interval, Target target,
            std::function<void()> work, const char* scopeName = nullptr);
        // Calls stop().
        ~Periodic();

        Periodic(const Periodic&) = delete;
        Periodic& operator=(const Periodic&) = delete;

        // Unregisters from the tick and waits for a run in flight. Idempotent.
        void stop();
        [[nodiscard]] uint64_t runCount() const noexcept {
            return runs_.load(std::memory_order_relaxed);
        }

    private:
        friend class TaskSystem;

        class SetRun final : public TaskSet {
        public:
            SetRun(Periodic& owner, TaskPriority priority, const char* scopeName) noexcept
                : TaskSet(priority, 1, 1, scopeName), owner_(owner) {}

        private:
            void execute(TaskRange range, uint32_t threadIndex) override;
            Periodic& owner_;
        };

        class PinnedRun final : public PinnedTask {
        public:
            PinnedRun(Periodic& owner, const char* scopeName) noexcept
                : PinnedTask(scopeName), owner_(owner) {}

        private:
            void execute() override;
            Periodic& owner_;
        };

        void tick(uint64_t nowNanoseconds);
        [[nodiscard]] bool idle() const noexcept;
        void run();

        TaskSystem& system_;
        uint64_t intervalNanoseconds_ = 0;
        uint64_t nextDueNanoseconds_ = 0;
        Target target_ = Target::Background;
        std::function<void()> work_;
        std::atomic<uint64_t> runs_{ 0 };
        Periodic* next_ = nullptr;
        bool registered_ = false;
        SetRun setRun_;
        PinnedRun pinnedRun_;
    };

    class Strand;

    // M7R R5b.2: posts callables to a Strand. Each post allocates one item (this
    // is service work, never frame work); completed items are reclaimed on later
    // posts. Items run one at a time in post order at the strand's priority.
    class FunctionStrand final {
    public:
        FunctionStrand(TaskSystem& system, TaskPriority priority,
            const char* scopeName = nullptr);
        // Waits for posted work (it is cancelled if the system is shutting down).
        ~FunctionStrand();

        FunctionStrand(const FunctionStrand&) = delete;
        FunctionStrand& operator=(const FunctionStrand&) = delete;

        // Returns false (the callable never runs) after the task system began
        // shutting down. Callable from any thread the task system runs.
        bool post(std::function<void()> work);
        // Blocks (without running work) until everything posted ran or was
        // cancelled.
        void waitIdle();
        [[nodiscard]] TaskSystem& system() const noexcept { return system_; }

    private:
        struct Item;
        void reclaimLocked();

        TaskSystem& system_;
        std::unique_ptr<Strand> strand_;
        std::mutex mutex_;
        std::vector<std::unique_ptr<Item>> items_;
    };

    // A serial queue: items run one at a time, in post order, at the strand's
    // priority (a Background strand holds one admission slot while it drains).
    class StrandItem {
    public:
        StrandItem() = default;
        virtual ~StrandItem() = default;

        StrandItem(const StrandItem&) = delete;
        StrandItem& operator=(const StrandItem&) = delete;

        [[nodiscard]] bool isComplete() const noexcept;
        [[nodiscard]] bool wasCancelled() const noexcept;
        // Blocks (without running work) until the item ran or was cancelled.
        void waitPassively() const noexcept;

    protected:
        virtual void run() = 0;

    private:
        friend class Strand;
        StrandItem* next_ = nullptr;
        std::atomic<uint32_t> state_{ 0 };
    };

    class Strand final {
    public:
        Strand(TaskSystem& system, TaskPriority priority,
            const char* scopeName = nullptr) noexcept;
        // Waits for queued items to drain (they are cancelled if the system is
        // shutting down).
        ~Strand();

        Strand(const Strand&) = delete;
        Strand& operator=(const Strand&) = delete;

        // Queues the item. Returns false (item cancelled) after shutdown. Items
        // must stay alive until complete.
        bool post(StrandItem& item);
        // Blocks until every posted item ran or was cancelled.
        void waitIdle();
        [[nodiscard]] bool isIdle() const;

    private:
        class Pump final : public TaskSet {
        public:
            Pump(Strand& owner, TaskPriority priority, const char* scopeName) noexcept
                : TaskSet(priority, 1, 1, scopeName), owner_(owner) {}

        private:
            void execute(TaskRange range, uint32_t threadIndex) override;
            Strand& owner_;
        };

        void drain();
        void cancelQueuedLocked() noexcept;

        TaskSystem& system_;
        mutable std::mutex mutex_;
        StrandItem* head_ = nullptr;
        StrandItem* tail_ = nullptr;
        bool scheduled_ = false;
        Pump pump_;
    };

} // namespace Iridium::Tasks
