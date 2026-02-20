#include "core/Application.h"
#include <iostream>
#include <stdexcept>

int main() {
    Application app;
    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}