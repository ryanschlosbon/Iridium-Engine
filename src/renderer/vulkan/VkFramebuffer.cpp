#include "VkFramebuffer.h"
#include <stdexcept>
#include <array>

VkFramebufferWrapper::VkFramebufferWrapper(VkContext* context, VkRenderPassWrapper* renderPass,
    const std::vector<VkImageView>& normalImageViews,
    const std::vector<VkImageView>& albedoImageViews,
    const std::vector<VkImageView>& depthImageViews,
    VkExtent2D extent) : context(context) {

    framebuffers.resize(albedoImageViews.size());

    for (size_t i = 0; i < albedoImageViews.size(); i++) {
        // MUST MATCH THE ORDER IN VKRENDERPASS EXACLTY (Pos, Norm, Albedo, Depth)
        std::array<VkImageView, 3> attachments = {
            normalImageViews[i],
            albedoImageViews[i],
            depthImageViews[i]
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass->getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(context->getDevice(), &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("failed to create deferred framebuffer!");
        }
    }
}

VkFramebufferWrapper::~VkFramebufferWrapper() {
    for (auto framebuffer : framebuffers) {
        vkDestroyFramebuffer(context->getDevice(), framebuffer, nullptr);
    }
}