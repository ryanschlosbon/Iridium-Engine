#pragma once

#include <cstdint>

namespace Iridium {

    // Converts two timestamps from one Vulkan queue domain to nanoseconds.
    // validBits is the queue-family timestampValidBits value and permits wrap.
    [[nodiscard]] uint64_t calculateGpuTimestampDurationNanoseconds(
        uint64_t startTicks, uint64_t endTicks, uint32_t validBits,
        double timestampPeriodNanoseconds) noexcept;

} // namespace Iridium
