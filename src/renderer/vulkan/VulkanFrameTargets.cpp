#include "VulkanFrameTargets.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanGBufferLayout.h"

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
          sampler_(other.sampler_),
          integerSampler_(other.integerSampler_),
          extent_(other.extent_),
          format_(other.format_),
          targets_(std::move(other.targets_)),
          uiFramebuffers_(std::move(other.uiFramebuffers_)) {
        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.extent_ = {};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.targets_.clear();
        other.uiFramebuffers_.clear();
    }

    VulkanFrameTargets& VulkanFrameTargets::operator=(VulkanFrameTargets&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        cleanup();
        device_ = other.device_;
        sampler_ = other.sampler_;
        integerSampler_ = other.integerSampler_;
        extent_ = other.extent_;
        format_ = other.format_;
        targets_ = std::move(other.targets_);
        uiFramebuffers_ = std::move(other.uiFramebuffers_);

        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.extent_ = {};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.targets_.clear();
        other.uiFramebuffers_.clear();
        return *this;
    }

    VulkanFrameTargets::~VulkanFrameTargets() {
        cleanup();
    }

    void VulkanFrameTargets::init(VkDevice device,
        const ::VkSwapchain& swapchain, VkExtent2D sceneExtent,
        VulkanTargetRenderPasses renderPasses,
        uint32_t frameContextCount,
        bool hdr10Composition,
        const VulkanRenderGraphExecutor& graphResources) {
        if (device == VK_NULL_HANDLE || swapchain.getSwapchain() == VK_NULL_HANDLE ||
            swapchain.getImageCount() == 0 ||
            swapchain.getImageViews().size() != swapchain.getImageCount() ||
            frameContextCount == 0) {
            throw std::invalid_argument("VulkanFrameTargets requires a valid swapchain.");
        }
        if (sceneExtent.width == 0 || sceneExtent.height == 0) {
            throw std::invalid_argument(
                "VulkanFrameTargets requires a non-empty scene extent.");
        }
        if (renderPasses.gBuffer == VK_NULL_HANDLE || renderPasses.lighting == VK_NULL_HANDLE ||
            renderPasses.forward == VK_NULL_HANDLE || renderPasses.glassDepth == VK_NULL_HANDLE ||
            renderPasses.output == VK_NULL_HANDLE || renderPasses.ui == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanFrameTargets requires all render passes.");
        }
        if (device_ != VK_NULL_HANDLE || sampler_ != VK_NULL_HANDLE ||
            integerSampler_ != VK_NULL_HANDLE ||
            !targets_.empty() || !uiFramebuffers_.empty()) {
            throw std::logic_error("VulkanFrameTargets was initialized more than once.");
        }

        device_ = device;
        extent_ = sceneExtent;
        format_ = VulkanSceneColorFormat;
        targets_.resize(frameContextCount);
        uiFramebuffers_.resize(swapchain.getImageCount(), VK_NULL_HANDLE);

        try {
            for (VulkanFrameContextTargets& target : targets_) {
                const uint32_t frameIndex = static_cast<uint32_t>(
                    &target - targets_.data());
                target.normal = graphResources.imageResource(frameIndex, "gbuffer.normal");
                target.albedo = graphResources.imageResource(frameIndex, "gbuffer.albedo");
                target.emissive = graphResources.imageResource(frameIndex, "gbuffer.emissive");
                target.f0Roughness = graphResources.imageResource(
                    frameIndex, "gbuffer.f0-roughness");
                target.materialFlags = graphResources.imageResource(
                    frameIndex, "gbuffer.material-flags");
                target.depth = graphResources.imageResource(frameIndex, "depth.opaque");
                target.litScene = graphResources.imageResource(frameIndex, "scene.color");
                target.opaqueCopy = graphResources.imageResource(
                    frameIndex, "scene.opaque-copy");
                target.glassDepth = graphResources.imageResource(frameIndex, "depth.glass");
                target.output = graphResources.imageResource(frameIndex, "output.display");
            }
            if (hdr10Composition) {
                for (VulkanFrameContextTargets& target : targets_) {
                    const uint32_t frameIndex = static_cast<uint32_t>(
                        &target - targets_.data());
                    target.uiComposition = graphResources.imageResource(
                        frameIndex, "output.ui-composition");
                }
            }

            VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxAnisotropy = 1.0f;

            VkResult result = vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_);
            if (result != VK_SUCCESS) {
                throwVkError("vkCreateSampler", result);
            }
            samplerInfo.magFilter = VK_FILTER_NEAREST;
            samplerInfo.minFilter = VK_FILTER_NEAREST;
            result = vkCreateSampler(device_, &samplerInfo, nullptr, &integerSampler_);
            if (result != VK_SUCCESS) {
                throwVkError("vkCreateSampler(integer)", result);
            }

            for (size_t i = 0; i < targets_.size(); ++i) {
                VulkanFrameContextTargets& target = targets_[i];
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

                const std::array<VkImageView, 6> gBufferAttachments = {
                    target.normal.view, target.albedo.view, target.emissive.view,
                    target.f0Roughness.view, target.materialFlags.view, target.depth.view };
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

                const std::array<VkImageView, 1> outputAttachments = {
                    target.output.view };
                createFramebuffer(renderPasses.output, outputAttachments,
                    target.outputFramebuffer, "vkCreateFramebuffer(output)");

                if (hdr10Composition) {
                    const std::array<VkImageView, 1> uiAttachments = {
                        target.uiComposition.view };
                    VkFramebufferCreateInfo info{
                        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                    info.renderPass = renderPasses.ui;
                    info.attachmentCount = static_cast<uint32_t>(
                        uiAttachments.size());
                    info.pAttachments = uiAttachments.data();
                    info.width = swapchain.getExtent().width;
                    info.height = swapchain.getExtent().height;
                    info.layers = 1;
                    const VkResult result = vkCreateFramebuffer(
                        device_, &info, nullptr,
                        &target.uiCompositionFramebuffer);
                    if (result != VK_SUCCESS) {
                        throwVkError(
                            "vkCreateFramebuffer(uiComposition)", result);
                    }
                }

            }
            if (!hdr10Composition) for (size_t i = 0; i < uiFramebuffers_.size(); ++i) {
                const std::array<VkImageView, 1> uiAttachments = {
                    swapchain.getImageViews()[i] };
                const uint32_t width = swapchain.getExtent().width;
                const uint32_t height = swapchain.getExtent().height;
                VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                info.renderPass = renderPasses.ui;
                info.attachmentCount = static_cast<uint32_t>(uiAttachments.size());
                info.pAttachments = uiAttachments.data();
                info.width = width;
                info.height = height;
                info.layers = 1;
                const VkResult result = vkCreateFramebuffer(device_, &info, nullptr,
                    &uiFramebuffers_[i]);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateFramebuffer(ui)", result);
                }
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
            uiFramebuffers_.clear();
            sampler_ = VK_NULL_HANDLE;
            integerSampler_ = VK_NULL_HANDLE;
            extent_ = {};
            format_ = VK_FORMAT_UNDEFINED;
            return;
        }

        for (VkFramebuffer& framebuffer : uiFramebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
                framebuffer = VK_NULL_HANDLE;
            }
        }
        uiFramebuffers_.clear();
        for (VulkanFrameContextTargets& target : targets_) {
            if (target.uiCompositionFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.uiCompositionFramebuffer, nullptr);
                target.uiCompositionFramebuffer = VK_NULL_HANDLE;
            }
            if (target.glassDepthFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.glassDepthFramebuffer, nullptr);
                target.glassDepthFramebuffer = VK_NULL_HANDLE;
            }
            if (target.outputFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.outputFramebuffer, nullptr);
                target.outputFramebuffer = VK_NULL_HANDLE;
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

        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        if (integerSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, integerSampler_, nullptr);
            integerSampler_ = VK_NULL_HANDLE;
        }

        targets_.clear();
        device_ = VK_NULL_HANDLE;
        extent_ = {};
        format_ = VK_FORMAT_UNDEFINED;
    }

    size_t VulkanFrameTargets::size() const noexcept {
        return targets_.size();
    }

    VulkanFrameContextTargets& VulkanFrameTargets::get(size_t index) {
        return targets_.at(index);
    }

    const VulkanFrameContextTargets& VulkanFrameTargets::get(size_t index) const {
        return targets_.at(index);
    }

    VkSampler VulkanFrameTargets::sampler() const noexcept {
        return sampler_;
    }

    VkSampler VulkanFrameTargets::integerSampler() const noexcept {
        return integerSampler_;
    }

    VkExtent2D VulkanFrameTargets::extent() const noexcept {
        return extent_;
    }

    VkFormat VulkanFrameTargets::format() const noexcept {
        return format_;
    }

    VkFramebuffer VulkanFrameTargets::uiFramebuffer(
        size_t swapchainImageIndex) const {
        return uiFramebuffers_.at(swapchainImageIndex);
    }

    size_t VulkanFrameTargets::uiFramebufferCount() const noexcept {
        return uiFramebuffers_.size();
    }

    std::span<VulkanFrameContextTargets> VulkanFrameTargets::targets() noexcept {
        return targets_;
    }

    std::span<const VulkanFrameContextTargets> VulkanFrameTargets::targets() const noexcept {
        return targets_;
    }

} // namespace Iridium
