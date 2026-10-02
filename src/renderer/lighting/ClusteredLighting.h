#pragma once

#include "renderer/rhi/LightingTypes.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kClusterTileSize = 32;
    inline constexpr uint32_t kClusterDepthSlices = 24;
    inline constexpr uint32_t kMaximumLightsPerCluster = 256;
    inline constexpr uint32_t kMaximumClusterLightReferences = 4'194'304;
    inline constexpr uint32_t kMaximumClusterFallbackLights = 64;
    inline constexpr uint32_t kClusterScanWorkgroupSize = 256;

    inline constexpr const char* kClusterGlobalResourceName =
        "lighting.cluster.global";
    inline constexpr const char* kClusterHeaderResourceName =
        "lighting.cluster.headers";
    inline constexpr const char* kClusterIndexResourceName =
        "lighting.cluster.indices";
    inline constexpr const char* kClusterFallbackResourceName =
        "lighting.cluster.fallback";
    inline constexpr const char* kClusterDiagnosticResourceName =
        "lighting.cluster.diagnostics";
    inline constexpr const char* kClusterCountResourceName =
        "lighting.cluster.counts";
    inline constexpr const char* kClusterCursorResourceName =
        "lighting.cluster.cursors";
    inline constexpr const char* kClusterScanScratchResourceName =
        "lighting.cluster.scan-scratch";
    inline constexpr const char* kClusterIndirectResourceName =
        "lighting.cluster.indirect";

    [[nodiscard]] constexpr uint64_t clusterScanScratchElementCount(
        uint64_t clusterCount) noexcept {
        uint64_t result = 0;
        while (clusterCount > 1) {
            clusterCount = (clusterCount + kClusterScanWorkgroupSize - 1u) /
                kClusterScanWorkgroupSize;
            result += clusterCount;
        }
        return (std::max)(result, uint64_t{ 1 });
    }

    struct ClusterGridConfig {
        uint32_t tileWidth = kClusterTileSize;
        uint32_t tileHeight = kClusterTileSize;
        uint32_t depthSlices = kClusterDepthSlices;
        uint32_t maximumLightsPerCluster = kMaximumLightsPerCluster;
        uint32_t maximumLightReferences = kMaximumClusterLightReferences;
        uint32_t maximumDirectionalLights = kMaximumGlobalDirectionalLights;
        uint32_t maximumFallbackLights = kMaximumClusterFallbackLights;
    };

    struct ClusterFrameParameters {
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        float nearPlane = 0.1f;
        float farPlane = 1'000.0f;
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };
    };

    struct alignas(16) PackedGpuClusterParameters {
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };
        glm::uvec4 grid{};   // width, height, tiles-x, tiles-y
        glm::vec4 depth{};   // near, far, slices/log(far/near), reserved
        glm::uvec4 limits{}; // slices, per-cluster, references, directionals
        glm::uvec4 input{};  // active lights, fallback count, tile width, tile height
        glm::vec4 environment{}; // lighting scale, background scale, yaw radians, flags
    };

    struct ClusterGridDimensions {
        uint32_t tilesX = 0;
        uint32_t tilesY = 0;
        uint32_t depthSlices = 0;

        [[nodiscard]] uint64_t clusterCount() const noexcept {
            return static_cast<uint64_t>(tilesX) * tilesY * depthSlices;
        }

        friend bool operator==(ClusterGridDimensions,
            ClusterGridDimensions) = default;
    };

    struct ClusterLightHeader {
        uint32_t offset = 0;
        uint32_t count = 0;

        friend bool operator==(ClusterLightHeader,
            ClusterLightHeader) = default;
    };

    enum class ClusterOverflowCode : uint32_t {
        None = 0,
        DirectionalCapacity = 1,
        PerClusterCapacity = 2,
        GlobalReferenceCapacity = 3,
    };

    struct ClusterAssignmentStats {
        uint32_t activeLightCount = 0;
        uint32_t directionalLightCount = 0;
        uint32_t localLightCount = 0;
        uint32_t clustersUsed = 0;
        uint32_t maximumClusterOccupancy = 0;
        uint64_t requestedLightReferences = 0;
        uint32_t publishedLightReferences = 0;
        uint32_t fallbackLightCount = 0;
        uint32_t droppedLightCount = 0;
        ClusterOverflowCode overflow = ClusterOverflowCode::None;

        friend bool operator==(const ClusterAssignmentStats&,
            const ClusterAssignmentStats&) = default;
    };

    struct ClusteredLightingProduct {
        ClusterGridDimensions dimensions;
        std::vector<uint32_t> globalDirectionalSlots;
        std::vector<ClusterLightHeader> headers;
        std::vector<uint32_t> localLightSlots;
        std::vector<uint32_t> fallbackLightSlots;
        ClusterAssignmentStats stats;

        [[nodiscard]] bool usesFallback() const noexcept {
            return stats.overflow != ClusterOverflowCode::None;
        }

        friend bool operator==(const ClusteredLightingProduct&,
            const ClusteredLightingProduct&) = default;
    };

    [[nodiscard]] ClusterGridDimensions clusterGridDimensions(
        const ClusterGridConfig& config,
        const ClusterFrameParameters& frame);
    [[nodiscard]] uint32_t clusterDepthSlice(float positiveViewDepth,
        float nearPlane, float farPlane, uint32_t sliceCount);

    // Shared deterministic whole-product fallback ordering used by the CPU
    // reference and Vulkan upload path. Output storage is reusable so the
    // frame path remains allocation-free after capacity preparation.
    void selectClusterFallbackLights(const LightingFramePacket& lights,
        const glm::mat4& view, uint32_t maximumLights,
        std::vector<uint32_t>& output);

    class ClusteredLightAssigner final {
    public:
        explicit ClusteredLightAssigner(ClusterGridConfig config = {});

        [[nodiscard]] ClusteredLightingProduct build(
            const LightingFramePacket& lights,
            const ClusterFrameParameters& frame) const;
        [[nodiscard]] const ClusterGridConfig& config() const noexcept {
            return config_;
        }

    private:
        ClusterGridConfig config_;
    };

    static_assert(sizeof(ClusterLightHeader) == 8);
    static_assert(alignof(ClusterLightHeader) == 4);
    static_assert(sizeof(PackedGpuClusterParameters) == 208);
    static_assert(alignof(PackedGpuClusterParameters) == 16);

} // namespace Iridium
