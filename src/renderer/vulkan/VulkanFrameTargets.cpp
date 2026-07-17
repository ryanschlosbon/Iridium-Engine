#include "VulkanFrameTargets.h"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace Iridium {

    namespace {

        [[noreturn]] void throwVkError(const char* operation, VkResult result) {
            throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                std::to_string(static_cast<int>(result)) + ".");
        }

    } // namespace

    VulkanFrameTargets::VulkanFrameTargets(VulkanFrameTargets&& other) noexcept
        : device_(other.device_),
          allocator_(other.allocator_),
          sampler_(other.sampler_),
          extent_(other.extent_),
          format_(other.format_),
          targets_(std::move(other.targets_)) {
        other.device_ = VK_NULL_HANDLE;
        other.allocator_ = nullptr;
        other.sampler_ = VK_NULL_HANDLE;
        other.extent_ = {};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.targets_.clear();
    }

    VulkanFrameTargets& VulkanFrameTargets::operator=(VulkanFrameTargets&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        cleanup();
        device_ = other.device_;
        allocator_ = other.allocator_;
        sampler_ = other.sampler_;
        extent_ = other.extent_;
        format_ = other.format_;
        targets_ = std::move(other.targets_);

        other.device_ = VK_NULL_HANDLE;
        other.allocator_ = nullptr;
        other.sampler_ = VK_NULL_HANDLE;
        other.extent_ = {};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.targets_.clear();
        return *this;
    }

    VulkanFrameTargets::~VulkanFrameTargets() {
        cleanup();
    }

    void VulkanFrameTargets::init(VkDevice device, VulkanResourceAllocator& allocator,
        const ::VkSwapchain& swapchain, VulkanTargetRenderPasses renderPasses) {
        if (device == VK_NULL_HANDLE || swapchain.getSwapchain() == VK_NULL_HANDLE ||
            swapchain.getImageCount() == 0 || swapchain.getImageViews().size() != swapchain.getImageCount()) {
            throw std::invalid_argument("VulkanFrameTargets requires a valid swapchain.");
        }
        if (renderPasses.gBuffer == VK_NULL_HANDLE || renderPasses.lighting == VK_NULL_HANDLE ||
            renderPasses.forward == VK_NULL_HANDLE || renderPasses.glassDepth == VK_NULL_HANDLE ||
            renderPasses.ui == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanFrameTargets requires all render passes.");
        }
        if (device_ != VK_NULL_HANDLE || allocator_ != nullptr || sampler_ != VK_NULL_HANDLE ||
            !targets_.empty()) {
            throw std::logic_error("VulkanFrameTargets was initialized more than once.");
        }

        device_ = device;
        allocator_ = &allocator;
        extent_ = swapchain.getExtent();
        format_ = swapchain.getImageFormat();
        targets_.resize(swapchain.getImageCount());

        try {
            for (VulkanPerImageTargets& target : targets_) {
                target.normal = allocator_->createImage2D(extent_,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                target.albedo = allocator_->createImage2D(extent_,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                target.emissive = allocator_->createImage2D(extent_,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                target.depth = allocator_->createImage2D(extent_, VK_FORMAT_D32_SFLOAT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                target.litScene = allocator_->createImage2D(extent_, format_,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                target.opaqueCopy = allocator_->createImage2D(extent_, format_,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                target.glassDepth = allocator_->createImage2D(extent_, VK_FORMAT_D32_SFLOAT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            }

            VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxAnisotropy = 1.0f;

            VkResult result = vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_);
            if (result != VK_SUCCESS) {
                throwVkError("vkCreateSampler", result);
            }

            for (size_t i = 0; i < targets_.size(); ++i) {
                VulkanPerImageTargets& target = targets_[i];
                const uint32_t width = extent_.width;
                const uint32_t height = extent_.height;

                const auto createFramebuffer = [&](VkRenderPass renderPass,
                    std::span<const VkImageView> attachments, VkFramebuffer& framebuffer,
                    const char* name) {
                    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                    info.renderPass = renderPass;
                    info.attachmentCount = static_cast<uint32_t>(attachments.size());
                    info.pAttachments = attachments.data();
                    info.width = width;
                    info.height = height;
                    info.layers = 1;
                    const VkResult framebufferResult = vkCreateFramebuffer(
                        device_, &info, nullptr, &framebuffer);
                    if (framebufferResult != VK_SUCCESS) {
                        throwVkError(name, framebufferResult);
                    }
                };

                const std::array<VkImageView, 4> gBufferAttachments = {
                    target.normal.view, target.albedo.view, target.emissive.view, target.depth.view };
                createFramebuffer(renderPasses.gBuffer, gBufferAttachments,
                    target.gBufferFramebuffer, "vkCreateFramebuffer(gBuffer)");

                const std::array<VkImageView, 1> lightingAttachments = { target.litScene.view };
                createFramebuffer(renderPasses.lighting, lightingAttachments,
                    target.lightingFramebuffer, "vkCreateFramebuffer(lighting)");

                const std::array<VkImageView, 2> forwardAttachments = {
                    target.litScene.view, target.depth.view };
                createFramebuffer(renderPasses.forward, forwardAttachments,
                    target.forwardFramebuffer, "vkCreateFramebuffer(forward)");

                const std::array<VkImageView, 1> glassDepthAttachments = { target.glassDepth.view };
                createFramebuffer(renderPasses.glassDepth, glassDepthAttachments,
                    target.glassDepthFramebuffer, "vkCreateFramebuffer(glassDepth)");

                const std::array<VkImageView, 1> uiAttachments = { swapchain.getImageViews()[i] };
                createFramebuffer(renderPasses.ui, uiAttachments,
                    target.uiFramebuffer, "vkCreateFramebuffer(ui)");
            }
        }
        catch (...) {
            cleanup();
            throw;
        }
    }

    void VulkanFrameTargets::cleanup() {
        if (device_ == VK_NULL_HANDLE) {
            targets_.clear();
            sampler_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            extent_ = {};
            format_ = VK_FORMAT_UNDEFINED;
            return;
        }

        for (VulkanPerImageTargets& target : targets_) {
            if (target.uiFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.uiFramebuffer, nullptr);
                target.uiFramebuffer = VK_NULL_HANDLE;
            }
            if (target.glassDepthFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.glassDepthFramebuffer, nullptr);
                target.glassDepthFramebuffer = VK_NULL_HANDLE;
            }
            if (target.forwardFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.forwardFramebuffer, nullptr);
                target.forwardFramebuffer = VK_NULL_HANDLE;
            }
            if (target.lightingFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.lightingFramebuffer, nullptr);
                target.lightingFramebuffer = VK_NULL_HANDLE;
            }
            if (target.gBufferFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.gBufferFramebuffer, nullptr);
                target.gBufferFramebuffer = VK_NULL_HANDLE;
            }
        }

        if (allocator_ != nullptr) {
            for (VulkanPerImageTargets& target : targets_) {
                allocator_->destroy(target.glassDepth);
                allocator_->destroy(target.opaqueCopy);
                allocator_->destroy(target.litScene);
                allocator_->destroy(target.depth);
                allocator_->destroy(target.emissive);
                allocator_->destroy(target.albedo);
                allocator_->destroy(target.normal);
            }
        }

        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }

        targets_.clear();
        device_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        extent_ = {};
        format_ = VK_FORMAT_UNDEFINED;
    }

    size_t VulkanFrameTargets::size() const noexcept {
        return targets_.size();
    }

    VulkanPerImageTargets& VulkanFrameTargets::get(size_t index) {
        return targets_.at(index);
    }

    const VulkanPerImageTargets& VulkanFrameTargets::get(size_t index) const {
        return targets_.at(index);
    }

    VkSampler VulkanFrameTargets::sampler() const noexcept {
        return sampler_;
    }

    VkExtent2D VulkanFrameTargets::extent() const noexcept {
        return extent_;
    }

    VkFormat VulkanFrameTargets::format() const noexcept {
        return format_;
    }

    std::span<VulkanPerImageTargets> VulkanFrameTargets::targets() noexcept {
        return targets_;
    }

    std::span<const VulkanPerImageTargets> VulkanFrameTargets::targets() const noexcept {
        return targets_;
    }

} // namespace Iridium
