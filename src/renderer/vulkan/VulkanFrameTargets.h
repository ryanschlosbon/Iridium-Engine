#pragma once

#include "VulkanResourceAllocator.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/transparency/LayeredGlass.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr VkFormat VulkanSceneColorFormat =
        VK_FORMAT_R16G16B16A16_SFLOAT;
    inline constexpr VkFormat VulkanSdrOutputFormat =
        VK_FORMAT_B8G8R8A8_SRGB;

    class VulkanRenderGraphExecutor;
    struct VulkanLayeredGraphConfig;
    struct VulkanProductionGraphIds;

    struct VulkanFrameContextTargets {
        VulkanImageResource normal;
        VulkanImageResource albedo;
        VulkanImageResource emissive;
        VulkanImageResource f0Roughness;
        VulkanImageResource materialFlags;
        VulkanImageResource depth;
        VulkanImageResource litScene;
        VulkanImageResource refractionColorPyramid;
        VulkanImageResource refractionDepthPyramid;
        VulkanImageResource layeredEntryDepth;
        VulkanImageResource layeredEntryIdentity;
        VulkanImageResource layeredExitDepth;
        VulkanImageResource layeredExitIdentity;
        VulkanImageResource layeredLocalColor;
        VulkanImageResource weightedOitAccumulation;
        VulkanImageResource weightedOitRevealage;
        VulkanImageResource output;
        VulkanImageResource uiComposition;
        std::vector<VkImageView> refractionColorMipViews;
        std::vector<VkImageView> refractionDepthMipViews;

        struct DeepLayeredTier {
            std::array<VulkanImageResource, kMaximumLayeredInterfaceCount>
                interfaceDepth{};
            std::array<VulkanImageResource, kMaximumLayeredInterfaceCount>
                interfaceIdentity{};
            std::array<VulkanImageResource, kMaximumLayeredInterfaceCount>
                tileTermination{};
            VulkanImageResource localColor;
            VkExtent2D atlasExtent{};
            uint32_t interfaceCount = 0u;

            [[nodiscard]] bool active() const noexcept {
                return interfaceCount != 0u;
            }
        };

        DeepLayeredTier hero4;
        DeepLayeredTier cinematic8;
    };

    // Holds the frame contexts' views of the graph-owned scene-sized images,
    // the refraction pyramids' per-mip views and the shared samplers. Passes
    // record with dynamic rendering (M7R R4a), so there are no render passes or
    // framebuffers. It never owns graph images, swapchain images/views,
    // descriptors, editor texture IDs, pipelines, or the allocator.
    class VulkanFrameTargets final {
    public:
        VulkanFrameTargets() = default;
        VulkanFrameTargets(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets& operator=(const VulkanFrameTargets&) = delete;
        VulkanFrameTargets(VulkanFrameTargets&& other) noexcept;
        VulkanFrameTargets& operator=(VulkanFrameTargets&& other) noexcept;

        // cleanup() is idempotent. Destruction may call it only while the stored
        // device is still valid.
        ~VulkanFrameTargets();

        // All owned images are created in ResourceState::Undefined.
        void init(VkDevice device, VkExtent2D sceneExtent,
            uint32_t frameContextCount,
            bool hdr10Composition, bool transparencyPyramids,
            const VulkanLayeredGraphConfig& layered,
            const VulkanRenderGraphExecutor& graphResources,
            const VulkanProductionGraphIds& ids);
        void cleanup();

        [[nodiscard]] size_t size() const noexcept;
        [[nodiscard]] VulkanFrameContextTargets& get(size_t index);
        [[nodiscard]] const VulkanFrameContextTargets& get(size_t index) const;
        [[nodiscard]] VkSampler sampler() const noexcept;
        [[nodiscard]] VkSampler integerSampler() const noexcept;
        [[nodiscard]] VkSampler pyramidSampler() const noexcept;
        [[nodiscard]] VkSampler depthPyramidSampler() const noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] VkFormat format() const noexcept;
        [[nodiscard]] std::span<VulkanFrameContextTargets> targets() noexcept;
        [[nodiscard]] std::span<const VulkanFrameContextTargets> targets() const noexcept;

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkSampler integerSampler_ = VK_NULL_HANDLE;
        VkSampler pyramidSampler_ = VK_NULL_HANDLE;
        VkSampler depthPyramidSampler_ = VK_NULL_HANDLE;
        VkExtent2D extent_{};
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        std::vector<VulkanFrameContextTargets> targets_;
    };

} // namespace Iridium
