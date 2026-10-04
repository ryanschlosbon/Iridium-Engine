// Unit tests for Iridium::Cli::CliOptionRegistry (iridium_core).
#include "core/cli/CliOptionRegistry.h"

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium::Cli;
    using Args = std::vector<std::string_view>;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    CliOption makeSwitch(std::string name, std::string owner, bool* target) {
        CliOption option;
        option.name = std::move(name);
        option.owner = std::move(owner);
        option.help = "switch " + option.name;
        option.parse = [target](std::string_view value) {
            *target = value.empty();
        };
        return option;
    }

    CliOption makeValue(std::string name, std::string owner, std::string* target,
        bool rejectEmpty = false) {
        CliOption option;
        option.name = std::move(name);
        option.owner = std::move(owner);
        option.arity = 1;
        option.valueName = "VALUE";
        option.help = "value " + option.name;
        option.missingValueMessage = option.name + " requires a value";
        option.rejectEmptyValue = rejectEmpty;
        option.parse = [target](std::string_view value) {
            if (value == "bad") throw std::invalid_argument("bad value");
            *target = std::string(value);
        };
        return option;
    }

    template <typename Exception, typename Function>
    std::string thrownMessage(Function&& function) {
        try {
            function();
        }
        catch (const Exception& error) {
            return error.what();
        }
        catch (...) {
            return "<other exception>";
        }
        return "<no exception>";
    }

    bool testDuplicateRegistrationThrows() {
        bool a = false;
        CliOptionRegistry registry;
        CliOption help = makeSwitch("--help", "runtime", &a);
        help.aliases = { "-h" };
        registry.add(std::move(help));
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.add(makeSwitch("--help", "editor", &a));
        }) == "CLI option --help is already registered");
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.add(makeSwitch("-h", "editor", &a));
        }) == "CLI option -h is already registered");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeSwitch("--other", "editor", &a);
            option.aliases = { "-h" };
            registry.add(std::move(option));
        }) == "CLI option -h is already registered");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeSwitch("--self", "editor", &a);
            option.aliases = { "--self" };
            registry.add(std::move(option));
        }) == "CLI option --self is already registered");
        // A rejected registration leaves no partial entries behind.
        CHECK(registry.options().size() == 1);
        CHECK(registry.find("--other") == nullptr);
        CHECK(registry.find("--self") == nullptr);
        return true;
    }

    bool testInvalidRegistrationThrows() {
        bool a = false;
        std::string s;
        CliOptionRegistry registry;
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.add(makeSwitch("", "runtime", &a));
        }) == "CLI option requires a name");
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.add(makeSwitch("--x", "", &a));
        }) == "CLI option --x requires an owner");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeValue("--x", "runtime", &s);
            option.arity = 2;
            registry.add(std::move(option));
        }) == "CLI option --x has arity above 1");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeValue("--x", "runtime", &s);
            option.parse = nullptr;
            registry.add(std::move(option));
        }) == "CLI option --x has no parse callback");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeValue("--x", "runtime", &s);
            option.missingValueMessage.clear();
            registry.add(std::move(option));
        }) == "CLI option --x requires a missing-value message");
        CHECK(thrownMessage<std::logic_error>([&] {
            CliOption option = makeSwitch("--x", "runtime", &a);
            option.rejectEmptyValue = true;
            registry.add(std::move(option));
        }) == "CLI option --x takes no value to reject");
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.addValidator("", [] {});
        }) == "CLI validator requires an owner");
        CHECK(thrownMessage<std::logic_error>([&] {
            registry.addValidator("runtime", nullptr);
        }) == "CLI validator for runtime has no check");
        CHECK(registry.options().empty());
        return true;
    }

    bool testParsing() {
        bool help = false;
        bool other = false;
        std::string value;
        std::string strict;
        CliOptionRegistry registry;
        CliOption helpOption = makeSwitch("--help", "runtime", &help);
        helpOption.aliases = { "-h" };
        registry.add(std::move(helpOption));
        registry.add(makeSwitch("--other", "runtime", &other));
        registry.add(makeValue("--value", "runtime", &value));
        registry.add(makeValue("--strict", "runtime", &strict, true));

        registry.parse(Args{ "-h" });
        CHECK(help);  // arity-0 callbacks receive an empty value
        CHECK(registry.find("-h") == registry.find("--help"));

        registry.parse(Args{ "--value", "a", "--value", "b" });
        CHECK(value == "b");  // last wins
        registry.parse(Args{ "--value", "--other" });
        CHECK(value == "--other");  // the value slot is consumed verbatim
        CHECK(!other);
        registry.parse(Args{ "--value", "" });
        CHECK(value.empty());  // empty accepted unless rejectEmptyValue

        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--strict", "" });
        }) == "--strict requires a value");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--value" });
        }) == "--value requires a value");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--value", "bad" });
        }) == "bad value");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--nope" });
        }) == "Unknown option: --nope");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "" });
        }) == "Unknown option: ");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--HELP" });
        }) == "Unknown option: --HELP");
        // Arguments apply left to right: the first failure wins.
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--value", "bad", "--nope" });
        }) == "bad value");
        CHECK(thrownMessage<std::invalid_argument>([&] {
            registry.parse(Args{ "--nope", "--value", "bad" });
        }) == "Unknown option: --nope");
        return true;
    }

    bool testValidatorOrder() {
        std::vector<std::string> ran;
        bool flag = false;
        CliOptionRegistry registry;
        registry.add(makeSwitch("--flag", "a", &flag));
        registry.addValidator("a", [&] { ran.push_back("a30"); }, 30);
        registry.addValidator("b", [&] { ran.push_back("b10"); }, 10);
        registry.addValidator("a", [&] { ran.push_back("a0-first"); });
        registry.addValidator("c", [&] { ran.push_back("c30"); }, 30);
        registry.addValidator("b", [&] { ran.push_back("b0-second"); });
        registry.parse(Args{});
        CHECK((ran == std::vector<std::string>{
            "a0-first", "b0-second", "b10", "a30", "c30" }));

        // Validators run after every argument is applied; the first failure stops.
        std::vector<std::string> seen;
        CliOptionRegistry failing;
        failing.add(makeSwitch("--flag", "a", &flag));
        failing.addValidator("a", [&] {
            seen.push_back(flag ? "flag" : "no-flag");
            throw std::invalid_argument("first");
        }, 1);
        failing.addValidator("a", [&] {
            seen.push_back("second");
            throw std::invalid_argument("second");
        }, 2);
        flag = false;
        CHECK(thrownMessage<std::invalid_argument>([&] {
            failing.parse(Args{ "--flag" });
        }) == "first");
        CHECK((seen == std::vector<std::string>{ "flag" }));
        // An argument error preempts every validator.
        seen.clear();
        CHECK(thrownMessage<std::invalid_argument>([&] {
            failing.parse(Args{ "--nope" });
        }) == "Unknown option: --nope");
        CHECK(seen.empty());
        return true;
    }

    bool testUsageGrouping() {
        bool a = false;
        std::string s;
        CliOptionRegistry registry;
        registry.add(makeSwitch("--alpha", "runtime", &a));
        registry.add(makeValue("--beta", "renderer", &s));
        CliOption gamma = makeSwitch("--gamma", "runtime", &a);
        gamma.aliases = { "-g" };
        registry.add(std::move(gamma));
        CliOption longOne = makeValue("--a-very-long-option-name-indeed", "editor", &s);
        longOne.valueName = "N";
        registry.add(std::move(longOne));

        CHECK((registry.owners() == std::vector<std::string>{ "runtime", "renderer", "editor" }));
        const std::string expected =
            "\nruntime options:\n"
            "  --alpha                       switch --alpha\n"
            "  --gamma, -g                   switch --gamma\n"
            "\nrenderer options:\n"
            "  --beta VALUE                  value --beta\n"
            "\neditor options:\n"
            "  --a-very-long-option-name-indeed N  value --a-very-long-option-name-indeed\n";
        if (registry.usage() != expected) {
            std::cerr << "  usage was:\n" << registry.usage();
            return false;
        }
        CHECK(CliOptionRegistry{}.usage().empty());
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Duplicate registration throws", testDuplicateRegistrationThrows },
        { "Invalid registration throws", testInvalidRegistrationThrows },
        { "Parsing, aliases, last-wins, errors", testParsing },
        { "Validator order", testValidatorOrder },
        { "Usage grouping", testUsageGrouping },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
