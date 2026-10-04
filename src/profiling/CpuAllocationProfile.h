#pragma once

#include <cstdint>

namespace Iridium {

    struct CpuAllocationFrameSample {
        // Frame allocations: the thread that called beginCpuAllocationFrame (the
        // main thread in the engine) plus any thread inside a
        // CpuAllocationFrameScope (task workers while they run frame-critical
        // tasks). This is the steady-frame allocation figure.
        uint64_t allocationCount = 0;
        uint64_t requestedBytes = 0;
        // M7R R5b.1: allocations made by every other thread while the frame was
        // active (task workers outside frame-critical work, the pinned I/O
        // thread, service threads). Not part of the steady-frame invariant.
        uint64_t backgroundAllocationCount = 0;
        uint64_t backgroundRequestedBytes = 0;
    };

    // The diagnostic tracks C++ global new/new[] calls only. It deliberately does
    // not claim malloc/free, third-party allocators, or driver allocations.
    // begin/end are called by one thread (the frame thread); allocations are
    // attributed per thread as described on CpuAllocationFrameSample.
    void beginCpuAllocationFrame() noexcept;
    [[nodiscard]] CpuAllocationFrameSample endCpuAllocationFrame() noexcept;
    [[nodiscard]] bool isCpuAllocationFrameActive() noexcept;

    namespace cpu_allocation_detail {
        // Header-only thread state, so code that marks frame work (the task
        // system) does not have to link the operator new replacement, which is
        // an OBJECT library linked only into executables that track allocations.
        // Both are constant-initialized: no TLS guard on the operator new path.
        inline thread_local uint32_t frameScopeDepth = 0;
        inline thread_local unsigned char threadMarker = 0;

        [[nodiscard]] inline const void* currentThreadMarker() noexcept {
            return &threadMarker;
        }
    } // namespace cpu_allocation_detail

    // Attributes this thread's allocations to the frame counters while alive. The
    // task system opens one around every frame-critical task it runs on a worker,
    // so parallel frame work stays inside the steady-frame invariant. Nests.
    class CpuAllocationFrameScope final {
    public:
        CpuAllocationFrameScope() noexcept { ++cpu_allocation_detail::frameScopeDepth; }
        ~CpuAllocationFrameScope() { --cpu_allocation_detail::frameScopeDepth; }
        CpuAllocationFrameScope(const CpuAllocationFrameScope&) = delete;
        CpuAllocationFrameScope& operator=(const CpuAllocationFrameScope&) = delete;
    };

} // namespace Iridium
