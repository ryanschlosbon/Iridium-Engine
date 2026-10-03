#pragma once

#include "VkContext.h"
#include "VkSwapchain.h"
#include "renderer/rhi/GBufferLayout.h"
#include "VkRenderPass.h"
#include "utils/File.h"
#include <string>

class VkGraphicsPipeline {
public:
    // M7R R4a: the fixed wireframe/selection pipelines record with dynamic
    // rendering into the G-buffer attachments of `layout` (+ D32 depth).
	VkGraphicsPipeline(VkContext* context, VkSwapchain* swapchain,
        VkPipelineLayout pipelineLayout, Iridium::GBufferLayout layout);
    ~VkGraphicsPipeline();

    VkPipeline getWireframePipeline() { return wireframePipeline; }
    VkPipeline getOutlinePipeline() { return outlinePipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

private:
    VkContext* context;
    VkPipeline wireframePipeline;
    VkPipeline outlinePipeline;
    VkPipelineLayout pipelineLayout; // Holds "Global Variables" definitions

    // Helper to wrap shader code into a Vulkan module
    VkShaderModule createShaderModule(const std::vector<char>& code);
	VkPipeline createPipeline(VkSwapchain* swapchain,
        bool isWireframe, bool isOutline, Iridium::GBufferLayout layout);
};
