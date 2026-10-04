#pragma once

#include "VulkanResourceAllocator.h"
#include "renderer/rhi/ReflectionProbeCapture.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanFrameScheduler;

    struct VulkanReflectionProbeCaptureTargetConfig {
        uint32_t maximumOwners = 64;
        uint32_t maximumCapturesInFlight = 4;
        uint64_t maximumStagingBytes = 3ull * 1024ull * 1024ull * 1024ull;
        uint64_t maximumPublishedBytes = 4ull * 1024ull * 1024ull * 1024ull;
    };

    struct VulkanReflectionProbeCaptureStaging {
        SceneEntityUuid owner;
        uint64_t captureTicket = 0;
        uint32_t resolution = 0;
        uint32_t mipLevels = 0;
        VulkanImageResource rawRadiance;
        VulkanImageResource depth;
        VulkanImageResource prefilteredRadiance;
        std::array<VkImageView, kReflectionProbeCaptureFaceCount>
            rawFaceViews{};
        std::array<VkImageView, kReflectionProbeCaptureFaceCount>
            depthFaceViews{};
        std::vector<VkImageView> prefilteredMipArrayViews;
        uint64_t logicalBytes = 0;
    };

    // Owns private capture staging and last-known-good published cubemaps. The
    // caller must perform acquire/abandon/promote only at a frame boundary.
    // M7R R4c.3: with a scheduler attached, images and views that frames
    // submitted so far may still reference (a replaced published cube, a
    // removed owner's targets) go to its deletion queue instead of being
    // destroyed at once; with nothing in flight they are destroyed at once.
    class VulkanReflectionProbeCaptureTargets final {
    public:
        VulkanReflectionProbeCaptureTargets() = default;
        VulkanReflectionProbeCaptureTargets(
            const VulkanReflectionProbeCaptureTargets&) = delete;
        VulkanReflectionProbeCaptureTargets& operator=(
            const VulkanReflectionProbeCaptureTargets&) = delete;

        void init(VkDevice device, VkPhysicalDevice physicalDevice,
            VulkanResourceAllocator& allocator,
            VulkanReflectionProbeCaptureTargetConfig config = {});
        void setDeferredDestruction(VulkanFrameScheduler* scheduler) noexcept {
            scheduler_ = scheduler;
        }
        // Device idle: destroys everything at once.
        void cleanup() noexcept;

        [[nodiscard]] const VulkanReflectionProbeCaptureStaging& acquire(
            SceneEntityUuid owner, uint64_t captureTicket,
            uint32_t resolution);
        void abandon(SceneEntityUuid owner, uint64_t captureTicket);
        void promote(SceneEntityUuid owner, uint64_t captureTicket);
        void remove(SceneEntityUuid owner) noexcept;

        [[nodiscard]] const VulkanReflectionProbeCaptureStaging* staging(
            SceneEntityUuid owner, uint64_t captureTicket) const noexcept;
        [[nodiscard]] const VulkanImageResource* published(
            SceneEntityUuid owner) const noexcept;
        [[nodiscard]] uint64_t stagingLogicalBytes() const noexcept {
            return stagingLogicalBytes_;
        }
        [[nodiscard]] uint64_t publishedLogicalBytes() const noexcept {
            return publishedLogicalBytes_;
        }
        [[nodiscard]] uint32_t capturesInFlight() const noexcept;
        [[nodiscard]] uint32_t publishedCount() const noexcept;

    private:
        struct OwnerState {
            SceneEntityUuid owner;
            VulkanReflectionProbeCaptureStaging staging;
            VulkanImageResource published;
            uint64_t publishedLogicalBytes = 0;
            bool hasStaging = false;
            bool hasPublished = false;
        };

        [[nodiscard]] OwnerState* find(SceneEntityUuid owner) noexcept;
        [[nodiscard]] const OwnerState* find(
            SceneEntityUuid owner) const noexcept;
        [[nodiscard]] VkImageView createView(VkImage image,
            VkFormat format, VkImageAspectFlags aspect,
            VkImageViewType type, uint32_t baseMip, uint32_t mipCount,
            uint32_t baseLayer, uint32_t layerCount) const;
        // Clears a staging record; its mip-view vector keeps its storage
        // for the owner's next capture (M7R R5c.8).
        static void resetStaging(VulkanReflectionProbeCaptureStaging& staging) noexcept;
        void destroyStaging(VulkanReflectionProbeCaptureStaging& staging)
            noexcept;
        [[nodiscard]] bool deferDestruction() const noexcept;
        void releaseImage(VulkanImageResource& image) noexcept;
        void releaseView(VkImageView& view) noexcept;
        void destroyOwner(OwnerState& owner) noexcept;
        static void validateConfig(
            const VulkanReflectionProbeCaptureTargetConfig& config);

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanFrameScheduler* scheduler_ = nullptr;
        VulkanReflectionProbeCaptureTargetConfig config_{};
        uint32_t maximumCubeDimension_ = 0;
        uint64_t stagingLogicalBytes_ = 0;
        uint64_t publishedLogicalBytes_ = 0;
        std::vector<OwnerState> owners_;
    };

} // namespace Iridium
