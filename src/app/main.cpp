#include "app/Application.h"
#include "app/ApplicationConfig.h"
#include "core/BuildFeatures.h"
#include "qualification/harness/QualificationHarness.h"
// Declaration only; defined in iridium_qualification, which is linked only
// when IRIDIUM_QUALIFICATION=ON. The call below is a discarded statement
// otherwise, so OFF builds need no definition.
#include "qualification/vulkan/VulkanQualificationInstall.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    try {
        // M7R R2.7 -> R2.9: the legacy backend factory overload attaches the
        // Vulkan qualification extension registered here.
        if constexpr (Iridium::kQualificationBuild)
            Iridium::installQualificationBackendExtensions();
        std::vector<std::string_view> arguments;
        arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        Iridium::ApplicationConfig config = Iridium::parseApplicationConfig(arguments);
        if (config.showHelp) {
            std::cout << Iridium::applicationUsage();
            return EXIT_SUCCESS;
        }

        // Qualification builds attach the harness to every run. Without
        // qualification flags it only prints the IRIDIUM_* startup and run-metric
        // lines; production code never depends on it (M7R R2.6).
        std::unique_ptr<Iridium::IFrameObserver> observer;
        if constexpr (Iridium::kQualificationBuild) {
            observer = Iridium::createQualificationHarness(config);
        }
        Iridium::Application app(std::move(config), observer.get());
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
