#pragma once

#include <cstdint>
#include <stdexcept>

namespace Iridium {

    [[nodiscard]] constexpr uint32_t
        nextMaterialTableCapacity(
            uint32_t currentCapacity,
            uint32_t requiredCapacity,
            uint32_t maximumCapacity) {
        if (requiredCapacity == 0 ||
            maximumCapacity == 0 ||
            requiredCapacity > maximumCapacity) {
            throw std::overflow_error(
                "material table requirement exceeds its maximum capacity");
        }
        uint32_t capacity =
            currentCapacity == 0 ? 1u :
                currentCapacity;
        if (capacity > maximumCapacity) {
            throw std::overflow_error(
                "material table current capacity exceeds its maximum");
        }
        while (capacity < requiredCapacity) {
            capacity =
                capacity > maximumCapacity / 2u
                ? maximumCapacity
                : capacity * 2u;
        }
        return capacity;
    }

} // namespace Iridium
