// Qualification-owned command line: validators, table-scale and stress
// generators, benchmark fixtures, reference/oracle modes, frame capture and
// run-report output. M7R R2.9 moves this file into the qualification library so
// these flags disappear from builds without it.
#include "app/cli/ApplicationCliOptions.h"

#include "app/ApplicationConfig.h"
#include "app/cli/CliValueParsers.h"

#include <stdexcept>
#include <string>

namespace Iridium::AppCli {

    namespace {

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
        ApplicationConfig& config) {
        const std::string_view owner = kQualificationOwner;
        ApplicationConfig& c = config;

        addSwitch(registry, owner, "--validate-texture-residency-churn",
            "Exercise fallback and fence-delayed index reuse",
            [&c] { c.validateTextureResidencyChurn = true; });
        addSwitch(registry, owner, "--validate-reflection-probes",
            "Generate resident and renderable-owner probe fixtures",
            [&c] { c.validateReflectionProbes = true; });
        addSwitch(registry, owner, "--validate-ordinary2-capture",
            "Read back and verify the first measured Ordinary2 interfaces and local color",
            [&c] { c.validateOrdinary2Capture = true; });
        addSwitch(registry, owner, "--validate-ordinary2-fallback",
            "Verify invalid topology stays ThinGlass with no Ordinary2 atlas",
            [&c] { c.validateOrdinary2Fallback = true; });
        addSwitch(registry, owner, "--validate-ordinary2-resize",
            "Resize populated Ordinary2 targets and verify post-restore GPU pairing",
            [&c] { c.validateOrdinary2Resize = true; });
        addSwitch(registry, owner, "--validate-weighted-oit-resize",
            "Resize populated WeightedOIT targets and restore the base extent",
            [&c] { c.validateWeightedOitResize = true; });
        addSwitch(registry, owner, "--validate-deep-layered-capture",
            "Read back the selected deep tier and verify resolve/fallback handoff",
            [&c] { c.validateDeepLayeredCapture = true; });
        addSwitch(registry, owner, "--validate-deep-layered-lifecycle",
            "Retire/reactivate the selected deep tier twice, then verify GPU output",
            [&c] { c.validateDeepLayeredLifecycle = true; });
        addValueOption(registry, owner, "--deep-layered-validation-quality", "QUALITY",
            "Select hero4 (default) or cinematic8 validation",
            "--deep-layered-validation-quality requires hero4 or cinematic8",
            [&c](std::string_view value) {
                if (value == "hero4") {
                    c.deepLayeredCaptureQuality = TransparencyQuality::Hero4;
                }
                else if (value == "cinematic8") {
                    c.deepLayeredCaptureQuality = TransparencyQuality::Cinematic8;
                }
                else {
                    throw std::invalid_argument(
                        "--deep-layered-validation-quality requires hero4 or cinematic8");
                }
            });
        addValueOption(registry, owner, "--validate-texture-table-scale", "COUNT",
            "Grow and populate indexed view/sampler tables",
            "--validate-texture-table-scale requires a view count",
            [&c](std::string_view value) {
                c.validateTextureTableScale = parseGeneratorCount(value,
                    "--validate-texture-table-scale", 65'535, "1..65535 views");
            });
        addValueOption(registry, owner, "--validate-material-table-scale", "COUNT",
            "Grow and populate the indexed GPU material table",
            "--validate-material-table-scale requires a record count",
            [&c](std::string_view value) {
                c.validateMaterialTableScale = parseGeneratorCount(value,
                    "--validate-material-table-scale", 65'536, "1..65536 records");
            });
        addValueOption(registry, owner, "--validate-light-table-scale", "COUNT",
            "Grow and populate the GPU light record table",
            "--validate-light-table-scale requires a record count",
            [&c](std::string_view value) {
                c.validateLightTableScale = parseGeneratorCount(value,
                    "--validate-light-table-scale", 65'536, "1..65536 records");
            });
        addValueOption(registry, owner, "--cluster-stress-lights", "COUNT",
            "Generate a clustered local-light stress fixture",
            "--cluster-stress-lights requires a light count",
            [&c](std::string_view value) {
                c.clusterStressLightCount = parseGeneratorCount(value,
                    "--cluster-stress-lights", 65'536, "1..65536 lights");
            });
        addSwitch(registry, owner, "--benchmark-disable-local-shadows",
            "Disable castsShadows on generated spot/point benchmark lights",
            [&c] { c.disableBenchmarkLocalShadows = true; });
        addValueOption(registry, owner, "--profile-cpu-output", "PATH",
            "Collect and write JSON Lines telemetry",
            "--profile-cpu-output requires a path",
            [&c](std::string_view value) {
                c.enableCpuProfiling = true;
                c.cpuProfileOutput = std::string(value);
            },
            true);
        addValueOption(registry, owner, "--cache-state", "NAME",
            "warm-steady-state, fresh-process-os-driver-cache-uncontrolled, or "
            "manually-cold-os-driver-cache",
            "--cache-state requires a state name",
            [&c](std::string_view state) {
                if (state != "warm-steady-state" &&
                    state != "fresh-process-os-driver-cache-uncontrolled" &&
                    state != "manually-cold-os-driver-cache") {
                    throw std::invalid_argument("Unknown cache state: " + std::string(state));
                }
                c.cacheState = state;
            });
        addSwitch(registry, owner, "--select-benchmark-entity",
            "Select the first deterministic fixture entity",
            [&c] { c.selectBenchmarkEntity = true; });
        addSwitch(registry, owner, "--validate-output-transport-switch",
            "Rebuild scRGB, HDR10, then SDR across frames",
            [&c] { c.validateOutputTransportSwitch = true; });
        addValueOption(registry, owner, "--benchmark", "ID",
            "Run a deterministic benchmark fixture",
            "--benchmark requires a fixture ID",
            [&c](std::string_view value) { c.benchmarkId = value; },
            true);
        addValueOption(registry, owner, "--benchmark-manifest", "PATH",
            "Override the M0 benchmark manifest",
            "--benchmark-manifest requires a path",
            [&c](std::string_view value) { c.benchmarkManifest = std::string(value); },
            true);
        addValueOption(registry, owner, "--weighted-oit-order-seed", "N",
            "Deterministically permute OIT draws for qualification; 0 preserves production order",
            "--weighted-oit-order-seed requires an unsigned integer",
            [&c](std::string_view value) {
                c.weightedOitOrderSeed = parseUnsigned(value, "--weighted-oit-order-seed");
            });
        addValueOption(registry, owner, "--capture-frame", "INDEX",
            "Capture zero-based measured frame INDEX",
            "--capture-frame requires a measured-frame index",
            [&c](std::string_view value) {
                c.captureFrameIndex = parseUnsigned(value, "--capture-frame");
            });
        addValueOption(registry, owner, "--capture-directory", "PATH",
            "Write a stable .tga/.json capture pair under PATH",
            "--capture-directory requires a path",
            [&c](std::string_view value) { c.captureDirectory = std::string(value); },
            true);
        addValueOption(registry, owner, "--capture-point", "NAME",
            "Capture scene (default), final-sdr, or final-output",
            "--capture-point requires scene, final-sdr, or final-output",
            [&c](std::string_view value) {
                if (value == "scene") {
                    c.capturePoint = FrameCapturePoint::SceneLinear;
                }
                else if (value == "final-sdr") {
                    c.capturePoint = FrameCapturePoint::FinalSdr;
                }
                else if (value == "final-output") {
                    c.capturePoint = FrameCapturePoint::FinalOutput;
                }
                else {
                    throw std::invalid_argument(
                        "--capture-point requires scene, final-sdr, or final-output");
                }
            });
        addSwitch(registry, owner, "--require-capture-signal",
            "Reject constant/nonfinite RGB captures (fixture sanity gate)",
            [&c] { c.requireCaptureSignal = true; });
        addSwitch(registry, owner, "--reference-direct-gbuffer",
            "Qualification: direct GBuffer, conventional-shadow, and probe-capture submission",
            [&c] { c.forceDirectGBufferReference = true; });
        addSwitch(registry, owner, "--reference-direct-shadows",
            "Qualification: conventional directional/spot/point shadows only",
            [&c] { c.forceDirectShadowReference = true; });
        addSwitch(registry, owner, "--reference-direct-probe-capture",
            "Qualification: direct probe capture while other consumers stay automatic",
            [&c] { c.forceDirectProbeCaptureReference = true; });
        addSwitch(registry, owner, "--shadow-indirect-qualification-oracle",
            "Repeat device-built shadow visibility on CPU for exact qualification",
            [&c] { c.shadowIndirectQualificationOracle = true; });
        addSwitch(registry, owner, "--virtual-shadow-depth-qualification-oracle",
            "Expensive full-depth CPU/GPU request comparison",
            [&c] {
                c.experimentalVirtualShadowResources = true;
                c.virtualShadowDepthQualificationOracle = true;
            });
        addSwitch(registry, owner, "--gpu-lod-qualification-oracle",
            "Repeat GPU LOD selection on CPU for exact qualification",
            [&c] { c.gpuLodQualificationOracle = true; });
        addSwitch(registry, owner, "--probe-lod-qualification-oracle",
            "Repeat probe LOD commands on CPU for exact qualification",
            [&c] { c.probeLodQualificationOracle = true; });
        addSwitch(registry, owner, "--depth-occlusion-qualification-oracle",
            "Repeat fused occlusion projection/query independently for exact qualification",
            [&c] { c.depthOcclusionQualificationOracle = true; });
        addSwitch(registry, owner, "--validate-depth-pyramid-capture",
            "Read back live depth and verify every pyramid mip",
            [&c] {
                c.experimentalDepthPyramid = true;
                c.validateDepthPyramidCapture = true;
            });
        addSwitch(registry, owner, "--validate-depth-pyramid-resize",
            "Exercise deterministic scene-target resize/history recovery",
            [&c] {
                c.experimentalDepthPyramid = true;
                c.validateDepthPyramidResize = true;
            });
        addValueOption(registry, owner, "--gpu-lod-minimum-resident-level", "N",
            "Qualification: physically withhold finer LOD index ranges (0..15)",
            "--gpu-lod-minimum-resident-level requires 0..15",
            [&c](std::string_view value) {
                const uint64_t level = parseUnsigned(value, "--gpu-lod-minimum-resident-level");
                if (level > 15u) {
                    throw std::invalid_argument(
                        "--gpu-lod-minimum-resident-level requires 0..15");
                }
                c.gpuLodMinimumResidentLevel = static_cast<uint32_t>(level);
            });

        const std::string ownerName(owner);
        registry.addValidator(ownerName, [&c] {
            if (c.captureFrameIndex.has_value() != !c.captureDirectory.empty()) {
                throw std::invalid_argument(
                    "--capture-frame and --capture-directory must be specified together");
            }
        }, kValidateCapturePairing);
        registry.addValidator(ownerName, [&c] {
            if (c.requireCaptureSignal && !c.captureFrameIndex.has_value()) {
                throw std::invalid_argument(
                    "--require-capture-signal requires a capture request");
            }
        }, kValidateCaptureSignal);
        registry.addValidator(ownerName, [&c] {
            if (c.capturePoint == FrameCapturePoint::FinalSdr &&
                c.outputTransport != Color::OutputTransport::SdrSrgb) {
                throw std::invalid_argument(
                    "--capture-point final-sdr requires --output-transport sdr");
            }
        }, kValidateFinalSdrCapture);
        registry.addValidator(ownerName, [&c] {
            if (c.validateLightTableScale != 0 && c.clusterStressLightCount != 0) {
                throw std::invalid_argument(
                    "Light-table and cluster-stress generators are mutually exclusive");
            }
        }, kValidateLightGenerators);
    }

} // namespace Iridium::AppCli
