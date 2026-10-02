#include "app/Application.h"
#include "app/ApplicationConfig.h"
#include "app/cli/ApplicationCliOptions.h"
#include "core/BuildFeatures.h"
#include "core/cli/CliOptionRegistry.h"
// The only preprocessor switch on IRIDIUM_QUALIFICATION (M7R R2.9): OFF builds
// neither compile nor link the qualification library, its flags or its harness.
#if IRIDIUM_QUALIFICATION
#include "qualification/QualificationOptions.h"
#include "qualification/harness/QualificationHarness.h"
#endif
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        // Runtime, editor and renderer flags, then (qualification builds only)
        // the qualification flags; usage follows the same registry.
        Iridium::ApplicationConfig config{};
        Iridium::Cli::CliOptionRegistry registry;
        Iridium::AppCli::registerApplicationOptions(registry, config);
#if IRIDIUM_QUALIFICATION
        Iridium::QualificationOptions qualification{};
        Iridium::registerQualificationOptions(registry, qualification, config);
#endif
        registry.parse(arguments);
        if (config.showHelp) {
            std::cout << Iridium::applicationUsage(registry);
            return EXIT_SUCCESS;
        }

        // Qualification builds attach the harness to every run. Without
        // qualification flags it only prints the IRIDIUM_* startup and run-metric
        // lines; production code never depends on it (M7R R2.6).
        std::unique_ptr<Iridium::IFrameObserver> observer;
#if IRIDIUM_QUALIFICATION
        observer = Iridium::createQualificationHarness(qualification);
#endif
        Iridium::Application app(std::move(config), observer.get());
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
