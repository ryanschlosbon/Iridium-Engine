#pragma once

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/VirtualShadowMap.h"

#include <cstdint>
#include <filesystem>

namespace Iridium {

    class CpuProfiler;

    // M7R R4d (--upload-queue): where resource uploads run.
    //   Auto: fresh resources on the device's upload queue (a dedicated
    //     transfer or graphics-free compute family, with queue-family
    //     ownership transfers), the rest on graphics; frames wait on the
    //     upload timeline instead of the CPU.
    //   Graphics: the same asynchronous path on the graphics queue only.
    //   LegacyBlocking: the pre-R4d path (graphics submit and a fence wait in
    //     every flush, one staging allocation per upload).
    enum class UploadQueueMode : uint8_t {
        Auto,
        Graphics,
        LegacyBlocking,
    };

    // M9.2 anti-aliasing of the main view. None keeps the M7R single-frame
    // image (the frozen-set route); Taa resolves native temporal AA (1:1).
    enum class AntiAliasingMode : uint8_t {
        None,
        Taa,
    };

    // M9.2 native TAA parameters (taa_resolve.comp). Defaults are the best
    // measured against the 64-sample references, held and in motion (M9 plan,
    // TAA tuning s2-s8 and the motion sweeps).
    struct TemporalAntiAliasingTuning {
        float minimumHistoryWeight = 0.70f;
        float maximumHistoryWeight = 0.97f;
        float motionPixelsForMinimum = 2.0f;
        float varianceGamma = 1.0f;
        float reconstructionSharpness = 6.0f;
        // Clip half-width for still pixels (blended toward varianceGamma
        // as motion grows to one pixel).
        float staticVarianceGamma = 3.0f;
        // History weight of still pixels (0: the moving-content rule).
        float stillHistoryWeight = 0.97f;

        friend bool operator==(const TemporalAntiAliasingTuning&,
            const TemporalAntiAliasingTuning&) = default;
    };

    // M9.5 exposure of the main view. Manual keeps today's output (the
    // manual EV; the frozen-set and fixture route); Auto meters the resolved
    // scene colour on the GPU and adapts, with the manual EV as compensation.
    enum class ExposureMode : uint8_t {
        Manual,
        Auto,
    };

    // M9.5 auto-exposure parameters (exposure_histogram.comp,
    // exposure_adapt.comp). EV100 is scene luminance in photometric units
    // (scene-linear / PhotometricToSceneScale) at ISO 100, K = 12.5.
    struct AutoExposureSettings {
        // The 128 log2-luminance histogram bins span this range.
        float histogramMinEv100 = -10.0f;
        float histogramMaxEv100 = 20.0f;
        // The metered luminance is the mean of the bins between these
        // fractions of the (weighted) pixel count.
        float lowPercentile = 0.10f;
        float highPercentile = 0.90f;
        // The adapted EV100 never leaves [minimumEv100, maximumEv100].
        float minimumEv100 = -10.0f;
        float maximumEv100 = 20.0f;
        // Toward a brighter scene (up) and a darker one (down), EV per second
        // of the view's own time.
        float speedUpEvPerSecond = 3.0f;
        float speedDownEvPerSecond = 1.0f;
        // 0 meters every pixel equally; 1 weights the centre up to 16x.
        float centreWeight = 0.0f;

        friend bool operator==(const AutoExposureSettings&,
            const AutoExposureSettings&) = default;
    };

    // M9.4 bloom (bloom.comp, output.frag). A dual-filter chain over the
    // resolved scene colour from half resolution down, composited before
    // exposure and the output transform. With no threshold the composite is
    // energy-conserving: colour = lerp(scene, bloom, intensity), so bloom
    // only redistributes light. A threshold selects the energy above it
    // (soft knee) and adds it: colour = scene + intensity * bloom.
    struct BloomSettings {
        // Off until admission (M9.7); then on by owner decision.
        bool enabled = false;
        // The share of light scattered into the bloom (0..1).
        float intensity = 0.04f;
        // Scene-linear (pre-exposure) threshold; 0 disables it.
        float threshold = 0.0f;
        // Soft-knee half-width around the threshold (scene-linear).
        float knee = 0.0f;
        // Chain levels from half resolution (1..8; clamped to the extent).
        uint32_t levels = 6u;
        // Karis (1 / (1 + luma)) box weights on the first downsample. They
        // keep aliased sub-pixel highlights from flickering in the chain but
        // remove part of their energy. Measured share of the scene's energy
        // the chain keeps (M9.4): TF-hdr 63% with them, 99.9% without;
        // TF-static and TF-emissive 99.0% with, 99.7-99.8% without.
        bool karisAverage = true;

        friend bool operator==(const BloomSettings&, const BloomSettings&) = default;
    };

    struct RenderBackendConfig {
        bool enableValidation = false;
        // Adds Khronos synchronization validation when enableValidation is set.
        bool enableSynchronizationValidation = false;
        // Directory of the persisted pipeline cache (<vendor>-<device>.ircache,
        // M7R R4c.4). Empty: no pipeline cache.
        std::filesystem::path pipelineCacheDirectory;
        // M7.6 build-only qualification: no history consumption or occlusion rejection.
        bool experimentalDepthPyramid = false;
        // M7.8 live depth-demand qualification; virtual raster/sampling stay off.
        bool experimentalVirtualShadowResources = false;
        VirtualShadowResourceConfig virtualShadowResources{};
        // Qualification-only previous-frame query dispatch; never rejects draws.
        bool experimentalDepthOcclusionQuery = false;
        // Explicit M7.6 experiment: consume qualified GPU results in compaction.
        bool experimentalDepthOcclusionRejection = false;
        // M7R R4b: transient render-graph images whose lifetimes never overlap
        // share memory (--render-graph-aliasing on|off; on since R4b.6, the
        // switch is kept until R6).
        bool renderGraphAliasing = true;
        UploadQueueMode uploadQueue = UploadQueueMode::Auto;
        AntiAliasingMode antiAliasing = AntiAliasingMode::None;
        TemporalAntiAliasingTuning taaTuning{};
        // M9.5: Manual until admission (M9.7); Auto adds the exposure passes.
        ExposureMode exposureMode = ExposureMode::Manual;
        AutoExposureSettings autoExposure{};
        // M9.4: off until admission; enabled adds post.bloom.
        BloomSettings bloom{};
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
