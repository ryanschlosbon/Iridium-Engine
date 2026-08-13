#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include "assets/AssetGuid.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/FrameCapture.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/RenderBackendConfig.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/color/OutputTransformConfig.h"

namespace Iridium {

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
        bool forceWireframe = false;
        bool validateTextureResidencyChurn = false;
        bool validateReflectionProbes = false;
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
        std::filesystem::path cookedModelArtifact;
        std::filesystem::path cookedEnvironmentArtifact;
        std::optional<AssetGuid> editorAssetViewerGuid;
        std::optional<uint64_t> captureFrameIndex;
        FrameCapturePoint capturePoint = FrameCapturePoint::SceneLinear;
        std::filesystem::path captureDirectory;
        std::string cacheState = "unspecified";
    };

    [[nodiscard]] ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments);
    [[nodiscard]] std::string applicationUsage();

} // namespace Iridium
