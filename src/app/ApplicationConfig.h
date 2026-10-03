#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include "core/types/AssetGuid.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/RenderBackendConfig.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/color/OutputTransformConfig.h"
#include "material/TransparencyPolicy.h"

namespace Iridium {

    struct ApplicationConfig {
#if defined(NDEBUG)
        bool enableValidation = false;
#else
        bool enableValidation = true;
#endif
        // Khronos synchronization validation (implies enableValidation; M7R R3).
        bool enableSynchronizationValidation = false;
        bool enableCpuProfiling = false;
        bool enableGpuProfiling = false;
        bool enableTransparentPipelineStatistics = false;
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference;
        bool showHelp = false;
        bool windowVisible = true;
        bool windowDecorated = true;
        bool showProfiler = false;
        bool showMaterialDiagnostics = false;
        float experimentalShadowLodErrorTexels = 0.0f;
        uint32_t shadowLodMaximumLevel = 15u;
        float experimentalGpuLodErrorPixels = 0.0f;
        uint32_t gpuLodMaximumLevel = 15u;
        float gpuLodHysteresisFraction = 0.15f;
        float experimentalProbeLodErrorPixels = 0.0f;
        uint32_t probeLodMaximumLevel = 15u;
        bool experimentalDepthPyramid = false;
        bool experimentalVirtualShadowResources = false;
        bool experimentalDepthOcclusionQuery = false;
        bool experimentalDepthOcclusionRejection = false;
        // M7R R4b: --render-graph-aliasing on|off (on since R4b.6; the switch
        // is kept until R6).
        bool renderGraphAliasing = true;
        // M7R R4d: --upload-queue auto|graphics|legacy-blocking.
        UploadQueueMode uploadQueue = UploadQueueMode::Auto;
        bool forceWireframe = false;
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
        RenderDebugView debugView = RenderDebugView::Final;
        // --pipeline-cache (M7R R4c.4): the persisted pipeline cache's
        // directory; empty is the user cache directory
        // (%LOCALAPPDATA%/Iridium/PipelineCache), and `off` disables it.
        std::filesystem::path pipelineCacheDirectory;
        bool pipelineCacheEnabled = true;
        std::filesystem::path cookedModelArtifact;
        std::filesystem::path cookedEnvironmentArtifact;
        std::optional<AssetGuid> editorAssetViewerGuid;
    };

    namespace Cli { class CliOptionRegistry; }

    // Parses the runtime, editor and renderer options (AppCli::
    // registerApplicationOptions). Qualification flags are registered by the
    // qualification library, which main.cpp adds to its registry only in
    // IRIDIUM_QUALIFICATION=ON builds; here they are unknown options.
    [[nodiscard]] ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments);
    // "Usage: IridiumEngine [options]\n" followed by the registry's usage.
    [[nodiscard]] std::string applicationUsage(
        const Cli::CliOptionRegistry& registry);
    // Usage of the runtime, editor and renderer options.
    [[nodiscard]] std::string applicationUsage();

} // namespace Iridium
