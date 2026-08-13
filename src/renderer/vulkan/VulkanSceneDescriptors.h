#pragma once

#include "DescriptorAllocator.h"
#include "VulkanFrameTargets.h"
#include "renderer/rhi/ReflectionProbeTypes.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    struct VulkanClusterSceneBufferDescriptors {
        VkDescriptorBufferInfo global;
        VkDescriptorBufferInfo headers;
        VkDescriptorBufferInfo indices;
        VkDescriptorBufferInfo fallback;
        VkDescriptorBufferInfo diagnostics;
        VkDescriptorBufferInfo parameters;
    };

    struct VulkanReflectionProbeBufferDescriptors {
        VkDescriptorBufferInfo records;
        VkDescriptorBufferInfo headers;
        VkDescriptorBufferInfo indices;
    };

    struct VulkanEnvironmentImageDescriptors {
        VkDescriptorImageInfo irradiance;
        VkDescriptorImageInfo prefilteredRadiance;
        VkDescriptorImageInfo brdfLut;
        VkDescriptorImageInfo skyRadiance;
    };

    struct VulkanDirectionalShadowDescriptors {
        VkDescriptorImageInfo image;
        std::vector<VkDescriptorBufferInfo> frameData;
    };

    struct VulkanSpotShadowDescriptors {
        VkDescriptorImageInfo image;
        std::vector<VkDescriptorBufferInfo> frameData;
    };

    struct VulkanPointShadowDescriptors {
        std::array<VkDescriptorImageInfo, 3> images{};
        std::vector<VkDescriptorBufferInfo> frameData;
    };

    // Owns only lighting descriptor-set handles. DescriptorAllocator owns the
    // underlying pools; target images and the environment texture remain external.
    class VulkanSceneDescriptors final {
    public:
        VulkanSceneDescriptors() = default;
        VulkanSceneDescriptors(const VulkanSceneDescriptors&) = delete;
        VulkanSceneDescriptors& operator=(const VulkanSceneDescriptors&) = delete;

        void init(VkDevice device, ::DescriptorAllocator& allocator, VkDescriptorSetLayout layout);
        // One set is built per frame context. The caller guarantees GPU idle. Old
        // sets are freed before replacements;
        // the most recent environment descriptor is reapplied automatically.
        void rebuild(const VulkanFrameTargets& frameTargets);
        void setEnvironmentImages(
            const VulkanEnvironmentImageDescriptors& environment);
        void setDirectionalShadow(
            const VulkanDirectionalShadowDescriptors& shadow);
        void setSpotShadow(const VulkanSpotShadowDescriptors& shadow);
        void setPointShadow(const VulkanPointShadowDescriptors& shadow);
        void setLightBuffers(std::span<const VkDescriptorBufferInfo> buffers);
        void setClusterBuffers(
            std::span<const VulkanClusterSceneBufferDescriptors> buffers);
        void setReflectionProbeBuffers(std::span<const
            VulkanReflectionProbeBufferDescriptors> buffers);
        void setReflectionProbeImages(
            std::span<const VkDescriptorImageInfo> images);
        void cleanup();

        [[nodiscard]] VkDescriptorSet get(uint32_t frameIndex) const;
        [[nodiscard]] size_t size() const noexcept;

    private:
        // Binding contract: 0 depth, 1 normal, 2 albedo, 3 reserved legacy,
        // 4 opaque scene copy, 5 glass depth, 6 emissive, 7 F0/roughness,
        // 8 material/flags, 9 packed light records, 10 global slots, 11 cluster
        // headers, 12 local indices, 13 fallback slots, 14 diagnostics, and
        // 15 cluster parameters, 16 irradiance cube, 17 prefiltered-radiance
        // cube, 18 F0/F90 BRDF LUT, 19 sky-radiance cube, 20 directional
        // shadow array, 21 directional-shadow frame data, 22 spot-shadow
        // atlas, 23 spot-shadow frame data, 24/25/26 point-shadow cube
        // arrays at 256/512/1024, 27 point-shadow frame data, 28 local-probe
        // records, 29 local-probe cluster headers, 30 local-probe indices, and
        // 31 the fixed indexed local-prefiltered-cubemap table. Deferred and
        // forward layouts share this set.
        VkDevice device_ = VK_NULL_HANDLE;
        ::DescriptorAllocator* allocator_ = nullptr;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets_;
        VulkanEnvironmentImageDescriptors environmentImages_{};
        VulkanDirectionalShadowDescriptors directionalShadow_{};
        VulkanSpotShadowDescriptors spotShadow_{};
        VulkanPointShadowDescriptors pointShadow_{};
        std::vector<VkDescriptorBufferInfo> lightBuffers_;
        std::vector<VulkanClusterSceneBufferDescriptors> clusterBuffers_;
        std::vector<VulkanReflectionProbeBufferDescriptors>
            reflectionProbeBuffers_;
        std::array<VkDescriptorImageInfo,
            kMaximumGpuReflectionProbeEnvironments> reflectionProbeImages_{};
        bool hasEnvironmentImages_ = false;
        bool hasDirectionalShadow_ = false;
        bool hasSpotShadow_ = false;
        bool hasPointShadow_ = false;
        bool hasReflectionProbeImages_ = false;
    };

} // namespace Iridium
