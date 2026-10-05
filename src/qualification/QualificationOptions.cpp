
// Qualification-owned command line (M7R R2.9): validators, table-scale and
// stress generators, benchmark fixtures, reference/oracle modes, frame capture
// and run-report output. Compiled only into iridium_qualification, so these
// flags do not exist in IRIDIUM_QUALIFICATION=OFF builds.
#include "qualification/QualificationOptions.h"

#include "app/ApplicationConfig.h"
#include "app/cli/ApplicationCliOptions.h"
#include "app/cli/CliValueParsers.h"

#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        using AppCli::addSwitch;
        using AppCli::addValueOption;
        using AppCli::parseUnsigned;

        // "<name> requires 1..<maximum> <unit>" table/stress generator counts.
        uint32_t parseGeneratorCount(std::string_view text, const char* option,
            uint64_t maximum, const char* range) {
            const uint64_t count = parseUnsigned(text, option);
            if (count == 0 || count > maximum) {
                throw std::invalid_argument(std::string(option) + " requires " + range);
            }
            return static_cast<uint32_t>(count);
        }

    } // namespace

    void registerQualificationOptions(Cli::CliOptionRegistry& registry,
        QualificationOptions& options, ApplicationConfig& config) {
        const std::string_view owner = AppCli::kQualificationOwner;
        QualificationOptions& q = options;
        ApplicationConfig& c = config;

        addSwitch(registry, owner, "--validate-texture-residency-churn",
            "Exercise fallback and fence-delayed index reuse",
            [&q] { q.validateTextureResidencyChurn = true; });
        addSwitch(registry, owner, "--validate-reflection-probes",
            "Generate resident and renderable-owner probe fixtures",
            [&q] { q.validateReflectionProbes = true; });
        addSwitch(registry, owner, "--validate-ordinary2-capture",
            "Read back and verify the first measured Ordinary2 interfaces and local color",
            [&q] { q.validateOrdinary2Capture = true; });
        addSwitch(registry, owner, "--validate-ordinary2-fallback",
            "Verify invalid topology stays ThinGlass with no Ordinary2 atlas",
            [&q] { q.validateOrdinary2Fallback = true; });
        addSwitch(registry, owner, "--validate-ordinary2-resize",
            "Resize populated Ordinary2 targets and verify post-restore GPU pairing",
            [&q] { q.validateOrdinary2Resize = true; });
        addSwitch(registry, owner, "--validate-weighted-oit-resize",
            "Resize populated WeightedOIT targets and restore the base extent",
            [&q] { q.validateWeightedOitResize = true; });
        addSwitch(registry, owner, "--validate-deep-layered-capture",
            "Read back the selected deep tier and verify resolve/fallback handoff",
            [&q] { q.validateDeepLayeredCapture = true; });
        addSwitch(registry, owner, "--validate-deep-layered-lifecycle",
            "Retire/reactivate the selected deep tier twice, then verify GPU output",
            [&q] { q.validateDeepLayeredLifecycle = true; });
        addValueOption(registry, owner, "--deep-layered-validation-quality", "QUALITY",
            "Select hero4 (default) or cinematic8 validation",
            "--deep-layered-validation-quality requires hero4 or cinematic8",
            [&q](std::string_view value) {
                if (value == "hero4") {
                    q.deepLayeredCaptureQuality = TransparencyQuality::Hero4;
                }
                else if (value == "cinematic8") {
                    q.deepLayeredCaptureQuality = TransparencyQuality::Cinematic8;
                }
                else {
                    throw std::invalid_argument(
                        "--deep-layered-validation-quality requires hero4 or cinematic8");
                }
            });
        addValueOption(registry, owner, "--validate-texture-table-scale", "COUNT",
            "Grow and populate indexed view/sampler tables",
            "--validate-texture-table-scale requires a view count",
            [&q](std::string_view value) {
                q.validateTextureTableScale = parseGeneratorCount(value,
                    "--validate-texture-table-scale", 65'535, "1..65535 views");
            });
        addValueOption(registry, owner, "--validate-material-table-scale", "COUNT",
            "Grow and populate the indexed GPU material table",
            "--validate-material-table-scale requires a record count",
            [&q](std::string_view value) {
                q.validateMaterialTableScale = parseGeneratorCount(value,
                    "--validate-material-table-scale", 65'536, "1..65536 records");
            });
        addValueOption(registry, owner, "--validate-light-table-scale", "COUNT",
            "Grow and populate the GPU light record table",
            "--validate-light-table-scale requires a record count",
            [&q](std::string_view value) {
                q.validateLightTableScale = parseGeneratorCount(value,
                    "--validate-light-table-scale", 65'536, "1..65536 records");
            });
        addValueOption(registry, owner, "--cluster-stress-lights", "COUNT",
            "Generate a clustered local-light stress fixture",
            "--cluster-stress-lights requires a light count",
            [&q](std::string_view value) {
                q.clusterStressLightCount = parseGeneratorCount(value,
                    "--cluster-stress-lights", 65'536, "1..65536 lights");
            });
        addSwitch(registry, owner, "--benchmark-disable-local-shadows",
            "Disable castsShadows on generated spot/point benchmark lights",
            [&q] { q.disableBenchmarkLocalShadows = true; });
        addValueOption(registry, owner, "--profile-cpu-output", "PATH",
            "Collect and write JSON Lines telemetry",
            "--profile-cpu-output requires a path",
            [&q, &c](std::string_view value) {
                c.enableCpuProfiling = true;
                q.cpuProfileOutput = std::string(value);
            },
            true);
        addValueOption(registry, owner, "--cache-state", "NAME",
            "warm-steady-state, fresh-process-os-driver-cache-uncontrolled, or "
            "manually-cold-os-driver-cache",
            "--cache-state requires a state name",
            [&q](std::string_view state) {
                if (state != "warm-steady-state" &&
                    state != "fresh-process-os-driver-cache-uncontrolled" &&
                    state != "manually-cold-os-driver-cache") {
                    throw std::invalid_argument("Unknown cache state: " + std::string(state));
                }
                q.cacheState = state;
            });
        addSwitch(registry, owner, "--select-benchmark-entity",
            "Select the first deterministic fixture entity",
            [&q] { q.selectBenchmarkEntity = true; });
        addSwitch(registry, owner, "--validate-output-transport-switch",
            "Rebuild scRGB, HDR10, then SDR across frames",
            [&q] { q.validateOutputTransportSwitch = true; });
        addValueOption(registry, owner, "--benchmark", "ID",
            "Run a deterministic benchmark fixture",
            "--benchmark requires a fixture ID",
            [&q](std::string_view value) { q.benchmarkId = value; },
            true);
        addValueOption(registry, owner, "--benchmark-manifest", "PATH",
            "Override the M0 benchmark manifest",
            "--benchmark-manifest requires a path",
            [&q](std::string_view value) { q.benchmarkManifest = std::string(value); },
            true);
        addValueOption(registry, owner, "--weighted-oit-order-seed", "N",
            "Deterministically permute OIT draws for qualification; 0 preserves production order",
            "--weighted-oit-order-seed requires an unsigned integer",
            [&q](std::string_view value) {
                q.weightedOitOrderSeed = parseUnsigned(value, "--weighted-oit-order-seed");
            });
        addValueOption(registry, owner, "--capture-frame", "INDEX",
            "Capture zero-based measured frame INDEX",
            "--capture-frame requires a measured-frame index",
            [&q](std::string_view value) {
                q.captureFrameIndex = parseUnsigned(value, "--capture-frame");
            });
        addValueOption(registry, owner, "--capture-directory", "PATH",
            "Write a stable .tga/.json capture pair under PATH",
            "--capture-directory requires a path",
            [&q](std::string_view value) { q.captureDirectory = std::string(value); },
            true);
        addValueOption(registry, owner, "--capture-point", "NAME",
            "Capture scene (default), final-sdr, or final-output",
            "--capture-point requires scene, final-sdr, or final-output",
            [&q](std::string_view value) {
                if (value == "scene") {
                    q.capturePoint = FrameCapturePoint::SceneLinear;
                }
                else if (value == "final-sdr") {
                    q.capturePoint = FrameCapturePoint::FinalSdr;
                }
                else if (value == "final-output") {
                    q.capturePoint = FrameCapturePoint::FinalOutput;
                }
                else {
                    throw std::invalid_argument(
                        "--capture-point requires scene, final-sdr, or final-output");
                }
            });
        addSwitch(registry, owner, "--require-capture-signal",
            "Reject constant/nonfinite RGB captures (fixture sanity gate)",
            [&q] { q.requireCaptureSignal = true; });
        addSwitch(registry, owner, "--reference-direct-gbuffer",
            "Qualification: direct GBuffer, conventional-shadow, and probe-capture submission",
            [&q] { q.forceDirectGBufferReference = true; });
        addSwitch(registry, owner, "--reference-direct-shadows",
            "Qualification: conventional directional/spot/point shadows only",
            [&q] { q.forceDirectShadowReference = true; });
        addSwitch(registry, owner, "--reference-direct-probe-capture",
            "Qualification: direct probe capture while other consumers stay automatic",
            [&q] { q.forceDirectProbeCaptureReference = true; });
        addSwitch(registry, owner, "--shadow-indirect-qualification-oracle",
            "Repeat device-built shadow visibility on CPU for exact qualification",
            [&q] { q.shadowIndirectQualificationOracle = true; });
        addSwitch(registry, owner, "--virtual-shadow-depth-qualification-oracle",
            "Expensive full-depth CPU/GPU request comparison",
            [&q, &c] {
                c.experimentalVirtualShadowResources = true;
                q.virtualShadowDepthQualificationOracle = true;
            });
        addSwitch(registry, owner, "--gpu-lod-qualification-oracle",
            "Repeat GPU LOD selection on CPU for exact qualification",
            [&q] { q.gpuLodQualificationOracle = true; });
        addSwitch(registry, owner, "--probe-lod-qualification-oracle",
            "Repeat probe LOD commands on CPU for exact qualification",
            [&q] { q.probeLodQualificationOracle = true; });
        addSwitch(registry, owner, "--depth-occlusion-qualification-oracle",
            "Repeat fused occlusion projection/query independently for exact qualification",
            [&q] { q.depthOcclusionQualificationOracle = true; });
        addSwitch(registry, owner, "--qualification-indirect-stream-digest",
            "Print a digest of every GPU-driven compaction stream per view and frame",
            [&q] { q.indirectStreamDigest = true; });
        addSwitch(registry, owner, "--qualification-caster-revision-oracle",
            "Recompute the retired per-frame caster hashes and compare change frames with the caster revisions",
            [&q] { q.casterRevisionOracle = true; });
        addSwitch(registry, owner, "--qualification-extraction-verifier",
            "Run the full-walk GPU-scene observation beside the change-driven one every frame and fail on any difference",
            [&q] { q.extractionVerifier = true; });
        addSwitch(registry, owner, "--qualification-alias-poison",
            "Fill render-graph alias heaps with NaN at frame start (implies aliasing on)",
            [&q, &c] {
                c.renderGraphAliasing = true;
                q.aliasPoison = true;
            });
        addValueOption(registry, owner, "--qualification-scripted-changes", "PATH",
            "Apply a deterministic mid-run change scenario (hitch measurement)",
            "--qualification-scripted-changes requires a path",
            [&q](std::string_view value) { q.scriptedChanges = std::string(value); },
            true);
        addValueOption(registry, owner, "--qualification-background-cook", "PATH",
            "Starvation test: re-cook this asset (DDC bypassed) in a loop of Background tasks for the whole run",
            "--qualification-background-cook requires a path",
            [&q](std::string_view value) { q.backgroundCookSource = std::string(value); },
            true);
        addSwitch(registry, owner, "--qualification-frame-task-probe",
            "Starvation test: join a fixed frame-critical parallelFor every frame and report its worker start and join times",
            [&q] { q.frameTaskProbe = true; });
        addSwitch(registry, owner, "--qualification-allocation-trace",
            "Record a call stack for every steady-frame allocation (allocation.cpp.*) and report them grouped by stack (implies CPU profiling)",
            [&q, &c] {
                q.allocationTrace = true;
                c.enableCpuProfiling = true;
            });
        addSwitch(registry, owner, "--validate-depth-pyramid-capture",
            "Read back live depth and verify every pyramid mip",
            [&q, &c] {
                c.experimentalDepthPyramid = true;
                q.validateDepthPyramidCapture = true;
            });
        addSwitch(registry, owner, "--validate-depth-pyramid-resize",
            "Exercise deterministic scene-target resize/history recovery",
            [&q, &c] {
                c.experimentalDepthPyramid = true;
                q.validateDepthPyramidResize = true;
            });
        addValueOption(registry, owner, "--gpu-lod-minimum-resident-level", "N",
            "Qualification: physically withhold finer LOD index ranges (0..15)",
            "--gpu-lod-minimum-resident-level requires 0..15",
            [&q](std::string_view value) {
                const uint64_t level = parseUnsigned(value, "--gpu-lod-minimum-resident-level");
                if (level > 15u) {
                    throw std::invalid_argument(
                        "--gpu-lod-minimum-resident-level requires 0..15");
                }
                q.gpuLodMinimumResidentLevel = static_cast<uint32_t>(level);
            });

        const std::string ownerName(owner);
        registry.addValidator(ownerName, [&q] {
            if (q.captureFrameIndex.has_value() != !q.captureDirectory.empty()) {
                throw std::invalid_argument(
                    "--capture-frame and --capture-directory must be specified together");
            }
        }, AppCli::kValidateCapturePairing);
        registry.addValidator(ownerName, [&q] {
            if (q.requireCaptureSignal && !q.captureFrameIndex.has_value()) {
                throw std::invalid_argument(
                    "--require-capture-signal requires a capture request");
            }
        }, AppCli::kValidateCaptureSignal);
        registry.addValidator(ownerName, [&q, &c] {
            if (q.capturePoint == FrameCapturePoint::FinalSdr &&
                c.outputTransport != Color::OutputTransport::SdrSrgb) {
                throw std::invalid_argument(
                    "--capture-point final-sdr requires --output-transport sdr");
            }
        }, AppCli::kValidateFinalSdrCapture);
        registry.addValidator(ownerName, [&q] {
            if (q.validateLightTableScale != 0 && q.clusterStressLightCount != 0) {
                throw std::invalid_argument(
                    "Light-table and cluster-stress generators are mutually exclusive");
            }
        }, AppCli::kValidateLightGenerators);
        // After every frozen-order check: the per-frame drain timeline is
        // appended to the CPU profile.
        registry.addValidator(ownerName, [&q] {
            if (!q.scriptedChanges.empty() && q.cpuProfileOutput.empty()) {
                throw std::invalid_argument(
                    "--qualification-scripted-changes requires --profile-cpu-output");
            }
        }, AppCli::kValidateLightGenerators + 10);
    }

} // namespace Iridium
