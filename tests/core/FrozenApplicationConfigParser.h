#pragma once

// FROZEN REFERENCE FOR M7R R2 CLI PARITY (test-only; delete at M7R R2.10).
// The 6b000ad hand-written parser, used as the oracle for the registry parser.
//
// M7R R2.9 split the 36 qualification fields out of Iridium::ApplicationConfig
// into Iridium::QualificationOptions. The frozen parser keeps writing the
// pre-split shape, so this header freezes that struct too (field-for-field the
// R2.8 ApplicationConfig); FrozenApplicationConfigParser.cpp is unchanged.

#include "core/types/AssetGuid.h"
#include "core/types/FrameCapture.h"
#include "material/TransparencyPolicy.h"
#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/ShadowSettings.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Iridium::FrozenR2Reference {

    struct ApplicationConfig {
#if defined(NDEBUG)
        bool enableValidation = false;
#else
        bool enableValidation = true;
#endif
        bool enableCpuProfiling = false;
        bool enableGpuProfiling = false;
        bool enableTransparentPipelineStatistics = false;
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference;
        bool showHelp = false;
        bool windowVisible = true;
        bool windowDecorated = true;
        bool showProfiler = false;
        bool showMaterialDiagnostics = false;
        bool selectBenchmarkEntity = false;
        bool disableBenchmarkLocalShadows = false;
        bool forceDirectGBufferReference = false;
        bool forceDirectShadowReference = false;
        bool forceDirectProbeCaptureReference = false;
        bool shadowIndirectQualificationOracle = false;
        float experimentalShadowLodErrorTexels = 0.0f;
        uint32_t shadowLodMaximumLevel = 15u;
        float experimentalGpuLodErrorPixels = 0.0f;
        uint32_t gpuLodMaximumLevel = 15u;
        float gpuLodHysteresisFraction = 0.15f;
        bool gpuLodQualificationOracle = false;
        float experimentalProbeLodErrorPixels = 0.0f;
        uint32_t probeLodMaximumLevel = 15u;
        bool probeLodQualificationOracle = false;
        bool experimentalDepthPyramid = false;
        bool experimentalVirtualShadowResources = false;
        bool virtualShadowDepthQualificationOracle = false;
        bool experimentalDepthOcclusionQuery = false;
        bool experimentalDepthOcclusionRejection = false;
        bool depthOcclusionQualificationOracle = false;
        bool validateDepthPyramidCapture = false;
        bool validateDepthPyramidResize = false;
        uint32_t gpuLodMinimumResidentLevel = 0;
        bool forceWireframe = false;
        bool validateTextureResidencyChurn = false;
        bool validateReflectionProbes = false;
        bool validateOrdinary2Capture = false;
        bool validateOrdinary2Fallback = false;
        bool validateOrdinary2Resize = false;
        bool validateWeightedOitResize = false;
        bool validateDeepLayeredCapture = false;
        bool validateDeepLayeredLifecycle = false;
        bool validateOutputTransportSwitch = false;
        TransparencyQuality deepLayeredCaptureQuality =
            TransparencyQuality::Hero4;
        uint32_t validateTextureTableScale = 0;
        uint32_t validateMaterialTableScale = 0;
        uint32_t validateLightTableScale = 0;
        uint32_t clusterStressLightCount = 0;
        uint32_t clusterTileSize = 32;
        uint32_t clusterDepthSlices = 24;
        ProjectShadowSettings shadowSettings{};
        ProjectReflectionProbeSettings reflectionProbeSettings{};
        double manualExposureEv = 0.0;
        OutputTransformOperator outputOperator = OutputTransformOperator::Aces2;
        Color::OutputTransport outputTransport = Color::OutputTransport::SdrSrgb;
        double paperWhiteNits = 203.0;
        double peakNits = 1000.0;
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 720;
        uint64_t warmupFrameCount = 0;
        uint64_t frameLimit = 0;
        bool warmupFrameCountSpecified = false;
        bool frameLimitSpecified = false;
        std::filesystem::path cpuProfileOutput;
        RenderDebugView debugView = RenderDebugView::Final;
        std::string benchmarkId;
        std::filesystem::path benchmarkManifest;
        uint64_t weightedOitOrderSeed = 0;
        std::filesystem::path cookedModelArtifact;
        std::filesystem::path cookedEnvironmentArtifact;
        std::optional<AssetGuid> editorAssetViewerGuid;
        std::optional<uint64_t> captureFrameIndex;
        bool requireCaptureSignal = false;
        FrameCapturePoint capturePoint = FrameCapturePoint::SceneLinear;
        std::filesystem::path captureDirectory;
        std::string cacheState = "unspecified";
    };

    [[nodiscard]] ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments);
    [[nodiscard]] std::string applicationUsage();

} // namespace Iridium::FrozenR2Reference
