#include "app/cli/ApplicationCliOptions.h"

#include "app/ApplicationConfig.h"
#include "app/cli/CliValueParsers.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace Iridium::AppCli {

    namespace {

        std::pair<uint32_t, uint32_t> parseWindowSize(std::string_view text) {
            const size_t separator = text.find_first_of("xX");
            if (separator == std::string_view::npos) {
                throw std::invalid_argument("--window-size requires WIDTHxHEIGHT");
            }
            const uint64_t width = parseUnsigned(text.substr(0, separator), "--window-size");
            const uint64_t height = parseUnsigned(text.substr(separator + 1), "--window-size");
            if (width == 0 || height == 0 ||
                width > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("--window-size dimensions are out of range");
            }
            return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
        }

    } // namespace

    void registerRuntimeOptions(Cli::CliOptionRegistry& registry,
        ApplicationConfig& config) {
        const std::string_view owner = kRuntimeOwner;
        ApplicationConfig& c = config;

        addSwitch(registry, owner, "--validation", "Enable Vulkan validation",
            [&c] { c.enableValidation = true; });
        addSwitch(registry, owner, "--no-validation", "Disable Vulkan validation",
            [&c] { c.enableValidation = false; });
        addSwitch(registry, owner, "--validation-sync",
            "Enable Vulkan validation with synchronization hazard checks",
            [&c] {
                c.enableValidation = true;
                c.enableSynchronizationValidation = true;
            });
        addSwitch(registry, owner, "--profile-cpu", "Collect bounded CPU frame telemetry",
            [&c] { c.enableCpuProfiling = true; });
        addSwitch(registry, owner, "--profile-gpu", "Collect delayed Vulkan GPU timestamps",
            [&c] {
                c.enableCpuProfiling = true;
                c.enableGpuProfiling = true;
            });
        addSwitch(registry, owner, "--profile-transparent-overdraw",
            "Collect optional transparent fragment workload",
            [&c] {
                c.enableCpuProfiling = true;
                c.enableTransparentPipelineStatistics = true;
            });
        addSwitch(registry, owner, "--hidden-window", "Create a hidden benchmark surface",
            [&c] {
                c.windowVisible = false;
                c.windowDecorated = false;
            });
        addSwitch(registry, owner, "--borderless-window",
            "Remove decorations from a visible surface",
            [&c] { c.windowDecorated = false; });
        addValueOption(registry, owner, "--cooked-model-artifact", "PATH",
            "Load a self-contained cooked model instead of source",
            "--cooked-model-artifact requires a path",
            [&c](std::string_view value) { c.cookedModelArtifact = std::string(value); },
            true);
        addValueOption(registry, owner, "--cooked-environment-artifact", "PATH",
            "Load a cooked cubemap/IBL environment product",
            "--cooked-environment-artifact requires a path",
            [&c](std::string_view value) {
                c.cookedEnvironmentArtifact = std::string(value);
            },
            true);
        addValueOption(registry, owner, "--warmup-frames", "COUNT",
            "Run COUNT unmeasured frames first",
            "--warmup-frames requires a frame count",
            [&c](std::string_view value) {
                c.warmupFrameCount = parseUnsigned(value, "--warmup-frames");
                c.warmupFrameCountSpecified = true;
            });
        addValueOption(registry, owner, "--frame-limit", "COUNT",
            "Exit after COUNT measured frames",
            "--frame-limit requires a frame count",
            [&c](std::string_view value) {
                c.frameLimit = parseUnsigned(value, "--frame-limit");
                c.frameLimitSpecified = true;
            });
        addValueOption(registry, owner, "--window-size", "WIDTHxHEIGHT",
            "Set the render-window dimensions",
            "--window-size requires WIDTHxHEIGHT",
            [&c](std::string_view value) {
                const auto [width, height] = parseWindowSize(value);
                c.windowWidth = width;
                c.windowHeight = height;
            });

        Cli::CliOption help;
        help.name = "--help";
        help.aliases = { "-h" };
        help.help = "Show this help";
        help.owner = std::string(owner);
        help.parse = [&c](std::string_view) { c.showHelp = true; };
        registry.add(std::move(help));
    }

} // namespace Iridium::AppCli
