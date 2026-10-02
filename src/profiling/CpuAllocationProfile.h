#pragma once

#include <cstdint>

namespace Iridium {

    struct CpuAllocationFrameSample {
        uint64_t allocationCount = 0;
        uint64_t requestedBytes = 0;
    };

    // The diagnostic tracks C++ global new/new[] calls only. It deliberately does
    // not claim malloc/free, third-party allocators, or driver allocations.
    void beginCpuAllocationFrame() noexcept;
    [[nodiscard]] CpuAllocationFrameSample endCpuAllocationFrame() noexcept;
    [[nodiscard]] bool isCpuAllocationFrameActive() noexcept;

} // namespace Iridium
