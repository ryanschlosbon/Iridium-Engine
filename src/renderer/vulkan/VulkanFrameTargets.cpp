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
          targets_(std::move(other.targets_)) {
        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.pyramidSampler_ = VK_NULL_HANDLE;
        other.depthPyramidSampler_ = VK_NULL_HANDLE;
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
        sampler_ = other.sampler_;
        integerSampler_ = other.integerSampler_;
        pyramidSampler_ = other.pyramidSampler_;
        depthPyramidSampler_ = other.depthPyramidSampler_;
        extent_ = other.extent_;
        format_ = other.format_;
        targets_ = std::move(other.targets_);

        other.device_ = VK_NULL_HANDLE;
        other.sampler_ = VK_NULL_HANDLE;
        other.integerSampler_ = VK_NULL_HANDLE;
        other.pyramidSampler_ = VK_NULL_HANDLE;
        other.depthPyramidSampler_ = VK_NULL_HANDLE;
        other.extent_ = {};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.targets_.clear();
        return *this;
    }

    VulkanFrameTargets::~VulkanFrameTargets() {
        cleanup();
    }

    void VulkanFrameTargets::init(VkDevice device, VkExtent2D sceneExtent,
        uint32_t frameContextCount,
        bool hdr10Composition,
        bool transparencyPyramids,
        const VulkanLayeredGraphConfig& layered,
        const VulkanRenderGraphExecutor& graphResources,
        const VulkanProductionGraphIds& ids) {
        if (device == VK_NULL_HANDLE || frameContextCount == 0) {
            throw std::invalid_argument(
                "VulkanFrameTargets requires a device and frame contexts.");
        }
        if (sceneExtent.width == 0 || sceneExtent.height == 0) {
            throw std::invalid_argument(
                "VulkanFrameTargets requires a non-empty scene extent.");
        }
        const bool ordinary2 = layered.enabled(TransparencyQuality::Ordinary2);
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
        if (device_ != VK_NULL_HANDLE || sampler_ != VK_NULL_HANDLE ||
            integerSampler_ != VK_NULL_HANDLE ||
            pyramidSampler_ != VK_NULL_HANDLE ||
            depthPyramidSampler_ != VK_NULL_HANDLE ||
            !targets_.empty()) {
            throw std::logic_error("VulkanFrameTargets was initialized more than once.");
        }

        device_ = device;
        extent_ = sceneExtent;
        format_ = VulkanSceneColorFormat;
        targets_.resize(frameContextCount);

        try {
            for (VulkanFrameContextTargets& target : targets_) {
                const uint32_t frameIndex = static_cast<uint32_t>(
                    &target - targets_.data());
                const auto image = [&](RenderGraph::GraphResourceId id) {
                    return graphResources.image(frameIndex, id);
                };
                target.normal = image(ids.gbufferNormal);
                target.albedo = image(ids.gbufferAlbedo);
                target.emissive = image(ids.gbufferEmissive);
                target.f0Roughness = image(ids.gbufferF0Roughness);
                target.materialFlags = image(ids.gbufferMaterialFlags);
                target.depth = image(ids.depth);
                target.litScene = image(ids.sceneColor);
                if (transparencyPyramids) {
                    target.refractionColorPyramid = image(ids.refractionColorPyramid);
                    target.refractionDepthPyramid = image(ids.refractionDepthPyramid);
                }
                if (ordinary2) {
                    target.layeredEntryDepth = image(ids.ordinary2EntryDepth);
                    target.layeredEntryIdentity = image(ids.ordinary2EntryIdentity);
                    target.layeredExitDepth = image(ids.ordinary2ExitDepth);
                    target.layeredExitIdentity = image(ids.ordinary2ExitIdentity);
                    target.layeredLocalColor = image(ids.ordinary2LocalColor);
                }
                const auto acquireDeepLayeredTier = [&](
                        TransparencyQuality quality,
                        const VulkanDeepLayeredGraphIds& tierIds,
                        VulkanFrameContextTargets::DeepLayeredTier& tier) {
                    if (!layered.enabled(quality)) return;
                    tier.atlasExtent = layered.atlasExtent(quality);
                    tier.interfaceCount = layeredQualityTierContract(
                        quality).maximumInterfaceCount;
                    for (uint32_t interfaceIndex = 0u;
                        interfaceIndex < tier.interfaceCount;
                        ++interfaceIndex) {
                        tier.interfaceDepth[interfaceIndex] =
                            image(tierIds.interfaceDepth[interfaceIndex]);
                        tier.interfaceIdentity[interfaceIndex] =
                            image(tierIds.interfaceIdentity[interfaceIndex]);
                        if (deepLayeredTerminationInterface(interfaceIndex,
                                tier.interfaceCount)) {
                            tier.tileTermination[interfaceIndex] =
                                image(tierIds.tileTermination[interfaceIndex]);
                        }
                    }
                    tier.localColor = image(tierIds.localColor);
                };
                acquireDeepLayeredTier(TransparencyQuality::Hero4, ids.hero4,
                    target.hero4);
                acquireDeepLayeredTier(TransparencyQuality::Cinematic8,
                    ids.cinematic8, target.cinematic8);
                if (layered.weightedOit) {
                    target.weightedOitAccumulation = image(ids.oitAccumulation);
                    target.weightedOitRevealage = image(ids.oitRevealage);
                }
                target.output = image(ids.output);
            }
            if (hdr10Composition) {
                for (VulkanFrameContextTargets& target : targets_) {
                    const uint32_t frameIndex = static_cast<uint32_t>(
                        &target - targets_.data());
                    target.uiComposition = graphResources.image(frameIndex,
                        ids.uiComposition);
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

            for (VulkanFrameContextTargets& target : targets_) {
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
            integerSampler_ = VK_NULL_HANDLE;
            pyramidSampler_ = VK_NULL_HANDLE;
            depthPyramidSampler_ = VK_NULL_HANDLE;
            extent_ = {};
            format_ = VK_FORMAT_UNDEFINED;
            return;
        }

        for (VulkanFrameContextTargets& target : targets_) {
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

    std::span<VulkanFrameContextTargets> VulkanFrameTargets::targets() noexcept {
        return targets_;
    }

    std::span<const VulkanFrameContextTargets> VulkanFrameTargets::targets() const noexcept {
        return targets_;
    }

} // namespace Iridium
