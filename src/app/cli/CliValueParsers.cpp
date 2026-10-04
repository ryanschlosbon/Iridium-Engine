#include "app/cli/CliValueParsers.h"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Iridium::AppCli {

    uint64_t parseUnsigned(std::string_view text, const char* option) {
        uint64_t value = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end != last) {
            throw std::invalid_argument(std::string(option) +
                " requires an unsigned integer");
        }
        return value;
    }

    double parseFiniteRange(std::string_view text, const char* option,
        double minimum, double maximum) {
        double value = 0.0;
        const auto [end, error] = std::from_chars(
            text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() ||
            !std::isfinite(value) || value < minimum || value > maximum) {
            throw std::invalid_argument(std::string(option) +
                " requires a finite value in [" +
                std::to_string(minimum) + ", " +
                std::to_string(maximum) + "]");
        }
        return value;
    }

    double parseLuminance(std::string_view text, const char* option,
        double minimum, double maximum) {
        double value = 0.0;
        const auto [end, error] = std::from_chars(
            text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() ||
            !std::isfinite(value) || value < minimum || value > maximum) {
            throw std::invalid_argument(std::string(option) +
                " requires a finite luminance in [" +
                std::to_string(minimum) + ", " +
                std::to_string(maximum) + "] nits");
        }
        return value;
    }

    void addSwitch(Cli::CliOptionRegistry& registry, std::string_view owner,
        std::string name, std::string help, std::function<void()> apply) {
        Cli::CliOption option;
        option.name = std::move(name);
        option.help = std::move(help);
        option.owner = std::string(owner);
        option.parse = [apply = std::move(apply)](std::string_view) { apply(); };
        registry.add(std::move(option));
    }

    void addValueOption(Cli::CliOptionRegistry& registry, std::string_view owner,
        std::string name, std::string valueName, std::string help,
        std::string missingValueMessage,
        std::function<void(std::string_view)> parse, bool rejectEmptyValue) {
        Cli::CliOption option;
        option.name = std::move(name);
        option.arity = 1;
        option.valueName = std::move(valueName);
        option.help = std::move(help);
        option.owner = std::string(owner);
        option.missingValueMessage = std::move(missingValueMessage);
        option.rejectEmptyValue = rejectEmptyValue;
        option.parse = std::move(parse);
        registry.add(std::move(option));
    }

} // namespace Iridium::AppCli
