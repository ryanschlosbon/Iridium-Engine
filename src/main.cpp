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
#include "renderer/VkGraphicsPipeline.h"
#include "renderer/VkCommandManager.h"
#include "renderer/VkSyncObjects.h"

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
    VkGraphicsPipeline* vkPipeline;
	VkCommandManager* vkCommandManager;
	VkSyncObjects* vkSyncObjects;

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
		vkPipeline = new VkGraphicsPipeline(vkContext, vkSwapchain, vkRenderPass);
        vkFramebuffer = new VkFramebufferWrapper(vkContext, vkSwapchain, vkRenderPass);
		vkCommandManager = new VkCommandManager(vkContext, vkFramebuffer, vkPipeline);

		vkSyncObjects = new VkSyncObjects(vkContext);

		// Record the commands immediately (we only do this once for now)
		vkCommandManager->recordCommands(vkRenderPass, vkFramebuffer, vkPipeline, vkSwapchain->getExtent());
    }

    void drawFrame() {
        // Wait for the previous frame to finish
		vkWaitForFences(vkContext->getDevice(), 1, &vkSyncObjects->getInFlightFence(), VK_TRUE, UINT64_MAX);
		vkResetFences(vkContext->getDevice(), 1, &vkSyncObjects->getInFlightFence());

		// Get the next image from the Swapchain
		uint32_t imageIndex;
		vkAcquireNextImageKHR(vkContext->getDevice(), vkSwapchain->getSwapchain(), UINT64_MAX, 
			vkSyncObjects->getImageAvailableSemaphore(), VK_NULL_HANDLE, &imageIndex);

        // Submit to the Command Buffer
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = { vkSyncObjects->getImageAvailableSemaphore() };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;
        // We use the command buffer that matches the image index
		submitInfo.pCommandBuffers = &vkCommandManager->getCommandBuffer(imageIndex);

		VkSemaphore signalSemaphores[] = { vkSyncObjects->getRenderFinishedSemaphore() };
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(vkContext->getGraphicsQueue(), 1, &submitInfo, vkSyncObjects->getInFlightFence())
            != VK_SUCCESS) {
			throw std::runtime_error("failed to submit draw command buffer!");
        }

		// Present the image
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = { vkSwapchain->getSwapchain() };
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &imageIndex;

		vkQueuePresentKHR(vkContext->getPresentQueue(), &presentInfo);
    }

    void mainLoop() {
        // Simple loop that keeps the window open
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
			drawFrame();
        }
		// Wait for the device to finish before exiting
		vkDeviceWaitIdle(vkContext->getDevice());
    }

    void cleanup() {
        delete vkSyncObjects;
        delete vkCommandManager;
		delete vkFramebuffer;
		delete vkPipeline;
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