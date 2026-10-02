#pragma once
#include <vulkan/vulkan.h>

class VkContext;

class VkForwardRenderPass {
public:
    VkForwardRenderPass(VkContext* context, VkFormat colorFormat,
        VkFormat depthFormat, bool depthReadOnly = false);
    ~VkForwardRenderPass();

    VkRenderPass getRenderPass() const { return renderPass; }

    // Vulkan 1.3 NONE leaves an unmodified read-only attachment untouched.
    // DONT_CARE would discard it; STORE introduces a depth write at pass end.
    static constexpr VkAttachmentStoreOp depthStoreOperation(bool depthReadOnly) {
        return depthReadOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    }

private:
    VkContext* context;
    VkRenderPass renderPass;

    void createRenderPass(VkFormat colorFormat, VkFormat depthFormat,
        bool depthReadOnly);
};
