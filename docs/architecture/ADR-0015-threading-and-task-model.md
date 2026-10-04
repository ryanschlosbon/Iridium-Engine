# ADR-0015: Threading and Task Model

- Status: Accepted 2026-10-03 (M7R R5b.1). The task system, its tests and the
  thread-scoped allocation counters are in place. R5b.2 moved every service thread,
  the importer fork-join, the menu-bar `std::async` and the convolution's
  `std::execution::par` onto it; R5b.3 added the starvation test (its timing run is
  pending). As-built notes are in the M7R plan's decision log (2026-10-03, lane B).
- Date: 2026-10-03
- Owners: Core, Profiling, Assets (service migration)
- Library: enkiTS v1.12 (zlib, commit `0289cf6`), approved by the owner 2026-10-03.

## Context

The M7R R5 audit (`docs/milestones/M7R-R5-cpu-frame-design.md` §1.2) found 10
classes that own a persistent `std::jthread` (up to 11 live instances), a transient
`std::jthread` fork-join in the glTF importer, a `std::async` in the editor menu
bar and `std::execution::par` in the environment convolution. Each has its own
deque, mutex and condition variable, or a polling loop. None is named, none emits
profiler scopes, and none has a priority relation to the frame: cooking runs at
normal OS priority next to rendering.

R5c parallelizes transform update, extraction and sorting. That needs worker
threads the main thread can join within a frame, and background work must not
delay them. The allocation invariant (zero steady-frame C++ allocations) must keep
measuring the frame, not whatever service threads happen to allocate.

enkiTS v1.12 and Taskflow 4.1.0 were compared (design §4.1). enkiTS is the only one
of the two that provides all three hard requirements in a release: task priorities,
pinned tasks, and no allocation after initialization.

## Decision

1. **One task system.** `Iridium::Tasks::TaskSystem` (`src/core/tasks`, library
   `iridium_tasks`) is the only owner of worker threads. enkiTS is linked privately;
   its headers appear only in `TaskSystem.cpp`. The public API is engine types:
   - `TaskSet`: caller-owned parallel work over `[0, count)` in ranges of at least
     `grain` indices (except the last). The object is its own handle (the design
     draft's `TaskHandle`). `FunctionTaskSet` wraps a callable.
   - `TaskSystem::parallelFor`: submit plus wait for a callable (the design's
     `ParallelFor`).
   - `PinnedTask` with `submitPinnedIo`: work on the pinned I/O thread (the
     design's `PinnedIo`).
   - `Strand` and `StrandItem`: a serial FIFO queue at one priority.
   - `Periodic` is deferred to R5b.2. It runs on the frame orchestrator's tick, which
     R5a introduces.

   Submitting, running and waiting allocate nothing. Task objects are caller-owned
   and the enkiTS task object lives inside them (fixed storage, size checked at
   compile time). The engine never moves `enki::Dependency` objects, which is why
   v1.12 suffices (design decision 1).

2. **Three priorities, mapped to enkiTS HIGH/MED/LOW:**
   - `FrameCritical`: extraction, sorts and parallel-for work that the main thread
     joins this frame.
   - `Normal`: editor responsiveness, such as material-preview compile, menu-bar
     scans and upload preparation.
   - `Background`: cooking, thumbnails, hashing, catalog rebuilds and baking.

3. **Waiting rule.** A waiter helps run tasks only at or above its own priority.
   The main thread counts as `FrameCritical`, so it joins frame work with
   `WaitforTask(task, HIGH)` and never runs a `Normal` or `Background` task. When
   the waited task is lower than the waiter's priority, the waiter blocks
   (`std::atomic::wait`) without running work. enkiTS raises the filter to the
   waited task's priority, so a main-thread wait on a lower-priority task never
   reaches `WaitforTask`. The I/O thread never helps.

4. **Threads.** The default is hardware threads − 2 workers: one hardware thread is
   left for the main thread and one for the I/O thread. On the i9-14900K that is 30
   workers.
   - **The pinned I/O thread** is the last enkiTS thread. It enters a
     `WaitForNewPinnedTasks`/`RunPinnedTasks` loop before the constructor returns,
     so it runs only `PinnedTask`s, in FIFO order, and never runs a task set. It is
     for blocking file and SQLite I/O.
   - **Per-core affinity is not set.** R5b.3 measures it on the hybrid CPU, with
     P/E-class recording (`GetLogicalProcessorInformationEx`) and per-worker
     `CoInitializeEx(MTA)` in a thread-start hook (it replaces the
     `WicFactoryLifetime` thread). Both arrive with their first consumers.
   - **Construction and shutdown happen on the main thread**, which becomes enkiTS
     thread 0. Only one `TaskSystem` exists at a time.

5. **Background admission gate (starvation bound).** At most
   `workers − reservedFrameWorkers` threads run background work at once (R = 8 by
   default, the P-core count; at least 1).
   - **Slots.** A background task holds slots. A background task set is split into
     at most as many chunks as its slots, so it can never occupy more threads than
     it holds.
   - **Admission order.** Tasks are admitted in FIFO order when slots free up. Each
     gets as many free slots as it asked for, up to `ceil(count / grain)`.
   - **Fork-join inside background work.** A background task that waits on a
     still-pending background child admits the child under its own slot, plus any
     free slots. The waiting slot is not running its own work, so the bound holds
     and nested fork-join cannot deadlock behind the gate.
   - **Splitting.** Background work is split into tasks of at most about 5 ms where
     the code allows (per texture, per mip chain).
   - **The bound.** A frame task waits for a thread only when all R reserved workers
     are busy with frame or normal work, and then at most for one background chunk's
     remaining time.
   - **Pipe capacity.** The bound also keeps background chunks far below enkiTS's
     per-thread pipe capacity (256). A full pipe would make the submitting thread
     run the work inline, and for the main thread that must never be background
     work.

6. **Shutdown.** `shutdown()` stops admission and cancels pending background tasks,
   strand items queued behind them, and any later submission. Cancelled tasks report
   `wasCancelled()`. It finishes started work, with the main thread helping at every
   priority because this is shutdown, and joins every thread. Long-running tasks
   poll `isShutdownRequested()`.

7. **Failures.** An exception from a task is caught at the range boundary and
   counted (`TaskSystemStatistics::exceptionCount`); the task still completes. It
   never takes down a worker. Services report failures through their existing
   result queues.

8. **Telemetry.**
   - **Per-worker scope streams.** `CpuProfiler` gains per-worker streams (R5b.1).
     A task-system thread records its `CpuScope`s into its own single-producer
     ring. Worker scopes never hold the frame open and may span frames. `endFrame`
     merges them into `CpuFrameProfile::workerEvents`, and their per-name sums
     become `ProfileRunStatistics::workerRanges`: the aggregate worker time per
     stage. Main-thread scopes stay in the shared table and are the critical path.
     The JSON export adds `worker_events` and `worker_ranges` only when streams were
     prepared, so profiles of runs without a task system are unchanged.
   - **Counters.** `TaskSystem::recordFrameCounters` writes `task.frame.count`,
     `task.frame.start_latency_us` (the maximum time from enqueue to start) and
     `task.background.active`. The orchestrator calls it from R5b.2.
   - **Tracy** is not adopted in R5 (owner decision 2).

9. **Thread-scoped allocation counters.** `allocation.cpp.*` counts the thread that
   called `beginCpuAllocationFrame` (the main thread), plus any thread while it runs
   a `FrameCritical` task (`CpuAllocationFrameScope`). Every other thread is counted
   separately, in `CpuAllocationFrameSample::backgroundAllocationCount` and
   `backgroundRequestedBytes`. Frame-critical work stays inside the zero-allocation
   invariant wherever it runs, and background work cannot pollute it. The
   main-thread figures are unchanged. Recording `allocation.cpp.background.*` into
   profiles is an `src/app` change and lands with R5b.2.

## Consequences

- **No other threading primitives.** Outside `iridium_tasks`, no `std::thread`,
  `std::async` or `std::execution::par` is used once R5b.2 is complete. Until then
  the existing service threads remain, and their allocations already count as
  background.
- **Services keep their public APIs.** Only their execution backing changes:

  | Service | New form |
  |---|---|
  | 1 Catalog | `Strand` (Background) for imports and rebuilds; SQLite transaction on the I/O thread; results unchanged |
  | 2 Model preparation | `Strand`; the DDC future poll becomes a continuation task |
  | 3 Environment preparation | `Strand`; `.get()` becomes a continuation |
  | 4 Thumbnails | `Strand` (Background; was BELOW_NORMAL) |
  | 5/6 DDC | each cook key is a Background task (de-duplication map kept); the importer fork-join becomes a `parallelFor` over texture-view jobs, joined under the gate's nested rule; the convolution's `par` becomes a `parallelFor` |
  | 7 Reimport | `Strand` with the per-item `stop_source` kept |
  | 8 Source monitor | `Periodic` (10 ms debounce tick) on the orchestrator's frame tick; SHA-256 hashing as a Background task outside the monitor mutex |
  | 9 Watcher | `Periodic` 250 ms `stat` scan on the I/O thread, outside the mutex |
  | 10 Material preview | Normal single-slot task, latest wins |
  | 11 WIC lifetime | removed: every worker initializes the MTA at thread start (or the thread is kept as the documented exception if WIC needs a dedicated owner) |
  | Menu-bar `std::async` | Normal task plus a completion flag; shutdown cancels it instead of blocking |

- **Rules for submitters:**
  - Only threads the task system runs (main, workers, I/O) may submit.
  - A task must stay alive and unmoved until complete.
  - Waiting on a task is allowed only for its owner, or from inside the task that
    submitted it (enkiTS's child-wait rule).
  - The main thread submits a bounded number of `Normal` task sets per frame, so
    its own pipe never fills.
- **Evidence tier.** The task system adds headroom, not behavior. Until a service
  moves (R5b.2), production builds contain neither the library nor its threads.

## Rejected alternatives

- **Taskflow 4.1.0 (MIT).** It has no priorities or pinned tasks in any release, and
  it allocates a node per task and a topology per run, so frames are not
  allocation-free as shipped.
- **Keeping per-service threads with OS priorities.** OS priority does not bound
  latency to the main thread's join. Ten condition-variable services also cannot
  share workers with frame work.
- **A gate that counts tasks instead of threads.** One background task set can
  occupy every worker through work stealing. The gate therefore caps each task's
  chunk count at the slots it holds.

## Verification

- **`EnkiTsSpikeTests` (R5b.0)** runs against enkiTS directly:
  - three priorities;
  - a HIGH-filtered main-thread wait never runs MED or LOW work;
  - pinned tasks run on a dedicated I/O thread that never runs task sets;
  - waits inside tasks (nested fork-join);
  - no allocation after `Initialize`: 15 allocations (78,456 B) at init, then 0 in
    64 warm-up and 2,000 steady iterations, by both the enkiTS allocator hook and
    the engine operator new counter (main thread and enkiTS threads).
- **`TaskSystemTests` (R5b.1)** covers:
  - priority ordering and the main-thread join rule;
  - `parallelFor` coverage and grain at every priority;
  - the admission gate: limit, FIFO admission, frame work beside saturated
    background, and a chunk cap that bounds concurrency;
  - nested background fork-join under a one-slot gate;
  - the pinned I/O thread (FIFO, never task sets);
  - strand serialization and FIFO order;
  - shutdown with pending background work (cancellation, strands, late
    submissions);
  - task exceptions;
  - exact thread-scoped allocation attribution;
  - per-worker profiler scopes, including a background scope spanning a frame
    boundary;
  - 500 steady frames that mix every primitive with 0 frame allocations, 0
    background allocations and 0 enkiTS allocations.
- **`CpuProfilerTests` and `CpuAllocationProfileTests`** cover worker streams, stream
  overflow, export and thread-scoped counting.
- **Production evidence.** The task system is not linked into production in R5b.1.
  With the thread-scoped counters in the engine:
  - frozen `r5b1` (synchronization validation, 0 messages, 0 hazards) is identical
    to `r0` or within its envelopes;
  - the indirect digest is identical to `r3a0`;
  - sweep `r5b1-sweep` passes 36/36 and matches `r4d-sweep-final`, apart from
    R00/R03's known run-to-run probe-publish variance (reruns match). Main-thread
    allocation counters are unchanged.
