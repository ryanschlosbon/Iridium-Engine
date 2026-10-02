#pragma once

// FROZEN REFERENCE FOR M7R R2 CLI PARITY (test-only; delete at M7R R2.10).
// The 6b000ad hand-written parser, used as the oracle for the registry parser.

#include "app/ApplicationConfig.h"

#include <span>
#include <string>
#include <string_view>

namespace Iridium::FrozenR2Reference {

    [[nodiscard]] ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments);
    [[nodiscard]] std::string applicationUsage();

} // namespace Iridium::FrozenR2Reference
