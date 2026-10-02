#pragma once

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/VirtualShadowMap.h"

#include <cstdint>

namespace Iridium {

    class CpuProfiler;

    struct RenderBackendConfig {
        bool enableValidation = false;
        // Adds Khronos synchronization validation when enableValidation is set.
        bool enableSynchronizationValidation = false;
        // M7.6 build-only qualification: no history consumption or occlusion rejection.
        bool experimentalDepthPyramid = false;
        // M7.8 live depth-demand qualification; virtual raster/sampling stay off.
        bool experimentalVirtualShadowResources = false;
        VirtualShadowResourceConfig virtualShadowResources{};
        // Qualification-only previous-frame query dispatch; never rejects draws.
        bool experimentalDepthOcclusionQuery = false;
        // Explicit M7.6 experiment: consume qualified GPU results in compaction.
        bool experimentalDepthOcclusionRejection = false;
        CpuProfiler* cpuProfiler = nullptr;
        bool enableGpuProfiling = false;
        bool enableTransparentPipelineStatistics = false;
        // Legacy qualification umbrella: direct GBuffer draws with no main-view
        // frustum rejection, plus conventional shadows and probe capture.
        bool forceDirectGBufferReference = false;
        // Qualification route: conventional directional, spot, and point
        // shadow submission while the main view and probe capture remain on
        // their independently selected routes.
        bool forceDirectShadowReference = false;
        // Independent shadow-map density policy. Zero pins shadow maps to LOD0.
        float experimentalShadowLodErrorTexels = 0.0f;
        uint32_t shadowLodMaximumLevel = 15u;
        // Zero disables experimental main-GBuffer LOD selection and history.
        float experimentalGpuLodErrorPixels = 0.0f;
        uint32_t gpuLodMaximumLevel = 15u;
        float gpuLodHysteresisFraction = 0.15f;
        // Independent, face-invariant radial LOD policy for reflection captures.
        // Zero pins probe captures to LOD0.
        float experimentalProbeLodErrorPixels = 0.0f;
        uint32_t probeLodMaximumLevel = 15u;
        uint64_t weightedOitOrderSeed = 0;
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference;
        uint32_t clusterTileSize = 32;
        uint32_t clusterDepthSlices = 24;
        uint32_t directionalShadowResolution = 4096;
        uint32_t spotShadowAtlasResolution = 8192;
        uint32_t pointShadowPool256Capacity = kPointShadowPool256Capacity;
        uint32_t pointShadowPool512Capacity = kPointShadowPool512Capacity;
        uint32_t pointShadowPool1024Capacity = kPointShadowPool1024Capacity;
        ProjectReflectionProbeSettings reflectionProbeSettings{};
        double manualExposureEv = 0.0;
        OutputTransformOperator outputOperator = OutputTransformOperator::Aces2;
		Color::OutputTransport outputTransport = Color::OutputTransport::SdrSrgb;
        double paperWhiteNits = 203.0;
        double peakNits = 1000.0;
    };

} // namespace Iridium
