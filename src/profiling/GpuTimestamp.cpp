#include "GpuTimestamp.h"

#include <cmath>
#include <limits>

namespace Iridium {

    uint64_t calculateGpuTimestampDurationNanoseconds(uint64_t startTicks,
        uint64_t endTicks, uint32_t validBits,
        double timestampPeriodNanoseconds) noexcept {
        if (validBits == 0 || validBits > 64 ||
            !(timestampPeriodNanoseconds > 0.0)) {
            return 0;
        }

        uint64_t elapsedTicks = 0;
        if (validBits == 64) {
            elapsedTicks = endTicks - startTicks;
        }
        else {
            const uint64_t mask = (uint64_t{ 1 } << validBits) - 1;
            elapsedTicks = (endTicks - startTicks) & mask;
        }

        const long double duration = static_cast<long double>(elapsedTicks) *
            static_cast<long double>(timestampPeriodNanoseconds);
        if (duration >= static_cast<long double>(
            std::numeric_limits<uint64_t>::max())) {
            return std::numeric_limits<uint64_t>::max();
        }
        return static_cast<uint64_t>(std::llround(duration));
    }

} // namespace Iridium
