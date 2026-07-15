#pragma once
#include <vulkan/vulkan.h>
#include <string>

namespace Iridium {

    class GlassDepthPipeline {
    public:
        // Takes the device, the render pass we just made, and your pipeline layout (which should hold your camera/model matrices)
        void init(VkDevice logicalDevice, VkRenderPass renderPass, VkPipelineLayout pipelineLayout, const std::string& vertexShaderPath);
        void cleanup();

        VkPipeline getPipeline() const { return pipeline; }

    private:
        VkDevice device = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        // Helper to load the compiled SPIR-V shader
        VkShaderModule createShaderModule(const std::string& filepath);
    };

} // namespace Iridium