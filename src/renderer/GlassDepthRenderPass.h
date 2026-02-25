#pragma once
#include <vulkan/vulkan.h>

namespace Iridium {

    class GlassDepthRenderPass {
    public:
        // Initializes the render pass. Pass in your chosen depth format (e.g., VK_FORMAT_D32_SFLOAT)
        void init(VkDevice logicalDevice, VkFormat depthFormat);

        // Destroys the Vulkan objects
        void cleanup();

        // Getter for pipeline creation and command buffer binding
        VkRenderPass getRenderPass() const { return renderPass; }

    private:
        VkDevice device = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
    };

} // namespace Iridium