#include "core/cli/CliOptionRegistry.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium::Cli {

    namespace {

        // Help text starts at this column unless the syntax is longer.
        constexpr size_t kHelpColumn = 32;
        constexpr std::string_view kIndent = "  ";

        std::string optionSyntax(const CliOption& option) {
            std::string syntax(kIndent);
            syntax += option.name;
            for (const std::string& alias : option.aliases) {
                syntax += ", ";
                syntax += alias;
            }
            if (!option.valueName.empty()) {
                syntax += ' ';
                syntax += option.valueName;
            }
            return syntax;
        }

    } // namespace

    void CliOptionRegistry::add(CliOption option) {
        if (option.name.empty()) {
            throw std::logic_error("CLI option requires a name");
        }
        if (option.owner.empty()) {
            throw std::logic_error("CLI option " + option.name + " requires an owner");
        }
        if (option.arity > 1) {
            throw std::logic_error("CLI option " + option.name + " has arity above 1");
        }
        if (!option.parse) {
            throw std::logic_error("CLI option " + option.name + " has no parse callback");
        }
        if (option.arity == 1 && option.missingValueMessage.empty()) {
            throw std::logic_error(
                "CLI option " + option.name + " requires a missing-value message");
        }
        if (option.arity == 0 && option.rejectEmptyValue) {
            throw std::logic_error(
                "CLI option " + option.name + " takes no value to reject");
        }
        std::vector<std::string_view> spellings{ option.name };
        spellings.insert(spellings.end(), option.aliases.begin(), option.aliases.end());
        for (size_t i = 0; i < spellings.size(); ++i) {
            if (spellings[i].empty()) {
                throw std::logic_error("CLI option " + option.name + " has an empty alias");
            }
            const bool repeatedHere = std::find(spellings.begin(),
                spellings.begin() + static_cast<std::ptrdiff_t>(i), spellings[i]) !=
                spellings.begin() + static_cast<std::ptrdiff_t>(i);
            if (repeatedHere || index_.contains(spellings[i])) {
                throw std::logic_error(
                    "CLI option " + std::string(spellings[i]) + " is already registered");
            }
        }
        const size_t position = options_.size();
        for (const std::string_view spelling : spellings) {
            index_.emplace(std::string(spelling), position);
        }
        options_.push_back(std::move(option));
    }

    void CliOptionRegistry::addValidator(std::string owner,
        std::function<void()> check, int order) {
        if (owner.empty()) {
            throw std::logic_error("CLI validator requires an owner");
        }
        if (!check) {
            throw std::logic_error("CLI validator for " + owner + " has no check");
        }
        // Insert after every validator with an order <= this one, so equal
        // orders keep registration order.
        const auto position = std::upper_bound(validators_.begin(), validators_.end(),
            order, [](int value, const Validator& validator) {
                return value < validator.order;
            });
        validators_.insert(position, Validator{ std::move(owner), std::move(check), order });
    }

    const CliOption* CliOptionRegistry::find(std::string_view nameOrAlias) const {
        const auto found = index_.find(nameOrAlias);
        return found == index_.end() ? nullptr : &options_[found->second];
    }

    void CliOptionRegistry::parse(std::span<const std::string_view> arguments) const {
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            const CliOption* option = find(argument);
            if (option == nullptr) {
                throw std::invalid_argument("Unknown option: " + std::string(argument));
            }
            if (option->arity == 0) {
                option->parse({});
                continue;
            }
            if (++index >= arguments.size() ||
                (option->rejectEmptyValue && arguments[index].empty())) {
                throw std::invalid_argument(option->missingValueMessage);
            }
            option->parse(arguments[index]);
        }
        for (const Validator& validator : validators_) {
            validator.check();
        }
    }

    std::vector<std::string> CliOptionRegistry::owners() const {
        std::vector<std::string> result;
        for (const CliOption& option : options_) {
            if (std::find(result.begin(), result.end(), option.owner) == result.end()) {
                result.push_back(option.owner);
            }
        }
        return result;
    }

    std::string CliOptionRegistry::usage() const {
        std::string text;
        for (const std::string& owner : owners()) {
            text += '\n';
            text += owner;
            text += " options:\n";
            for (const CliOption& option : options_) {
                if (option.owner != owner) continue;
                std::string line = optionSyntax(option);
                line.append(line.size() + 2 <= kHelpColumn
                    ? kHelpColumn - line.size() : 2, ' ');
                line += option.help;
                line += '\n';
                text += line;
            }
        }
        return text;
    }

} // namespace Iridium::Cli
