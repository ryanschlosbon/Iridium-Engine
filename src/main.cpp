#include "core/Application.h"
#include "core/ApplicationConfig.h"
#include <cstdlib>
#include <iostream>
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
        Iridium::ApplicationConfig config = Iridium::parseApplicationConfig(arguments);
        if (config.showHelp) {
            std::cout << Iridium::applicationUsage();
            return EXIT_SUCCESS;
        }

        Iridium::Application app(std::move(config));
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
