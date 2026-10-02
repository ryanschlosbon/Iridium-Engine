#pragma once

#include <cstdint>
#include <string>

namespace Iridium {

    struct SystemProfile {
        std::string operatingSystem;
        std::string cpuName;
        uint64_t physicalMemoryBytes = 0;
    };

    [[nodiscard]] SystemProfile querySystemProfile();

} // namespace Iridium
