// THE INCLUDES
#define GLFW_INCLUDE_VULKAN 
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <stdexcept> // For throwing errors if initialization fails
#include <cstring>   // For strcmp (string comparison)
#include "renderer/VkContext.h" // Engine Core
#include "renderer/VkSwapchain.h"
#include "renderer/VkRenderPass.h"
#include "renderer/VkFramebuffer.h"

// CONSTANTS
const int WIDTH = 1280;
const int HEIGHT = 720;

// THE CLASS STRUCTURE
class IridiumEngine {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window;
    VkContext* vkContext; // use a pointer so it can be easily created and destroyed
    VkSwapchain* vkSwapchain;
    VkRenderPassWrapper* vkRenderPass;
    VkFramebufferWrapper* vkFramebuffer;

    void initWindow() {
        glfwInit();

        // We are using Vulkan, so we must tell GLFW "No API please".
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        // Resize is disabled for now because handling window resizing 
        // in Vulkan requires rebuilding the entire swapchain (complex).
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        window = glfwCreateWindow(WIDTH, HEIGHT, "Iridium Engine", nullptr, nullptr);
    }

    void initVulkan() {
        // Initialize the Context
        // true = Enable Validation Layers
        // window = pass the GLFW window so the context can create the Surface
        vkContext = new VkContext(true, window);
        vkSwapchain = new VkSwapchain(vkContext, window);
        vkRenderPass = new VkRenderPassWrapper(vkContext, vkSwapchain);
        vkFramebuffer = new VkFramebufferWrapper(vkContext, vkSwapchain, vkRenderPass);
    }

    void mainLoop() {
        // Simple loop that keeps the window open
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
        }
    }

    void cleanup() {
		delete vkFramebuffer;
		delete vkRenderPass;
        delete vkSwapchain;
        delete vkContext;
        glfwDestroyWindow(window);
        glfwTerminate();
    }
};

int main() {
    IridiumEngine app;
    try {
        app.run();
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}