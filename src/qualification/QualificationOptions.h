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

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    struct ApplicationConfig;

    // M9 G6c --capture-frames FIRST:LAST[:STEP]: measured-frame indices,
    // inclusive, every STEP-th frame from FIRST.
    struct CaptureFrameRange {
        uint64_t first = 0;
        uint64_t last = 0;
        uint64_t step = 1;

        [[nodiscard]] bool contains(uint64_t measuredFrame) const noexcept {
            return measuredFrame >= first && measuredFrame <= last &&
                (measuredFrame - first) % step == 0u;
        }
        [[nodiscard]] uint64_t count() const noexcept {
            return (last - first) / step + 1u;
        }
        // The i-th selected measured frame (i < count()).
        [[nodiscard]] uint64_t frame(uint64_t index) const noexcept {
            return first + index * step;
        }
    };

    // Parses "FIRST:LAST" or "FIRST:LAST:STEP" (unsigned decimal, FIRST <=
    // LAST, STEP >= 1). Throws std::invalid_argument with the option's
    // message otherwise.
    [[nodiscard]] CaptureFrameRange parseCaptureFrameRange(std::string_view text);

    struct QualificationOptions {
        // Benchmark fixtures.
        std::string benchmarkId;
        std::filesystem::path benchmarkManifest;
        // M7C P1 --benchmark-model-artifact PATH (repeatable): cooked model
        // artifacts beyond --cooked-model-artifact for a multi-model fixture.
        // Each is matched to a fixture source by asset identity, not order.
        std::vector<std::filesystem::path> benchmarkModelArtifacts;
        bool selectBenchmarkEntity = false;
        bool disableBenchmarkLocalShadows = false;
        uint64_t weightedOitOrderSeed = 0;
        // M9 G6c --benchmark-hold-frame F: every benchmark-evaluated state
        // (camera pose, composition and instance motion, view history-reset
        // revision, visibility steps) is evaluated at min(frame, F). Frames
        // are benchmark frames, which are application frames: warmup frames
        // count, so measured frame m is benchmark frame warmup + m.
        std::optional<uint64_t> benchmarkHoldFrame;

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
        // M7R R5c.1/R5c.2: recompute the retired per-frame caster hashes and
        // compare their change frames with the backend's revisions
        // (VulkanCasterRevisionOracle); observes only, changes no work.
        bool casterRevisionOracle = false;
        // M7R R5c.5: run the full-walk GPU-scene observation beside the
        // change-driven one every frame and fail on any difference
        // (ExtractionVerifier); observes only, changes no work.
        bool extractionVerifier = false;
        // M7R R4b.5: fill every render-graph alias heap with a NaN pattern at
        // frame start (--qualification-alias-poison; implies aliasing on).
        bool aliasPoison = false;

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
        // M9 G6c capture sequence (exclusive with captureFrameIndex): one
        // artifact per selected measured frame, streamed to disk as each
        // readback completes.
        std::optional<CaptureFrameRange> captureFrameRange;
        bool requireCaptureSignal = false;
        FrameCapturePoint capturePoint = FrameCapturePoint::SceneLinear;
        std::filesystem::path captureDirectory;
        std::filesystem::path cpuProfileOutput;
        std::string cacheState = "unspecified";

        // M7R R4c.0 hitch scenario: deterministic mid-run scene changes
        // (qualification/harness/ScriptedChanges.h). Empty runs no changes.
        std::filesystem::path scriptedChanges;

        // M7R R5b.3 cook-while-render starvation test
        // (qualification/harness/StarvationLoad.h): re-cook this source (DDC
        // bypassed) in a loop of Background tasks for the whole run, and join
        // a fixed frame-critical parallelFor every frame.
        std::filesystem::path backgroundCookSource;
        bool frameTaskProbe = false;

        // M7R R5c.8: record a call stack for every steady-frame allocation
        // (qualification/harness/AllocationTrace.h).
        bool allocationTrace = false;
        // M9 G7: deterministic probe-capture publication.
        bool probeFinalizeDrain = false;
        // M9.5: read back the adapted exposure and histogram summary of every
        // frame (auto-exposure only) and print them at the end of the run.
        bool exposureTrace = false;
    };

    // Whether this run captures at all (--capture-frame or --capture-frames).
    [[nodiscard]] inline bool capturesFrames(
        const QualificationOptions& options) noexcept {
        return options.captureFrameIndex.has_value() ||
            options.captureFrameRange.has_value();
    }

    // Whether measured frame `measuredFrame` is captured.
    [[nodiscard]] inline bool captureSelectsFrame(
        const QualificationOptions& options, uint64_t measuredFrame) noexcept {
        if (options.captureFrameIndex)
            return *options.captureFrameIndex == measuredFrame;
        return options.captureFrameRange &&
            options.captureFrameRange->contains(measuredFrame);
    }

    // The benchmark frame whose state application frame `applicationFrame`
    // renders: itself, or the hold frame once it is reached.
    [[nodiscard]] inline uint64_t benchmarkStateFrameIndex(
        const QualificationOptions& options, uint64_t applicationFrame) noexcept {
        return options.benchmarkHoldFrame
            ? std::min(applicationFrame, *options.benchmarkHoldFrame)
            : applicationFrame;
    }

    // Registers the 46 qualification flags and their post-parse checks
    // (owner "qualification"). Flags that imply runtime or renderer behavior
    // also write `config`: --profile-cpu-output enables CPU profiling, the
    // VSM depth oracle enables the VSM resources and the depth-pyramid
    // validators enable the depth pyramid. The checks also read
    // config.outputTransport. Both objects must outlive registry.parse().
    void registerQualificationOptions(Cli::CliOptionRegistry& registry,
        QualificationOptions& options, ApplicationConfig& config);

} // namespace Iridium
