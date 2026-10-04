#pragma once

#include "VkContext.h"
#include "VkSwapchain.h"
#include "renderer/rhi/GBufferLayout.h"
#include "utils/File.h"
#include <string>

class VkGraphicsPipeline {
public:
    // M7R R4a: the fixed wireframe/selection pipelines record with dynamic
    // rendering into the G-buffer attachments of `layout` (+ D32 depth).
	VkGraphicsPipeline(VkContext* context, VkPipelineCache pipelineCache,
        VkSwapchain* swapchain, VkPipelineLayout pipelineLayout,
        Iridium::GBufferLayout layout);
    ~VkGraphicsPipeline();

    VkPipeline getWireframePipeline() { return wireframePipeline; }
    // M7R R5c.4f: the same wireframe state with the GPU-scene vertex shader
    // (transforms from the published records, gl_InstanceIndex = primitive)
    // for indirect bins.
    VkPipeline getWireframeIndirectPipeline() { return wireframeIndirectPipeline; }
    VkPipeline getOutlinePipeline() { return outlinePipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }

private:
    VkContext* context;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkPipeline wireframePipeline;
    VkPipeline wireframeIndirectPipeline;
    VkPipeline outlinePipeline;
    VkPipelineLayout pipelineLayout; // Holds "Global Variables" definitions

    // Helper to wrap shader code into a Vulkan module
    VkShaderModule createShaderModule(const std::vector<char>& code);
	VkPipeline createPipeline(VkSwapchain* swapchain,
        bool isWireframe, bool isOutline, Iridium::GBufferLayout layout,
        const char* vertexShader = "assets/shaders/canonical_material_vert.spv");
};
