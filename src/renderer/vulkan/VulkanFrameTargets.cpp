#include "VulkanFrameTargets.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanGBufferLayout.h"
#include "VulkanProductionRenderGraph.h"

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
          pyramidSampler_(other.pyramidSampler_),
          depthPyramidSampler_(other.depthPyramidSampler_),
          extent_(other.extent_),
          format_(other.format_),
          targets_(std::move(other.targets_)),
          uiFramebuffers_(std::move(other.uiFramebuffers_)) {
        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.pyramidSampler_ = VK_NULL_HANDLE;
        other.depthPyramidSampler_ = VK_NULL_HANDLE;
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
        pyramidSampler_ = other.pyramidSampler_;
        depthPyramidSampler_ = other.depthPyramidSampler_;
        extent_ = other.extent_;
        format_ = other.format_;
        targets_ = std::move(other.targets_);
        uiFramebuffers_ = std::move(other.uiFramebuffers_);

        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.pyramidSampler_ = VK_NULL_HANDLE;
        other.depthPyramidSampler_ = VK_NULL_HANDLE;
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
        bool transparencyPyramids,
        const VulkanLayeredGraphConfig& layered,
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
            renderPasses.forward == VK_NULL_HANDLE ||
            renderPasses.transparent == VK_NULL_HANDLE ||
            renderPasses.glassDepth == VK_NULL_HANDLE ||
            renderPasses.output == VK_NULL_HANDLE || renderPasses.ui == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanFrameTargets requires all render passes.");
        }
        const VkExtent2D ordinary2AtlasExtent = layered.atlasExtent(
            TransparencyQuality::Ordinary2);
        const bool ordinary2 = layered.enabled(TransparencyQuality::Ordinary2);
        const bool anyLayered = layered.anyEnabled();
        for (const TransparencyQuality quality : {
                TransparencyQuality::Ordinary2,
                TransparencyQuality::Hero4,
                TransparencyQuality::Cinematic8 }) {
            const VkExtent2D atlasExtent = layered.atlasExtent(quality);
            if (layered.enabled(quality) && (atlasExtent.width == 0u ||
                    atlasExtent.height == 0u)) {
                throw std::invalid_argument(
                    "VulkanFrameTargets requires complete layered atlas extents.");
            }
        }
        if (anyLayered &&
            (renderPasses.layeredInterfaceCapture == VK_NULL_HANDLE ||
                renderPasses.layeredLocalComposition == VK_NULL_HANDLE)) {
            throw std::invalid_argument(
                "VulkanFrameTargets requires complete layered render passes.");
        }
        if (device_ != VK_NULL_HANDLE || sampler_ != VK_NULL_HANDLE ||
            integerSampler_ != VK_NULL_HANDLE ||
            pyramidSampler_ != VK_NULL_HANDLE ||
            depthPyramidSampler_ != VK_NULL_HANDLE ||
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
                if (transparencyPyramids) {
                    target.refractionColorPyramid = graphResources.imageResource(
                        frameIndex, "scene.refraction-color-pyramid");
                    target.refractionDepthPyramid = graphResources.imageResource(
                        frameIndex, "depth.refraction-nearest-pyramid");
                }
                target.glassDepth = graphResources.imageResource(frameIndex, "depth.glass");
                if (ordinary2) {
                    target.layeredEntryDepth = graphResources.imageResource(
                        frameIndex, "depth.layered.entry");
                    target.layeredEntryIdentity = graphResources.imageResource(
                        frameIndex, "identity.layered.entry");
                    target.layeredExitDepth = graphResources.imageResource(
                        frameIndex, "depth.layered.exit");
                    target.layeredExitIdentity = graphResources.imageResource(
                        frameIndex, "identity.layered.exit");
                    target.layeredLocalColor = graphResources.imageResource(
                        frameIndex, "scene.layered.local-color");
                }
                const auto acquireDeepLayeredTier = [&](
                        TransparencyQuality quality, const char* name,
                        VulkanFrameContextTargets::DeepLayeredTier& tier) {
                    if (!layered.enabled(quality)) return;
                    tier.atlasExtent = layered.atlasExtent(quality);
                    tier.interfaceCount = layeredQualityTierContract(
                        quality).maximumInterfaceCount;
                    for (uint32_t interfaceIndex = 0u;
                        interfaceIndex < tier.interfaceCount;
                        ++interfaceIndex) {
                        const std::string suffix = std::string(name) +
                            ".interface." + std::to_string(interfaceIndex);
                        tier.interfaceDepth[interfaceIndex] =
                            graphResources.imageResource(frameIndex,
                                "depth.layered." + suffix);
                        tier.interfaceIdentity[interfaceIndex] =
                            graphResources.imageResource(frameIndex,
                                "identity.layered." + suffix);
                        if (deepLayeredTerminationInterface(interfaceIndex,
                                tier.interfaceCount)) {
                            tier.tileTermination[interfaceIndex] =
                                graphResources.imageResource(frameIndex,
                                    "termination.layered." + suffix);
                        }
                    }
                    tier.localColor = graphResources.imageResource(frameIndex,
                        std::string("scene.layered.") + name +
                            ".local-color");
                };
                acquireDeepLayeredTier(TransparencyQuality::Hero4, "hero4",
                    target.hero4);
                acquireDeepLayeredTier(TransparencyQuality::Cinematic8,
                    "cinematic8", target.cinematic8);
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
            if (transparencyPyramids) {
                samplerInfo.magFilter = VK_FILTER_LINEAR;
                samplerInfo.minFilter = VK_FILTER_LINEAR;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerInfo.minLod = 0.0f;
                samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
                result = vkCreateSampler(device_, &samplerInfo, nullptr,
                    &pyramidSampler_);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateSampler(refraction pyramid)", result);
                }
                samplerInfo.magFilter = VK_FILTER_NEAREST;
                samplerInfo.minFilter = VK_FILTER_NEAREST;
                samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                result = vkCreateSampler(device_, &samplerInfo, nullptr,
                    &depthPyramidSampler_);
                if (result != VK_SUCCESS) {
                    throwVkError("vkCreateSampler(refraction depth pyramid)",
                        result);
                }
            }

            for (size_t i = 0; i < targets_.size(); ++i) {
                VulkanFrameContextTargets& target = targets_[i];
                const uint32_t width = extent_.width;
                const uint32_t height = extent_.height;

                const auto createMipViews = [&](const VulkanImageResource& image,
                        std::vector<VkImageView>& views,
                        const char* operation) {
                    views.resize(image.mipLevels, VK_NULL_HANDLE);
                    for (uint32_t mip = 0; mip < image.mipLevels; ++mip) {
                        VkImageViewCreateInfo info{
                            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                        info.image = image.image;
                        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        info.format = image.format;
                        info.subresourceRange = {
                            VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
                        const VkResult viewResult = vkCreateImageView(device_,
                            &info, nullptr, &views[mip]);
                        if (viewResult != VK_SUCCESS)
                            throwVkError(operation, viewResult);
                    }
                };
                if (transparencyPyramids) {
                    createMipViews(target.refractionColorPyramid,
                        target.refractionColorMipViews,
                        "vkCreateImageView(refraction color mip)");
                    createMipViews(target.refractionDepthPyramid,
                        target.refractionDepthMipViews,
                        "vkCreateImageView(refraction depth mip)");
                }

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
                createFramebuffer(renderPasses.transparent, forwardAttachments,
                    target.transparentFramebuffer,
                    "vkCreateFramebuffer(transparent)");

                const std::array<VkImageView, 1> glassDepthAttachments = { target.glassDepth.view };
                createFramebuffer(renderPasses.glassDepth, glassDepthAttachments,
                    target.glassDepthFramebuffer, "vkCreateFramebuffer(glassDepth)");

                if (ordinary2) {
                    const auto createLayeredFramebuffer = [&](VkImageView identity,
                            VkImageView depth, VkFramebuffer& framebuffer,
                            const char* name) {
                        const std::array<VkImageView, 2> attachments{
                            identity, depth };
                        VkFramebufferCreateInfo info{
                            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                        info.renderPass = renderPasses.layeredInterfaceCapture;
                        info.attachmentCount =
                            static_cast<uint32_t>(attachments.size());
                        info.pAttachments = attachments.data();
                        info.width = ordinary2AtlasExtent.width;
                        info.height = ordinary2AtlasExtent.height;
                        info.layers = 1u;
                        const VkResult result = vkCreateFramebuffer(
                            device_, &info, nullptr, &framebuffer);
                        if (result != VK_SUCCESS)
                            throwVkError(name, result);
                    };
                    createLayeredFramebuffer(target.layeredEntryIdentity.view,
                        target.layeredEntryDepth.view,
                        target.layeredEntryFramebuffer,
                        "vkCreateFramebuffer(layered entry)");
                    createLayeredFramebuffer(target.layeredExitIdentity.view,
                        target.layeredExitDepth.view,
                        target.layeredExitFramebuffer,
                        "vkCreateFramebuffer(layered exit)");
                    const std::array<VkImageView, 1> localAttachments{
                        target.layeredLocalColor.view };
                    VkFramebufferCreateInfo localInfo{
                        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                    localInfo.renderPass =
                        renderPasses.layeredLocalComposition;
                    localInfo.attachmentCount =
                        static_cast<uint32_t>(localAttachments.size());
                    localInfo.pAttachments = localAttachments.data();
                    localInfo.width = ordinary2AtlasExtent.width;
                    localInfo.height = ordinary2AtlasExtent.height;
                    localInfo.layers = 1u;
                    const VkResult localResult = vkCreateFramebuffer(device_,
                        &localInfo, nullptr,
                        &target.layeredLocalCompositionFramebuffer);
                    if (localResult != VK_SUCCESS)
                        throwVkError(
                            "vkCreateFramebuffer(layered local composition)",
                            localResult);
                }

                const auto createDeepLayeredTierFramebuffers = [&]
                    (VulkanFrameContextTargets::DeepLayeredTier& tier,
                        const char* tierName) {
                    if (!tier.active()) return;
                    for (uint32_t interfaceIndex = 0u;
                        interfaceIndex < tier.interfaceCount;
                        ++interfaceIndex) {
                        const std::array<VkImageView, 2> attachments{
                            tier.interfaceIdentity[interfaceIndex].view,
                            tier.interfaceDepth[interfaceIndex].view };
                        VkFramebufferCreateInfo info{
                            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                        info.renderPass =
                            renderPasses.layeredInterfaceCapture;
                        info.attachmentCount = static_cast<uint32_t>(
                            attachments.size());
                        info.pAttachments = attachments.data();
                        info.width = tier.atlasExtent.width;
                        info.height = tier.atlasExtent.height;
                        info.layers = 1u;
                        const VkResult result = vkCreateFramebuffer(device_,
                            &info, nullptr,
                            &tier.interfaceFramebuffers[interfaceIndex]);
                        if (result != VK_SUCCESS) {
                            const std::string operation = std::string(
                                "vkCreateFramebuffer(layered ") + tierName +
                                " interface)";
                            throwVkError(operation.c_str(), result);
                        }
                    }
                    const std::array<VkImageView, 1> attachments{
                        tier.localColor.view };
                    VkFramebufferCreateInfo info{
                        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                    info.renderPass = renderPasses.layeredLocalComposition;
                    info.attachmentCount = static_cast<uint32_t>(
                        attachments.size());
                    info.pAttachments = attachments.data();
                    info.width = tier.atlasExtent.width;
                    info.height = tier.atlasExtent.height;
                    info.layers = 1u;
                    const VkResult result = vkCreateFramebuffer(device_,
                        &info, nullptr,
                        &tier.localCompositionFramebuffer);
                    if (result != VK_SUCCESS) {
                        const std::string operation = std::string(
                            "vkCreateFramebuffer(layered ") + tierName +
                            " local composition)";
                        throwVkError(operation.c_str(), result);
                    }
                };
                createDeepLayeredTierFramebuffers(target.hero4, "hero4");
                createDeepLayeredTierFramebuffers(target.cinematic8,
                    "cinematic8");

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
            pyramidSampler_ = VK_NULL_HANDLE;
            depthPyramidSampler_ = VK_NULL_HANDLE;
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
            const auto destroyDeepLayeredTier = [&](
                    VulkanFrameContextTargets::DeepLayeredTier& tier) {
                if (tier.localCompositionFramebuffer != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(device_,
                        tier.localCompositionFramebuffer, nullptr);
                    tier.localCompositionFramebuffer = VK_NULL_HANDLE;
                }
                for (VkFramebuffer& framebuffer :
                        tier.interfaceFramebuffers) {
                    if (framebuffer != VK_NULL_HANDLE)
                        vkDestroyFramebuffer(device_, framebuffer, nullptr);
                    framebuffer = VK_NULL_HANDLE;
                }
                tier.atlasExtent = {};
                tier.interfaceCount = 0u;
            };
            destroyDeepLayeredTier(target.cinematic8);
            destroyDeepLayeredTier(target.hero4);
            if (target.layeredLocalCompositionFramebuffer !=
                    VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_,
                    target.layeredLocalCompositionFramebuffer, nullptr);
                target.layeredLocalCompositionFramebuffer = VK_NULL_HANDLE;
            }
            if (target.layeredExitFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.layeredExitFramebuffer,
                    nullptr);
                target.layeredExitFramebuffer = VK_NULL_HANDLE;
            }
            if (target.layeredEntryFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.layeredEntryFramebuffer,
                    nullptr);
                target.layeredEntryFramebuffer = VK_NULL_HANDLE;
            }
            for (VkImageView& view : target.refractionColorMipViews) {
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device_, view, nullptr);
                view = VK_NULL_HANDLE;
            }
            target.refractionColorMipViews.clear();
            for (VkImageView& view : target.refractionDepthMipViews) {
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device_, view, nullptr);
                view = VK_NULL_HANDLE;
            }
            target.refractionDepthMipViews.clear();
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
            if (target.transparentFramebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, target.transparentFramebuffer,
                    nullptr);
                target.transparentFramebuffer = VK_NULL_HANDLE;
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
        if (pyramidSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, pyramidSampler_, nullptr);
            pyramidSampler_ = VK_NULL_HANDLE;
        }
        if (depthPyramidSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, depthPyramidSampler_, nullptr);
            depthPyramidSampler_ = VK_NULL_HANDLE;
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

    VkSampler VulkanFrameTargets::pyramidSampler() const noexcept {
        return pyramidSampler_;
    }

    VkSampler VulkanFrameTargets::depthPyramidSampler() const noexcept {
        return depthPyramidSampler_;
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
