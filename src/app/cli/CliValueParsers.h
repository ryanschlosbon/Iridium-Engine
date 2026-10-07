#pragma once

#include "core/cli/CliOptionRegistry.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// Value parsers and registration shorthands shared by the app/cli option
// modules. Error messages are part of the CLI contract; keep them verbatim.
namespace Iridium::AppCli {

    // Throws "<option> requires an unsigned integer".
    [[nodiscard]] uint64_t parseUnsigned(std::string_view text, const char* option);

    // Throws "<option> requires a finite value in [<min>, <max>]" (std::to_string
    // formatting of the bounds).
    [[nodiscard]] double parseFiniteRange(std::string_view text, const char* option,
        double minimum, double maximum);

    // Throws "<option> requires a finite luminance in [<min>, <max>] nits".
    [[nodiscard]] double parseLuminance(std::string_view text, const char* option,
        double minimum, double maximum);

    void addSwitch(Cli::CliOptionRegistry& registry, std::string_view owner,
        std::string name, std::string help, std::function<void()> apply);

    void addValueOption(Cli::CliOptionRegistry& registry, std::string_view owner,
        std::string name, std::string valueName, std::string help,
        std::string missingValueMessage,
        std::function<void(std::string_view)> parse,
        bool rejectEmptyValue = false);

} // namespace Iridium::AppCli
