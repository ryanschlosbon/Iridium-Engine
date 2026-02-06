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
	VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptorSetLayout; }

private:
    VkContext* context;
    VkPipeline graphicsPipeline;
    VkPipelineLayout pipelineLayout; // Holds "Global Variables" definitions
	VkDescriptorSetLayout descriptorSetLayout; // Defines how to connect shader resources (uniform buffers, textures, etc.)

    // Helper to wrap shader code into a Vulkan module
    VkShaderModule createShaderModule(const std::vector<char>& code);

    // The main builder
    void createGraphicsPipeline(VkSwapchain* swapchain, VkRenderPassWrapper* renderPass);
};