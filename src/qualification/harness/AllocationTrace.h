#pragma once

// M7R R5c.8: --qualification-allocation-trace. Records the call stack of
// every allocation the engine counts as a frame allocation (allocation.cpp.*:
// the main thread and frame-critical tasks) in the run's steady frames, and
// reports them grouped by stack at the end of the run as
// IRIDIUM_ALLOCATION_TRACE. Steady frames are the measured frames from
// frame-limit / 2 on (at least the second), the frames the sweep's and the
// timing summaries' steady allocation figures come from.
//
// The capture runs inside operator new through the allocation profile's
// observer slot (profiling/CpuAllocationProfile.h): a fixed record buffer
// reserved at construction and RtlCaptureStackBackTrace, so tracing itself
// allocates nothing. Symbols are resolved with DbgHelp after the run; a build
// without PDBs reports module+offset.

#include <cstdint>
#include <memory>
#include <string>

namespace Iridium {

    class AllocationTrace final {
    public:
        // Measured frames with index >= firstSteadyFrame are traced.
        explicit AllocationTrace(uint64_t firstSteadyFrame);
        // Uninstalls the observer.
        ~AllocationTrace();

        AllocationTrace(const AllocationTrace&) = delete;
        AllocationTrace& operator=(const AllocationTrace&) = delete;

        // Main thread, once per frame after the allocation frame began
        // (FrameBeginPhase::PreSceneUpdate). Arms the trace from the next
        // allocation frame once the next measured frame is steady, so steady
        // frames are traced from their first allocation.
        void onFrameBegin(bool measured, uint64_t measuredIndex) noexcept;

        // Stops recording and returns the report (JSON, one line).
        [[nodiscard]] std::string finishJson();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace Iridium
