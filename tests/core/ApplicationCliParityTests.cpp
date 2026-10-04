// M7R R2.4 contract for the registry-driven IridiumEngine command line.
//
// 1. A per-flag table, written from the original 6b000ad else-if parser (the
//    spec): every flag is accepted with a valid value and changes exactly the
//    expected fields (including implied flags); missing and invalid values fail
//    with the exact original messages; each flag has the expected owner.
// 2. Aliases and removed flags.
// 3. Usage text groups and layout.
// 4. Without the qualification registrations the qualification flags are
//    unknown options.
//
// During R2 a frozen copy of the 6b000ad parser and a 15,821-vector parity
// corpus proved the refactor exact; they were retired at M7R R2.10 (plan log).
// Since R2.9 the parse result is ApplicationConfig plus the qualification
// library's QualificationOptions (CombinedConfig).

#include "app/ApplicationConfig.h"
#include "app/cli/ApplicationCliOptions.h"
#include "qualification/QualificationOptions.h"
#include "core/cli/CliOptionRegistry.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using Args = std::vector<std::string_view>;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    #define CHECK_MSG(condition, message) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << "): " \
                          << message << '\n'; \
                return false; \
            } \
        } while (false)

    template <typename T>
    std::string exact(T value) {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    }

    template <typename E>
    int enumValue(E value) {
        return static_cast<int>(value);
    }

    // The registry parser writes two structs since M7R R2.9 (no field overlap).
    struct CombinedConfig : ApplicationConfig, QualificationOptions {};

    // Every field, exactly (floats round-trip via to_chars). Instantiated for
    // CombinedConfig.
    template <typename Config>
    std::string describe(const Config& c) {
        std::ostringstream s;
        const auto field = [&s](const char* name, const std::string& value) {
            s << name << '=' << value << ';';
        };
        const auto flag = [&](const char* name, bool value) { field(name, value ? "1" : "0"); };
        flag("enableValidation", c.enableValidation);
        flag("enableSynchronizationValidation", c.enableSynchronizationValidation);
        flag("enableCpuProfiling", c.enableCpuProfiling);
        flag("enableGpuProfiling", c.enableGpuProfiling);
        flag("enableTransparentPipelineStatistics", c.enableTransparentPipelineStatistics);
        field("gBufferLayout", exact(enumValue(c.gBufferLayout)));
        flag("showHelp", c.showHelp);
        flag("windowVisible", c.windowVisible);
        flag("windowDecorated", c.windowDecorated);
        flag("showProfiler", c.showProfiler);
        flag("showMaterialDiagnostics", c.showMaterialDiagnostics);
        flag("selectBenchmarkEntity", c.selectBenchmarkEntity);
        flag("disableBenchmarkLocalShadows", c.disableBenchmarkLocalShadows);
        flag("forceDirectGBufferReference", c.forceDirectGBufferReference);
        flag("forceDirectShadowReference", c.forceDirectShadowReference);
        flag("forceDirectProbeCaptureReference", c.forceDirectProbeCaptureReference);
        flag("shadowIndirectQualificationOracle", c.shadowIndirectQualificationOracle);
        field("experimentalShadowLodErrorTexels", exact(c.experimentalShadowLodErrorTexels));
        field("shadowLodMaximumLevel", exact(c.shadowLodMaximumLevel));
        field("experimentalGpuLodErrorPixels", exact(c.experimentalGpuLodErrorPixels));
        field("gpuLodMaximumLevel", exact(c.gpuLodMaximumLevel));
        field("gpuLodHysteresisFraction", exact(c.gpuLodHysteresisFraction));
        flag("gpuLodQualificationOracle", c.gpuLodQualificationOracle);
        field("experimentalProbeLodErrorPixels", exact(c.experimentalProbeLodErrorPixels));
        field("probeLodMaximumLevel", exact(c.probeLodMaximumLevel));
        flag("probeLodQualificationOracle", c.probeLodQualificationOracle);
        flag("experimentalDepthPyramid", c.experimentalDepthPyramid);
        flag("experimentalVirtualShadowResources", c.experimentalVirtualShadowResources);
        flag("virtualShadowDepthQualificationOracle", c.virtualShadowDepthQualificationOracle);
        flag("experimentalDepthOcclusionQuery", c.experimentalDepthOcclusionQuery);
        flag("experimentalDepthOcclusionRejection", c.experimentalDepthOcclusionRejection);
        flag("depthOcclusionQualificationOracle", c.depthOcclusionQualificationOracle);
        flag("indirectStreamDigest", c.indirectStreamDigest);
        flag("casterRevisionOracle", c.casterRevisionOracle);
        flag("renderGraphAliasing", c.renderGraphAliasing);
        field("uploadQueue", exact(enumValue(c.uploadQueue)));
        flag("aliasPoison", c.aliasPoison);
        flag("validateDepthPyramidCapture", c.validateDepthPyramidCapture);
        flag("validateDepthPyramidResize", c.validateDepthPyramidResize);
        field("gpuLodMinimumResidentLevel", exact(c.gpuLodMinimumResidentLevel));
        flag("forceWireframe", c.forceWireframe);
        flag("validateTextureResidencyChurn", c.validateTextureResidencyChurn);
        flag("validateReflectionProbes", c.validateReflectionProbes);
        flag("validateOrdinary2Capture", c.validateOrdinary2Capture);
        flag("validateOrdinary2Fallback", c.validateOrdinary2Fallback);
        flag("validateOrdinary2Resize", c.validateOrdinary2Resize);
        flag("validateWeightedOitResize", c.validateWeightedOitResize);
        flag("validateDeepLayeredCapture", c.validateDeepLayeredCapture);
        flag("validateDeepLayeredLifecycle", c.validateDeepLayeredLifecycle);
        flag("validateOutputTransportSwitch", c.validateOutputTransportSwitch);
        field("deepLayeredCaptureQuality", exact(enumValue(c.deepLayeredCaptureQuality)));
        field("validateTextureTableScale", exact(c.validateTextureTableScale));
        field("validateMaterialTableScale", exact(c.validateMaterialTableScale));
        field("validateLightTableScale", exact(c.validateLightTableScale));
        field("clusterStressLightCount", exact(c.clusterStressLightCount));
        field("clusterTileSize", exact(c.clusterTileSize));
        field("clusterDepthSlices", exact(c.clusterDepthSlices));
        const ProjectShadowSettings& sh = c.shadowSettings;
        field("shadow.filterMode", exact(enumValue(sh.filterMode)));
        field("shadow.qualityProfile", exact(enumValue(sh.qualityProfile)));
        field("shadow.sourceDiameter", exact(sh.directionalSourceAngularDiameterDegrees));
        field("shadow.maximumPenumbraTexels", exact(sh.maximumPenumbraTexels));
        field("shadow.directionalResolution", exact(sh.directionalResolution));
        field("shadow.maximumDirectionalLights", exact(sh.maximumDirectionalLights));
        field("shadow.maximumCascadeUpdatesPerLight", exact(sh.maximumCascadeUpdatesPerLight));
        field("shadow.directionalMaximumDistanceMeters", exact(sh.directionalMaximumDistanceMeters));
        field("shadow.directionalSplitLambda", exact(sh.directionalSplitLambda));
        field("shadow.directionalGuardBandFraction", exact(sh.directionalGuardBandFraction));
        field("shadow.directionalDepthPaddingMeters", exact(sh.directionalDepthPaddingMeters));
        field("shadow.receiverDepthBias", exact(sh.directionalReceiverDepthBiasTexels));
        field("shadow.receiverPlaneClamp", exact(sh.directionalReceiverPlaneClampTexels));
        field("shadow.normalOffset", exact(sh.directionalNormalOffsetTexels));
        field("shadow.spotAtlasResolution", exact(sh.spotAtlasResolution));
        field("shadow.maximumSpotRenderedTexelsPerFrame", exact(sh.maximumSpotRenderedTexelsPerFrame));
        field("shadow.maximumCompatibleSpotStaleFrames", exact(sh.maximumCompatibleSpotStaleFrames));
        field("shadow.pointPool256Capacity", exact(sh.pointPool256Capacity));
        field("shadow.pointPool512Capacity", exact(sh.pointPool512Capacity));
        field("shadow.pointPool1024Capacity", exact(sh.pointPool1024Capacity));
        field("shadow.maximumPointRenderedTexelsPerFrame", exact(sh.maximumPointRenderedTexelsPerFrame));
        field("shadow.maximumCompatiblePointStaleFrames", exact(sh.maximumCompatiblePointStaleFrames));
        const ProjectReflectionProbeSettings& rp = c.reflectionProbeSettings;
        field("probe.maximumRenderedTexelsPerFrame", exact(rp.maximumRenderedTexelsPerFrame));
        field("probe.maximumFacesPerProbePerFrame", exact(rp.maximumFacesPerProbePerFrame));
        field("probe.maximumCapturesInFlight", exact(rp.maximumCapturesInFlight));
        field("probe.minimumRealtimeFramesBetweenCaptures", exact(rp.minimumRealtimeFramesBetweenCaptures));
        field("probe.prefilterSampleCount", exact(rp.prefilterSampleCount));
        field("manualExposureEv", exact(c.manualExposureEv));
        field("outputOperator", exact(enumValue(c.outputOperator)));
        field("outputTransport", exact(enumValue(c.outputTransport)));
        field("paperWhiteNits", exact(c.paperWhiteNits));
        field("peakNits", exact(c.peakNits));
        field("windowWidth", exact(c.windowWidth));
        field("windowHeight", exact(c.windowHeight));
        field("warmupFrameCount", exact(c.warmupFrameCount));
        field("frameLimit", exact(c.frameLimit));
        flag("warmupFrameCountSpecified", c.warmupFrameCountSpecified);
        flag("frameLimitSpecified", c.frameLimitSpecified);
        field("cpuProfileOutput", c.cpuProfileOutput.generic_string());
        field("debugView", exact(enumValue(c.debugView)));
        field("benchmarkId", c.benchmarkId);
        field("benchmarkManifest", c.benchmarkManifest.generic_string());
        field("weightedOitOrderSeed", exact(c.weightedOitOrderSeed));
        field("pipelineCacheDirectory", c.pipelineCacheDirectory.generic_string());
        flag("pipelineCacheEnabled", c.pipelineCacheEnabled);
        field("cookedModelArtifact", c.cookedModelArtifact.generic_string());
        field("cookedEnvironmentArtifact", c.cookedEnvironmentArtifact.generic_string());
        field("editorAssetViewerGuid", c.editorAssetViewerGuid
            ? c.editorAssetViewerGuid->toString() : std::string("none"));
        field("captureFrameIndex", c.captureFrameIndex
            ? exact(*c.captureFrameIndex) : std::string("none"));
        flag("requireCaptureSignal", c.requireCaptureSignal);
        field("capturePoint", exact(enumValue(c.capturePoint)));
        field("captureDirectory", c.captureDirectory.generic_string());
        field("cacheState", c.cacheState);
        field("scriptedChanges", c.scriptedChanges.generic_string());
        field("backgroundCookSource", c.backgroundCookSource.generic_string());
        flag("frameTaskProbe", c.frameTaskProbe);
        return s.str();
    }

    // The IridiumEngine command line of a qualification build: the runtime,
    // editor and renderer registrations (ApplicationConfig) followed by the
    // qualification library's (QualificationOptions), as main.cpp builds it.
    void registerEngineOptions(Cli::CliOptionRegistry& registry, CombinedConfig& c) {
        AppCli::registerApplicationOptions(registry, c);
        registerQualificationOptions(registry, c, c);
    }

    CombinedConfig parseEngineConfig(std::span<const std::string_view> arguments) {
        CombinedConfig config{};
        Cli::CliOptionRegistry registry;
        registerEngineOptions(registry, config);
        registry.parse(arguments);
        return config;
    }

    std::string engineUsage() {
        CombinedConfig unused{};
        Cli::CliOptionRegistry registry;
        registerEngineOptions(registry, unused);
        return applicationUsage(registry);
    }

    // "OK:<config>" or "ERR:<kind>:<message>".
    template <typename Parser>
    std::string outcome(Parser parser, const Args& args) {
        try {
            return "OK:" + describe(parser(args));
        }
        catch (const std::invalid_argument& error) {
            return std::string("ERR:invalid_argument:") + error.what();
        }
        catch (const std::exception& error) {
            return std::string("ERR:exception:") + error.what();
        }
    }

    std::string newOutcome(const Args& args) { return outcome(&parseEngineConfig, args); }
    std::string error(std::string_view message) {
        return "ERR:invalid_argument:" + std::string(message);
    }

    std::string joined(const Args& args) {
        std::string text;
        for (const std::string_view arg : args) {
            text += '[';
            text += arg;
            text += ']';
        }
        return text;
    }

    constexpr std::string_view kGuid = "019fb73d-5a26-7326-8688-ea55a972179c";
    constexpr std::string_view kNilGuid = "00000000-0000-0000-0000-000000000000";

    struct InvalidValue {
        std::string_view value;
        std::string_view message;
    };

    struct FlagCase {
        std::string_view name;
        std::string_view owner;
        // Empty for a switch.
        std::string_view validValue;
        // Extra arguments the post-parse checks need for an accepted run.
        Args context;
        // Mutates a default config into the expected result of name+value+context.
        std::function<void(CombinedConfig&)> expect;
        std::string_view missingMessage;  // empty for a switch
        std::vector<InvalidValue> invalid;
    };

    constexpr std::string_view R = "runtime";
    constexpr std::string_view E = "editor";
    constexpr std::string_view G = "renderer";
    constexpr std::string_view Q = "qualification";

    // The spec: one row per flag of the 6b000ad parser (82 flags; the removed
    // --developer-legacy-transparency is checked separately).
    std::vector<FlagCase> flagTable() {
        using C = CombinedConfig;
        const Args captureContext{ "--capture-frame", "0", "--capture-directory", "out/cap" };
        const auto withCapture = [](C& c) {
            c.captureFrameIndex = 0;
            c.captureDirectory = "out/cap";
        };
        return {
            // --- runtime (13) ---
            { "--validation", R, {}, {}, [](C& c) { c.enableValidation = true; }, {}, {} },
            { "--no-validation", R, {}, {}, [](C& c) { c.enableValidation = false; }, {}, {} },
            { "--validation-sync", R, {}, {}, [](C& c) {
                c.enableValidation = true; c.enableSynchronizationValidation = true; }, {}, {} },
            { "--profile-cpu", R, {}, {}, [](C& c) { c.enableCpuProfiling = true; }, {}, {} },
            { "--profile-gpu", R, {}, {}, [](C& c) {
                c.enableCpuProfiling = true; c.enableGpuProfiling = true; }, {}, {} },
            { "--profile-transparent-overdraw", R, {}, {}, [](C& c) {
                c.enableCpuProfiling = true; c.enableTransparentPipelineStatistics = true; },
                {}, {} },
            { "--hidden-window", R, {}, {}, [](C& c) {
                c.windowVisible = false; c.windowDecorated = false; }, {}, {} },
            { "--borderless-window", R, {}, {}, [](C& c) { c.windowDecorated = false; }, {}, {} },
            { "--cooked-model-artifact", R, "out/ddc/model.irartifact", {}, [](C& c) {
                c.cookedModelArtifact = "out/ddc/model.irartifact"; },
                "--cooked-model-artifact requires a path",
                { { "", "--cooked-model-artifact requires a path" } } },
            { "--cooked-environment-artifact", R, "out/ddc/env.irartifact", {}, [](C& c) {
                c.cookedEnvironmentArtifact = "out/ddc/env.irartifact"; },
                "--cooked-environment-artifact requires a path",
                { { "", "--cooked-environment-artifact requires a path" } } },
            { "--warmup-frames", R, "5", {}, [](C& c) {
                c.warmupFrameCount = 5; c.warmupFrameCountSpecified = true; },
                "--warmup-frames requires a frame count",
                { { "-5", "--warmup-frames requires an unsigned integer" },
                  { "", "--warmup-frames requires an unsigned integer" } } },
            { "--frame-limit", R, "100", {}, [](C& c) {
                c.frameLimit = 100; c.frameLimitSpecified = true; },
                "--frame-limit requires a frame count",
                { { "x", "--frame-limit requires an unsigned integer" },
                  { "1.5", "--frame-limit requires an unsigned integer" } } },
            { "--window-size", R, "3840x2160", {}, [](C& c) {
                c.windowWidth = 3840; c.windowHeight = 2160; },
                "--window-size requires WIDTHxHEIGHT",
                { { "3840", "--window-size requires WIDTHxHEIGHT" },
                  { "0x2160", "--window-size dimensions are out of range" },
                  { "4000000000x1", "--window-size dimensions are out of range" },
                  { "ax2", "--window-size requires an unsigned integer" } } },
            { "--help", R, {}, {}, [](C& c) { c.showHelp = true; }, {}, {} },
            // --- editor (4) ---
            { "--show-profiler", E, {}, {}, [](C& c) { c.showProfiler = true; }, {}, {} },
            { "--show-material-diagnostics", E, {}, {}, [](C& c) {
                c.showMaterialDiagnostics = true; }, {}, {} },
            { "--wireframe", E, {}, {}, [](C& c) { c.forceWireframe = true; }, {}, {} },
            { "--open-asset-viewer", E, kGuid, {}, [](C& c) {
                c.editorAssetViewerGuid = AssetGuid::parse(kGuid); },
                "--open-asset-viewer requires an asset GUID",
                { { "", "--open-asset-viewer requires an asset GUID" },
                  { "not-a-guid", "--open-asset-viewer requires a non-nil asset GUID" },
                  { kNilGuid, "--open-asset-viewer requires a non-nil asset GUID" } } },
            // --- renderer (32) ---
            { "--cluster-tile-size", G, "16", {}, [](C& c) { c.clusterTileSize = 16; },
                "--cluster-tile-size requires 16 or 32",
                { { "8", "--cluster-tile-size requires 16 or 32" },
                  { "x", "--cluster-tile-size requires an unsigned integer" } } },
            { "--cluster-depth-slices", G, "32", {}, [](C& c) { c.clusterDepthSlices = 32; },
                "--cluster-depth-slices requires 24 or 32",
                { { "16", "--cluster-depth-slices requires 24 or 32" },
                  { "-1", "--cluster-depth-slices requires an unsigned integer" } } },
            { "--shadow-directional-resolution", G, "2048", {}, [](C& c) {
                c.shadowSettings.directionalResolution = 2048; },
                "--shadow-directional-resolution requires 512, 1024, 2048, or 4096",
                { { "1536", "--shadow-directional-resolution requires 512, 1024, 2048, or 4096" },
                  { "4k", "--shadow-directional-resolution requires an unsigned integer" } } },
            { "--shadow-directional-lights", G, "1", {}, [](C& c) {
                c.shadowSettings.maximumDirectionalLights = 1; },
                "--shadow-directional-lights requires 1 or 2",
                { { "0", "--shadow-directional-lights requires 1 or 2" },
                  { "3", "--shadow-directional-lights requires 1 or 2" } } },
            { "--shadow-directional-source-diameter", G, "1.25", {}, [](C& c) {
                c.shadowSettings.directionalSourceAngularDiameterDegrees = 1.25f; },
                "--shadow-directional-source-diameter requires degrees in [0, 5]",
                { { "5.1", "--shadow-directional-source-diameter requires a finite value in "
                           "[0.000000, 5.000000]" } } },
            { "--shadow-directional-distance", G, "325", {}, [](C& c) {
                c.shadowSettings.directionalMaximumDistanceMeters = 325.0f; },
                "--shadow-directional-distance requires metres in [1, 100000]",
                { { "0", "--shadow-directional-distance requires a finite value in "
                         "[1.000000, 100000.000000]" } } },
            { "--shadow-directional-receiver-bias", G, "1.75", {}, [](C& c) {
                c.shadowSettings.directionalReceiverDepthBiasTexels = 1.75f; },
                "--shadow-directional-receiver-bias requires shadow texels in [0, 8]",
                { { "8.1", "--shadow-directional-receiver-bias requires a finite value in "
                           "[0.000000, 8.000000]" } } },
            { "--shadow-directional-receiver-plane-clamp", G, "2.5", {}, [](C& c) {
                c.shadowSettings.directionalReceiverPlaneClampTexels = 2.5f; },
                "--shadow-directional-receiver-plane-clamp requires shadow texels in [0, 8]",
                { { "-0.1", "--shadow-directional-receiver-plane-clamp requires a finite "
                            "value in [0.000000, 8.000000]" } } },
            { "--shadow-directional-normal-offset", G, "0.75", {}, [](C& c) {
                c.shadowSettings.directionalNormalOffsetTexels = 0.75f; },
                "--shadow-directional-normal-offset requires shadow texels in [0, 4]",
                { { "4.5", "--shadow-directional-normal-offset requires a finite value in "
                           "[0.000000, 4.000000]" } } },
            { "--shadow-filter", G, "fixed", {}, [](C& c) {
                c.shadowSettings.filterMode = ShadowFilterMode::FixedPcf; },
                "--shadow-filter requires fixed or pcss",
                { { "variance", "--shadow-filter requires fixed or pcss" } } },
            { "--shadow-spot-atlas-resolution", G, "4096", {}, [](C& c) {
                c.shadowSettings.spotAtlasResolution = 4096; },
                "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192",
                { { "1024", "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192" } } },
            { "--gbuffer-layout", G, "quality", {}, [](C& c) {
                c.gBufferLayout = GBufferLayout::CanonicalQuality; },
                "--gbuffer-layout requires reference, quality, or compact",
                { { "legacy", "unknown GBuffer layout" } } },
            { "--exposure-ev", G, "-2.5", {}, [](C& c) { c.manualExposureEv = -2.5; },
                "--exposure-ev requires a value",
                { { "17", "--exposure-ev requires a finite value in [-16, 16]" },
                  { "nan", "--exposure-ev requires a finite value in [-16, 16]" } } },
            { "--output-operator", G, "legacy", {}, [](C& c) {
                c.outputOperator = OutputTransformOperator::AcesFittedLegacy; },
                "--output-operator requires aces2, legacy, or identity",
                { { "reinhard", "--output-operator requires aces2, legacy, or identity" } } },
            { "--output-transport", G, "hdr10", {}, [](C& c) {
                c.outputTransport = Color::OutputTransport::Hdr10Pq; },
                "--output-transport requires auto, sdr, scrgb, or hdr10",
                { { "dolby-vision", "--output-transport requires auto, sdr, scrgb, or hdr10" } } },
            { "--paper-white-nits", G, "250", {}, [](C& c) { c.paperWhiteNits = 250.0; },
                "--paper-white-nits requires a value",
                { { "20", "--paper-white-nits requires a finite luminance in "
                          "[80.000000, 1000.000000] nits" } } },
            { "--peak-nits", G, "1200", {}, [](C& c) { c.peakNits = 1200.0; },
                "--peak-nits requires a value",
                { { "50", "--peak-nits requires a finite luminance in "
                          "[100.000000, 10000.000000] nits" } } },
            { "--debug-view", G, "depth", {}, [](C& c) { c.debugView = RenderDebugView::Depth; },
                "--debug-view requires a view name",
                { { "bogus", "Unknown debug view: bogus" }, { "", "Unknown debug view: " } } },
            { "--experimental-shadow-lod-error-texels", G, "1.5", {}, [](C& c) {
                c.experimentalShadowLodErrorTexels = 1.5f; },
                "--experimental-shadow-lod-error-texels requires a texel threshold",
                { { "0", "--experimental-shadow-lod-error-texels requires a finite value in "
                         "[0.010000, 64.000000]" } } },
            { "--experimental-virtual-shadow-resources", G, {}, {}, [](C& c) {
                c.experimentalVirtualShadowResources = true; }, {}, {} },
            { "--shadow-lod-max-level", G, "2", {}, [](C& c) { c.shadowLodMaximumLevel = 2; },
                "--shadow-lod-max-level requires 0..15 (zero pins LOD0)",
                { { "16", "--shadow-lod-max-level requires 0..15" },
                  { "a", "--shadow-lod-max-level requires an unsigned integer" } } },
            { "--experimental-gpu-lod-error-pixels", G, "2.5", {}, [](C& c) {
                c.experimentalGpuLodErrorPixels = 2.5f; },
                "--experimental-gpu-lod-error-pixels requires a pixel threshold",
                { { "65", "--experimental-gpu-lod-error-pixels requires a finite value in "
                          "[0.010000, 64.000000]" } } },
            { "--gpu-lod-max-level", G, "0", {}, [](C& c) { c.gpuLodMaximumLevel = 0; },
                "--gpu-lod-max-level requires 0..15 (zero pins LOD0)",
                { { "16", "--gpu-lod-max-level requires 0..15" } } },
            { "--gpu-lod-hysteresis-fraction", G, "0.25", {}, [](C& c) {
                c.gpuLodHysteresisFraction = 0.25f; },
                "--gpu-lod-hysteresis-fraction requires 0..0.5",
                { { "0.51", "--gpu-lod-hysteresis-fraction requires a finite value in "
                            "[0.000000, 0.500000]" } } },
            { "--experimental-probe-lod-error-pixels", G, "3.5", {}, [](C& c) {
                c.experimentalProbeLodErrorPixels = 3.5f; },
                "--experimental-probe-lod-error-pixels requires a pixel threshold",
                { { "inf", "--experimental-probe-lod-error-pixels requires a finite value in "
                           "[0.010000, 64.000000]" } } },
            { "--probe-lod-max-level", G, "3", {}, [](C& c) { c.probeLodMaximumLevel = 3; },
                "--probe-lod-max-level requires 0..15 (zero pins LOD0)",
                { { "16", "--probe-lod-max-level requires 0..15" } } },
            { "--experimental-depth-pyramid", G, {}, {}, [](C& c) {
                c.experimentalDepthPyramid = true; }, {}, {} },
            { "--experimental-depth-occlusion-query", G, {}, {}, [](C& c) {
                c.experimentalDepthPyramid = true;
                c.experimentalDepthOcclusionQuery = true; }, {}, {} },
            { "--experimental-depth-occlusion-rejection", G, {}, {}, [](C& c) {
                c.experimentalDepthPyramid = true;
                c.experimentalDepthOcclusionQuery = true;
                c.experimentalDepthOcclusionRejection = true; }, {}, {} },
            // M7R R4c.4 (after the frozen parser): `off` is checked separately.
            { "--pipeline-cache", G, "out/m7r/pipeline-cache", {}, [](C& c) {
                c.pipelineCacheDirectory = "out/m7r/pipeline-cache"; },
                "--pipeline-cache requires a directory or off",
                { { "", "--pipeline-cache requires a directory or off" } } },
            // M7R R4b.4 (not in the 6b000ad parser); on by default since R4b.6.
            { "--render-graph-aliasing", G, "off", {}, [](C& c) {
                c.renderGraphAliasing = false; },
                "--render-graph-aliasing requires on or off",
                { { "yes", "--render-graph-aliasing requires on or off" },
                  { "", "--render-graph-aliasing requires on or off" } } },
            // M7R R4d.1 (not in the 6b000ad parser); auto by default.
            { "--upload-queue", G, "graphics", {}, [](C& c) {
                c.uploadQueue = UploadQueueMode::Graphics; },
                "--upload-queue requires auto, graphics or legacy-blocking",
                { { "transfer", "--upload-queue requires auto, graphics or legacy-blocking" },
                  { "", "--upload-queue requires auto, graphics or legacy-blocking" } } },
            // --- qualification (40) ---
            { "--validate-texture-residency-churn", Q, {}, {}, [](C& c) {
                c.validateTextureResidencyChurn = true; }, {}, {} },
            { "--validate-reflection-probes", Q, {}, {}, [](C& c) {
                c.validateReflectionProbes = true; }, {}, {} },
            { "--validate-ordinary2-capture", Q, {}, {}, [](C& c) {
                c.validateOrdinary2Capture = true; }, {}, {} },
            { "--validate-ordinary2-fallback", Q, {}, {}, [](C& c) {
                c.validateOrdinary2Fallback = true; }, {}, {} },
            { "--validate-ordinary2-resize", Q, {}, {}, [](C& c) {
                c.validateOrdinary2Resize = true; }, {}, {} },
            { "--validate-weighted-oit-resize", Q, {}, {}, [](C& c) {
                c.validateWeightedOitResize = true; }, {}, {} },
            { "--validate-deep-layered-capture", Q, {}, {}, [](C& c) {
                c.validateDeepLayeredCapture = true; }, {}, {} },
            { "--validate-deep-layered-lifecycle", Q, {}, {}, [](C& c) {
                c.validateDeepLayeredLifecycle = true; }, {}, {} },
            { "--validate-output-transport-switch", Q, {}, {}, [](C& c) {
                c.validateOutputTransportSwitch = true; }, {}, {} },
            { "--deep-layered-validation-quality", Q, "cinematic8", {}, [](C& c) {
                c.deepLayeredCaptureQuality = TransparencyQuality::Cinematic8; },
                "--deep-layered-validation-quality requires hero4 or cinematic8",
                { { "ordinary2", "--deep-layered-validation-quality requires hero4 or cinematic8" } } },
            { "--validate-texture-table-scale", Q, "8192", {}, [](C& c) {
                c.validateTextureTableScale = 8192; },
                "--validate-texture-table-scale requires a view count",
                { { "0", "--validate-texture-table-scale requires 1..65535 views" },
                  { "65536", "--validate-texture-table-scale requires 1..65535 views" },
                  { "x", "--validate-texture-table-scale requires an unsigned integer" } } },
            { "--validate-material-table-scale", Q, "65536", {}, [](C& c) {
                c.validateMaterialTableScale = 65536; },
                "--validate-material-table-scale requires a record count",
                { { "65537", "--validate-material-table-scale requires 1..65536 records" } } },
            { "--validate-light-table-scale", Q, "4096", {}, [](C& c) {
                c.validateLightTableScale = 4096; },
                "--validate-light-table-scale requires a record count",
                { { "0", "--validate-light-table-scale requires 1..65536 records" } } },
            { "--cluster-stress-lights", Q, "512", {}, [](C& c) {
                c.clusterStressLightCount = 512; },
                "--cluster-stress-lights requires a light count",
                { { "65537", "--cluster-stress-lights requires 1..65536 lights" } } },
            { "--benchmark", Q, "material_lab_v1", {}, [](C& c) {
                c.benchmarkId = "material_lab_v1"; },
                "--benchmark requires a fixture ID",
                { { "", "--benchmark requires a fixture ID" } } },
            { "--benchmark-manifest", Q, "assets/m.json", {}, [](C& c) {
                c.benchmarkManifest = "assets/m.json"; },
                "--benchmark-manifest requires a path",
                { { "", "--benchmark-manifest requires a path" } } },
            { "--benchmark-disable-local-shadows", Q, {}, {}, [](C& c) {
                c.disableBenchmarkLocalShadows = true; }, {}, {} },
            { "--select-benchmark-entity", Q, {}, {}, [](C& c) {
                c.selectBenchmarkEntity = true; }, {}, {} },
            { "--weighted-oit-order-seed", Q, "63", {}, [](C& c) { c.weightedOitOrderSeed = 63; },
                "--weighted-oit-order-seed requires an unsigned integer",
                { { "-1", "--weighted-oit-order-seed requires an unsigned integer" } } },
            { "--reference-direct-gbuffer", Q, {}, {}, [](C& c) {
                c.forceDirectGBufferReference = true; }, {}, {} },
            { "--reference-direct-shadows", Q, {}, {}, [](C& c) {
                c.forceDirectShadowReference = true; }, {}, {} },
            { "--reference-direct-probe-capture", Q, {}, {}, [](C& c) {
                c.forceDirectProbeCaptureReference = true; }, {}, {} },
            { "--shadow-indirect-qualification-oracle", Q, {}, {}, [](C& c) {
                c.shadowIndirectQualificationOracle = true; }, {}, {} },
            { "--virtual-shadow-depth-qualification-oracle", Q, {}, {}, [](C& c) {
                c.experimentalVirtualShadowResources = true;
                c.virtualShadowDepthQualificationOracle = true; }, {}, {} },
            { "--gpu-lod-qualification-oracle", Q, {}, {}, [](C& c) {
                c.gpuLodQualificationOracle = true; }, {}, {} },
            { "--probe-lod-qualification-oracle", Q, {}, {}, [](C& c) {
                c.probeLodQualificationOracle = true; }, {}, {} },
            { "--depth-occlusion-qualification-oracle", Q, {}, {}, [](C& c) {
                c.depthOcclusionQualificationOracle = true; }, {}, {} },
            { "--qualification-indirect-stream-digest", Q, {}, {}, [](C& c) {
                c.indirectStreamDigest = true; }, {}, {} },
            // M7R R5c.1 (not in 6b000ad).
            { "--qualification-caster-revision-oracle", Q, {}, {}, [](C& c) {
                c.casterRevisionOracle = true; }, {}, {} },
            // M7R R4b.5 (not in 6b000ad): implies --render-graph-aliasing on.
            { "--qualification-alias-poison", Q, {}, {}, [](C& c) {
                c.renderGraphAliasing = true;
                c.aliasPoison = true; }, {}, {} },
            // M7R R4c.0 (not in 6b000ad): requires --profile-cpu-output.
            { "--qualification-scripted-changes", Q, "hitch.json",
                { "--profile-cpu-output", "p.jsonl" }, [](C& c) {
                c.scriptedChanges = "hitch.json"; c.enableCpuProfiling = true;
                c.cpuProfileOutput = "p.jsonl"; },
                "--qualification-scripted-changes requires a path",
                { { "", "--qualification-scripted-changes requires a path" } } },
            // M7R R5b.3 starvation test (not in 6b000ad).
            { "--qualification-background-cook", Q, "models/source.gltf", {}, [](C& c) {
                c.backgroundCookSource = "models/source.gltf"; },
                "--qualification-background-cook requires a path",
                { { "", "--qualification-background-cook requires a path" } } },
            { "--qualification-frame-task-probe", Q, {}, {}, [](C& c) {
                c.frameTaskProbe = true; }, {}, {} },
            { "--validate-depth-pyramid-capture", Q, {}, {}, [](C& c) {
                c.experimentalDepthPyramid = true; c.validateDepthPyramidCapture = true; },
                {}, {} },
            { "--validate-depth-pyramid-resize", Q, {}, {}, [](C& c) {
                c.experimentalDepthPyramid = true; c.validateDepthPyramidResize = true; },
                {}, {} },
            { "--gpu-lod-minimum-resident-level", Q, "2", {}, [](C& c) {
                c.gpuLodMinimumResidentLevel = 2; },
                "--gpu-lod-minimum-resident-level requires 0..15",
                { { "16", "--gpu-lod-minimum-resident-level requires 0..15" } } },
            { "--capture-frame", Q, "7", { "--capture-directory", "out/cap" }, [](C& c) {
                c.captureFrameIndex = 7; c.captureDirectory = "out/cap"; },
                "--capture-frame requires a measured-frame index",
                { { "x", "--capture-frame requires an unsigned integer" } } },
            { "--capture-directory", Q, "out/cap", { "--capture-frame", "0" }, withCapture,
                "--capture-directory requires a path",
                { { "", "--capture-directory requires a path" } } },
            { "--capture-point", Q, "final-output", {}, [](C& c) {
                c.capturePoint = FrameCapturePoint::FinalOutput; },
                "--capture-point requires scene, final-sdr, or final-output",
                { { "swapchain", "--capture-point requires scene, final-sdr, or final-output" } } },
            { "--require-capture-signal", Q, {}, captureContext, [withCapture](C& c) {
                withCapture(c); c.requireCaptureSignal = true; }, {}, {} },
            { "--profile-cpu-output", Q, "profiles/run.jsonl", {}, [](C& c) {
                c.enableCpuProfiling = true; c.cpuProfileOutput = "profiles/run.jsonl"; },
                "--profile-cpu-output requires a path",
                { { "", "--profile-cpu-output requires a path" } } },
            { "--cache-state", Q, "manually-cold-os-driver-cache", {}, [](C& c) {
                c.cacheState = "manually-cold-os-driver-cache"; },
                "--cache-state requires a state name",
                { { "cold", "Unknown cache state: cold" }, { "", "Unknown cache state: " } } },
        };
    }

    Args acceptedArgs(const FlagCase& row) {
        Args args{ row.name };
        if (!row.validValue.empty() || !row.missingMessage.empty()) {
            args.push_back(row.validValue);
        }
        args.insert(args.end(), row.context.begin(), row.context.end());
        return args;
    }

    bool testFlagTable() {
        const std::vector<FlagCase> table = flagTable();
        CombinedConfig scratch{};
        Cli::CliOptionRegistry registry;
        registerEngineOptions(registry, scratch);

        CHECK(table.size() == 92);
        CHECK(registry.options().size() == 92);
        std::set<std::string_view> names;
        std::map<std::string_view, size_t> ownerCounts;
        for (const FlagCase& row : table) {
            CHECK_MSG(names.insert(row.name).second, row.name);
            ++ownerCounts[row.owner];
            const Cli::CliOption* option = registry.find(row.name);
            CHECK_MSG(option != nullptr, row.name);
            CHECK_MSG(option->name == row.name, row.name);
            CHECK_MSG(option->owner == row.owner, row.name);
            CHECK_MSG(option->arity == (row.missingMessage.empty() ? 0 : 1), row.name);

            // Accepted, with exactly the expected fields (and implications) set.
            CombinedConfig expected{};
            row.expect(expected);
            const Args args = acceptedArgs(row);
            const std::string actual = newOutcome(args);
            CHECK_MSG(actual == "OK:" + describe(expected), joined(args) << "\n    " << actual);
            // A switch must not be a no-op on the default config (except the
            // validation switch that matches the build default).
            if (row.name != "--validation" && row.name != "--no-validation") {
                CHECK_MSG(describe(expected) != describe(CombinedConfig{}), row.name);
            }

            if (!row.missingMessage.empty()) {
                const Args missing{ row.name };
                CHECK_MSG(newOutcome(missing) == error(row.missingMessage),
                    row.name << ": " << newOutcome(missing));
                CHECK_MSG(option->missingValueMessage == row.missingMessage, row.name);
            }
            for (const InvalidValue& invalid : row.invalid) {
                Args bad{ row.name, invalid.value };
                bad.insert(bad.end(), row.context.begin(), row.context.end());
                CHECK_MSG(newOutcome(bad) == error(invalid.message),
                    joined(bad) << ": " << newOutcome(bad));
            }
        }
        for (const Cli::CliOption& option : registry.options()) {
            CHECK_MSG(names.contains(option.name), option.name);
        }
        CHECK(newOutcome({ "--qualification-scripted-changes", "hitch.json" }) ==
            error("--qualification-scripted-changes requires --profile-cpu-output"));
        {
            // --upload-queue: every value, the last one wins.
            CombinedConfig legacy{};
            legacy.uploadQueue = UploadQueueMode::LegacyBlocking;
            CHECK(newOutcome({ "--upload-queue", "legacy-blocking" }) ==
                "OK:" + describe(legacy));
            CHECK(newOutcome({ "--upload-queue", "legacy-blocking", "--upload-queue", "auto" }) ==
                "OK:" + describe(CombinedConfig{}));
        }
        {
            // --pipeline-cache off disables the cache; a later path re-enables it.
            CombinedConfig off{};
            off.pipelineCacheEnabled = false;
            CHECK(newOutcome({ "--pipeline-cache", "off" }) == "OK:" + describe(off));
            CombinedConfig path{};
            path.pipelineCacheDirectory = "cache";
            CHECK(newOutcome({ "--pipeline-cache", "off", "--pipeline-cache", "cache" }) ==
                "OK:" + describe(path));
        }
        CHECK(ownerCounts[R] == 14);
        CHECK(ownerCounts[E] == 4);
        CHECK(ownerCounts[G] == 32);
        CHECK(ownerCounts[Q] == 42);
        std::cout << "  owners: runtime " << ownerCounts[R] << ", editor " << ownerCounts[E]
                  << ", renderer " << ownerCounts[G] << ", qualification "
                  << ownerCounts[Q] << '\n';
        return true;
    }

    bool testAliasesAndRemovedFlags() {
        CHECK(newOutcome({ "-h" }) == newOutcome({ "--help" }));
        CHECK(parseApplicationConfig(Args{ "-h" }).showHelp);
        for (const std::string_view removed : {
                 std::string_view("--developer-legacy-transparency"),
                 std::string_view("--render-graph"),
                 std::string_view("--render-graph-shadow"),
                 std::string_view("--canonical-materials"),
                 std::string_view("--material-descriptors") }) {
            const Args args{ removed };
            CHECK_MSG(newOutcome(args) == error("Unknown option: " + std::string(removed)),
                removed);
        }
        CHECK(engineUsage().find("--developer-legacy-transparency") == std::string::npos);
        return true;
    }

    std::string normalizeWhitespace(std::string_view text) {
        std::string result;
        bool space = false;
        for (const char ch : text) {
            if (ch == ' ' || ch == '\t') {
                space = true;
                continue;
            }
            if (space && !result.empty() && result.back() != '\n') result += ' ';
            space = false;
            result += ch;
        }
        return result;
    }

    std::vector<std::string> optionLines(const std::string& usage) {
        std::vector<std::string> lines;
        std::istringstream stream(usage);
        for (std::string line; std::getline(stream, line);) {
            if (line.starts_with("  -")) lines.push_back(normalizeWhitespace(line));
        }
        return lines;
    }

    bool testUsageParity() {
        const std::string usage = engineUsage();
        CHECK(usage.starts_with("Usage: IridiumEngine [options]\n"));
        CHECK(optionLines(usage).size() == 92);
        // Groups appear in owner order: runtime, editor, renderer, qualification.
        const size_t runtime = usage.find("runtime options:");
        const size_t editor = usage.find("editor options:");
        const size_t renderer = usage.find("renderer options:");
        const size_t qualification = usage.find("qualification options:");
        CHECK(runtime != std::string::npos && runtime < editor && editor < renderer &&
            renderer < qualification && qualification != std::string::npos);
        CHECK(usage.find("--benchmark ID") > qualification);
        CHECK(usage.find("--window-size WIDTHxHEIGHT") < editor);
        return true;
    }

    bool testRegistryWithoutQualification() {
        ApplicationConfig config{};
        Cli::CliOptionRegistry registry;
        AppCli::registerRuntimeOptions(registry, config);
        AppCli::registerEditorOptions(registry, config);
        AppCli::registerRendererOptions(registry, config);
        CHECK(registry.options().size() == 50);
        try {
            registry.parse(Args{ "--benchmark", "material_lab_v1" });
            CHECK(false);
        }
        catch (const std::invalid_argument& error) {
            CHECK(std::string_view(error.what()) == "Unknown option: --benchmark");
        }
        for (const FlagCase& row : flagTable()) {
            CHECK_MSG((registry.find(row.name) == nullptr) == (row.owner == Q), row.name);
        }
        CHECK(registry.usage().find("--benchmark") == std::string::npos);
        CHECK(registry.usage().find("qualification options:") == std::string::npos);
        // parseApplicationConfig/applicationUsage are this OFF-shaped registry.
        CHECK(newOutcome(Args{ "--benchmark", "material_lab_v1" }).starts_with("OK:"));
        try {
            (void)parseApplicationConfig(Args{ "--benchmark", "material_lab_v1" });
            CHECK(false);
        }
        catch (const std::invalid_argument& error) {
            CHECK(std::string_view(error.what()) == "Unknown option: --benchmark");
        }
        CHECK(applicationUsage().find("--benchmark") == std::string::npos);
        CHECK(applicationUsage().find("--frame-limit COUNT") != std::string::npos);
        // Non-qualification flags and their checks still work.
        ApplicationConfig accepted{};
        Cli::CliOptionRegistry partial;
        AppCli::registerRuntimeOptions(partial, accepted);
        AppCli::registerEditorOptions(partial, accepted);
        AppCli::registerRendererOptions(partial, accepted);
        partial.parse(Args{ "--frame-limit", "3", "--output-transport", "scrgb" });
        CHECK(accepted.frameLimit == 3);
        CHECK(accepted.outputTransport == Color::OutputTransport::ScRgb);
        try {
            ApplicationConfig rejected{};
            Cli::CliOptionRegistry hdr;
            AppCli::registerRendererOptions(hdr, rejected);
            hdr.parse(Args{ "--output-transport", "scrgb", "--output-operator", "legacy" });
            CHECK(false);
        }
        catch (const std::invalid_argument& error) {
            CHECK(std::string_view(error.what()) ==
                "HDR output transports currently require --output-operator aces2");
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Per-flag table (90 flags)", testFlagTable },
        { "Aliases and removed flags", testAliasesAndRemovedFlags },
        { "Usage parity", testUsageParity },
        { "Registry without qualification", testRegistryWithoutQualification },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
