#pragma once

#include <vulkan/vulkan.h>
#include "VkContext.h"

class VkUIRenderPass {
public:
    VkUIRenderPass(VkContext* context, VkFormat imageFormat,
        bool presentAfterPass = true);
    ~VkUIRenderPass();

    VkRenderPass getRenderPass() const { return renderPass; }

private:
    VkContext* context;
    VkRenderPass renderPass;
};
