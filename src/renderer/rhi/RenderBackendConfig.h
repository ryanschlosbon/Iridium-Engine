#pragma once

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ReflectionProbeSettings.h"

#include <cstdint>

namespace Iridium {

    class CpuProfiler;

    struct RenderBackendConfig {
        bool enableValidation = false;
        CpuProfiler* cpuProfiler = nullptr;
        bool enableGpuProfiling = false;
        bool enableTransparentPipelineStatistics = false;
        bool validateReflectionProbeCaptureTargets = false;
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
