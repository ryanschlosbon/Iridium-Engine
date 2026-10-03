#pragma once

// Qualification-owned command-line options (M7R R2.9): validators, table-scale
// and stress generators, benchmark fixtures, reference routes and oracles,
// frame capture and run-report output. They exist only in
// IRIDIUM_QUALIFICATION=ON builds; ApplicationConfig keeps the runtime,
// editor and renderer options.
//
// The Application still needs a few of these at run time (the reference
// routes, the WeightedOIT order seed and the resident LOD floor). The harness
// passes them through AppRunPolicy::routing, so production code never reads
// this struct.

#include "core/cli/CliOptionRegistry.h"
#include "core/types/FrameCapture.h"
#include "material/TransparencyPolicy.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Iridium {

    struct ApplicationConfig;

    struct QualificationOptions {
        // Benchmark fixtures.
        std::string benchmarkId;
        std::filesystem::path benchmarkManifest;
        bool selectBenchmarkEntity = false;
        bool disableBenchmarkLocalShadows = false;
        uint64_t weightedOitOrderSeed = 0;

        // Reference routes.
        bool forceDirectGBufferReference = false;
        bool forceDirectShadowReference = false;
        bool forceDirectProbeCaptureReference = false;
        uint32_t gpuLodMinimumResidentLevel = 0;

        // Oracles.
        bool shadowIndirectQualificationOracle = false;
        bool gpuLodQualificationOracle = false;
        bool probeLodQualificationOracle = false;
        bool virtualShadowDepthQualificationOracle = false;
        bool depthOcclusionQualificationOracle = false;
        // Hash every GPU-driven compaction stream per view/slot/frame
        // (VulkanIndirectStreamDigest); observes only, changes no work.
        bool indirectStreamDigest = false;

        // Validators.
        bool validateDepthPyramidCapture = false;
        bool validateDepthPyramidResize = false;
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

        // Frame capture and run report.
        std::optional<uint64_t> captureFrameIndex;
        bool requireCaptureSignal = false;
        FrameCapturePoint capturePoint = FrameCapturePoint::SceneLinear;
        std::filesystem::path captureDirectory;
        std::filesystem::path cpuProfileOutput;
        std::string cacheState = "unspecified";
    };

    // Registers the 37 qualification flags and their post-parse checks
    // (owner "qualification"). Flags that imply runtime or renderer behavior
    // also write `config`: --profile-cpu-output enables CPU profiling, the
    // VSM depth oracle enables the VSM resources and the depth-pyramid
    // validators enable the depth pyramid. The checks also read
    // config.outputTransport. Both objects must outlive registry.parse().
    void registerQualificationOptions(Cli::CliOptionRegistry& registry,
        QualificationOptions& options, ApplicationConfig& config);

} // namespace Iridium
