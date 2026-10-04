#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium::Cli {

    // One command-line flag. Modules describe their flags as data and register
    // them with a CliOptionRegistry; parsing and the usage text both come from
    // the registry, so a module that is not linked contributes no flags.
    struct CliOption {
        std::string name;                  // "--frame-limit"
        std::vector<std::string> aliases;  // "-h"
        uint8_t arity = 0;                 // 0 (switch) or 1 (takes one value)
        std::string valueName;             // "COUNT"; usage only
        std::string help;
        std::string owner;                 // "runtime", "editor", "renderer", ...
        // Thrown (std::invalid_argument) when an arity-1 option is the last
        // argument, or when rejectEmptyValue is set and the value is empty.
        std::string missingValueMessage;
        bool rejectEmptyValue = false;
        // Applies the option. Arity-0 options receive an empty view. Invalid
        // values throw std::invalid_argument with the option's own message.
        std::function<void(std::string_view)> parse;
    };

    class CliOptionRegistry {
    public:
        // Throws std::logic_error for an empty name or owner, an arity other
        // than 0 or 1, a missing parse callback or missing-value message, or a
        // name/alias that is already registered.
        void add(CliOption option);

        // Post-parse checks. They run after every argument has been applied,
        // in ascending `order`, then registration order. Order is observable
        // when several checks fail at once (only the first throws).
        void addValidator(std::string owner, std::function<void()> check,
            int order = 0);

        // Applies arguments left to right; a repeated option is applied again
        // (the last value wins). Throws std::invalid_argument("Unknown option: X")
        // for an unregistered argument.
        void parse(std::span<const std::string_view> arguments) const;

        // One line per option, grouped by owner in the order owners first
        // registered an option, options in registration order within a group.
        [[nodiscard]] std::string usage() const;

        [[nodiscard]] std::span<const CliOption> options() const noexcept {
            return options_;
        }
        [[nodiscard]] const CliOption* find(std::string_view nameOrAlias) const;
        [[nodiscard]] std::vector<std::string> owners() const;

    private:
        struct Validator {
            std::string owner;
            std::function<void()> check;
            int order = 0;
        };

        std::vector<CliOption> options_;
        std::map<std::string, size_t, std::less<>> index_;
        std::vector<Validator> validators_;
    };

} // namespace Iridium::Cli
