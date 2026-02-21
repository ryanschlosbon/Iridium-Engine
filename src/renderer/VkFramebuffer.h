#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include "VkContext.h"
#include "VkSwapchain.h"
#include "VkRenderPass.h"

class VkFramebufferWrapper {
public:
    VkFramebufferWrapper(VkContext* context, VkRenderPassWrapper* renderPass,
        const std::vector<VkImageView>& colorImageViews,
        const std::vector<VkImageView>& depthImageViews,
        VkExtent2D extent);
    ~VkFramebufferWrapper();

    VkFramebuffer getFramebuffer(int index) const { return framebuffers[index]; }

private:
    VkContext* context;
    std::vector<VkFramebuffer> framebuffers;
};