#include "app/ApplicationConfig.h"

#include "app/cli/ApplicationCliOptions.h"
#include "core/cli/CliOptionRegistry.h"

namespace Iridium {

    namespace AppCli {

        void registerApplicationOptions(Cli::CliOptionRegistry& registry,
            ApplicationConfig& config) {
            registerRuntimeOptions(registry, config);
            registerEditorOptions(registry, config);
            registerRendererOptions(registry, config);
        }

    } // namespace AppCli

    ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments) {
        ApplicationConfig config{};
        Cli::CliOptionRegistry registry;
        AppCli::registerApplicationOptions(registry, config);
        registry.parse(arguments);
        return config;
    }

    std::string applicationUsage(const Cli::CliOptionRegistry& registry) {
        return "Usage: IridiumEngine [options]\n" + registry.usage();
    }

    std::string applicationUsage() {
        ApplicationConfig unused{};
        Cli::CliOptionRegistry registry;
        AppCli::registerApplicationOptions(registry, unused);
        return applicationUsage(registry);
    }

} // namespace Iridium
