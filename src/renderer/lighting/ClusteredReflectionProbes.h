#pragma once

#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/ReflectionProbeTypes.h"

#include <cstdint>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kMaximumReflectionProbesPerCluster = 4;
    inline constexpr uint32_t kMaximumClusterProbeReferences = 1'048'576;

    struct ClusteredReflectionProbeConfig {
        uint32_t tileWidth = kClusterTileSize;
        uint32_t tileHeight = kClusterTileSize;
        uint32_t depthSlices = kClusterDepthSlices;
        uint32_t maximumProbesPerCluster =
            kMaximumReflectionProbesPerCluster;
        uint32_t maximumProbeReferences = kMaximumClusterProbeReferences;
    };

    enum class ClusterProbeOverflowCode : uint32_t {
        None = 0,
        GlobalReferenceCapacity = 1,
    };

    struct ClusteredReflectionProbeStats {
        uint32_t activeProbeCount = 0;
        uint32_t clustersUsed = 0;
        uint32_t maximumRequestedOccupancy = 0;
        uint64_t requestedProbeReferences = 0;
        uint32_t publishedProbeReferences = 0;
        uint64_t truncatedProbeReferences = 0;
        ClusterProbeOverflowCode overflow = ClusterProbeOverflowCode::None;
    };

    struct ClusteredReflectionProbeProduct {
        ClusterGridDimensions dimensions;
        std::vector<ClusterLightHeader> headers;
        std::vector<uint32_t> probeSlots;
        ClusteredReflectionProbeStats stats;

        friend bool operator==(const ClusteredReflectionProbeProduct&,
            const ClusteredReflectionProbeProduct&) = default;
    };

    class ClusteredReflectionProbeAssigner final {
    public:
        explicit ClusteredReflectionProbeAssigner(
            ClusteredReflectionProbeConfig config = {});

        [[nodiscard]] ClusteredReflectionProbeProduct build(
            const ReflectionProbeGpuFramePacket& probes,
            const ClusterFrameParameters& frame) const;

        [[nodiscard]] const ClusteredReflectionProbeConfig& config()
            const noexcept { return config_; }

    private:
        ClusteredReflectionProbeConfig config_;
    };

} // namespace Iridium
