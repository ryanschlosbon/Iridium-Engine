#pragma once
#include "VkContext.h"
#include <vulkan/vulkan.h>

class VkForwardRenderPass {
public:
    VkForwardRenderPass(VkContext* context, VkFormat colorFormat, VkFormat depthFormat);
    ~VkForwardRenderPass();

    VkRenderPass getRenderPass() const { return renderPass; }

private:
    VkContext* context;
    VkRenderPass renderPass;

    void createRenderPass(VkFormat colorFormat, VkFormat depthFormat);
};