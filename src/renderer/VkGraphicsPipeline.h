#pragma once

#include "VkContext.h"
#include "VkSwapchain.h"
#include "VkRenderPass.h"
#include "utils/File.h"
#include <string>

class VkGraphicsPipeline {
public:
    VkGraphicsPipeline(VkContext* context, VkSwapchain* swapchain, VkRenderPassWrapper* renderPass);
    ~VkGraphicsPipeline();

    VkPipeline getPipeline() const { return graphicsPipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }
    VkDescriptorSetLayout getGlobalSetLayout() { return globalSetLayout; }
    VkDescriptorSetLayout getMaterialSetLayout() { return materialSetLayout; }

private:
    VkContext* context;
    VkPipeline graphicsPipeline;
    VkPipelineLayout pipelineLayout; // Holds "Global Variables" definitions
    VkDescriptorSetLayout globalSetLayout;   // Set 0 (Camera/UBO)
    VkDescriptorSetLayout materialSetLayout; // Set 1 (Texture/Sampler)

    // Helper to wrap shader code into a Vulkan module
    VkShaderModule createShaderModule(const std::vector<char>& code);

    // The main builder
    void createGraphicsPipeline(VkSwapchain* swapchain, VkRenderPassWrapper* renderPass);
};