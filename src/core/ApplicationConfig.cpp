#include "ApplicationConfig.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Iridium {

    namespace {

        uint64_t parseUnsigned(std::string_view text, const char* option) {
            uint64_t value = 0;
            const char* first = text.data();
            const char* last = first + text.size();
            const auto [end, error] = std::from_chars(first, last, value);
            if (error != std::errc{} || end != last) {
                throw std::invalid_argument(std::string(option) +
                    " requires an unsigned integer");
            }
            return value;
        }

        double parseExposure(std::string_view text) {
            double value = 0.0;
            const auto [end, error] = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() ||
                !std::isfinite(value) || value < -16.0 || value > 16.0) {
                throw std::invalid_argument(
                    "--exposure-ev requires a finite value in [-16, 16]");
            }
            return value;
        }

        double parseLuminance(std::string_view text, const char* option,
            double minimum, double maximum) {
            double value = 0.0;
            const auto [end, error] = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() ||
                !std::isfinite(value) || value < minimum || value > maximum) {
                throw std::invalid_argument(std::string(option) +
                    " requires a finite luminance in [" +
                    std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "] nits");
            }
            return value;
        }

        double parseFiniteRange(std::string_view text, const char* option,
            double minimum, double maximum) {
            double value = 0.0;
            const auto [end, error] = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() ||
                !std::isfinite(value) || value < minimum || value > maximum) {
                throw std::invalid_argument(std::string(option) +
                    " requires a finite value in [" +
                    std::to_string(minimum) + ", " +
                    std::to_string(maximum) + "]");
            }
            return value;
        }

        std::pair<uint32_t, uint32_t> parseWindowSize(std::string_view text) {
            const size_t separator = text.find_first_of("xX");
            if (separator == std::string_view::npos) {
                throw std::invalid_argument("--window-size requires WIDTHxHEIGHT");
            }
            const uint64_t width = parseUnsigned(text.substr(0, separator), "--window-size");
            const uint64_t height = parseUnsigned(text.substr(separator + 1), "--window-size");
            if (width == 0 || height == 0 ||
                width > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("--window-size dimensions are out of range");
            }
            return { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
        }

    } // namespace

    ApplicationConfig parseApplicationConfig(
        std::span<const std::string_view> arguments) {
        ApplicationConfig config{};
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--validation") {
                config.enableValidation = true;
            }
            else if (argument == "--no-validation") {
                config.enableValidation = false;
            }
            else if (argument == "--profile-cpu") {
                config.enableCpuProfiling = true;
            }
            else if (argument == "--profile-gpu") {
                config.enableCpuProfiling = true;
                config.enableGpuProfiling = true;
            }
            else if (argument == "--profile-transparent-overdraw") {
                config.enableCpuProfiling = true;
                config.enableTransparentPipelineStatistics = true;
            }
            else if (argument == "--validate-texture-residency-churn") {
                config.validateTextureResidencyChurn = true;
            }
            else if (argument == "--validate-reflection-probes") {
                config.validateReflectionProbes = true;
            }
            else if (argument ==
                "--validate-texture-table-scale") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--validate-texture-table-scale requires a view count");
                }
                const uint64_t count =
                    parseUnsigned(
                        arguments[index],
                        "--validate-texture-table-scale");
                if (count == 0 ||
                    count > 65'535) {
                    throw std::invalid_argument(
                        "--validate-texture-table-scale requires 1..65535 views");
                }
                config.validateTextureTableScale =
                    static_cast<uint32_t>(count);
            }
            else if (argument ==
                "--validate-material-table-scale") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--validate-material-table-scale requires a record count");
                }
                const uint64_t count =
                    parseUnsigned(
                        arguments[index],
                        "--validate-material-table-scale");
                if (count == 0 ||
                    count > 65'536) {
                    throw std::invalid_argument(
                        "--validate-material-table-scale requires 1..65536 records");
                }
                config.validateMaterialTableScale =
                    static_cast<uint32_t>(count);
            }
            else if (argument == "--validate-light-table-scale") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--validate-light-table-scale requires a record count");
                }
                const uint64_t count = parseUnsigned(arguments[index],
                    "--validate-light-table-scale");
                if (count == 0 || count > 65'536) {
                    throw std::invalid_argument(
                        "--validate-light-table-scale requires 1..65536 records");
                }
                config.validateLightTableScale = static_cast<uint32_t>(count);
            }
            else if (argument == "--cluster-tile-size") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--cluster-tile-size requires 16 or 32");
                }
                const uint64_t size = parseUnsigned(arguments[index],
                    "--cluster-tile-size");
                if (size != 16 && size != 32) {
                    throw std::invalid_argument(
                        "--cluster-tile-size requires 16 or 32");
                }
                config.clusterTileSize = static_cast<uint32_t>(size);
            }
            else if (argument == "--cluster-depth-slices") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--cluster-depth-slices requires 24 or 32");
                }
                const uint64_t slices = parseUnsigned(arguments[index],
                    "--cluster-depth-slices");
                if (slices != 24 && slices != 32) {
                    throw std::invalid_argument(
                        "--cluster-depth-slices requires 24 or 32");
                }
                config.clusterDepthSlices = static_cast<uint32_t>(slices);
            }
            else if (argument == "--cluster-stress-lights") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--cluster-stress-lights requires a light count");
                }
                const uint64_t count = parseUnsigned(arguments[index],
                    "--cluster-stress-lights");
                if (count == 0 || count > 65'536) {
                    throw std::invalid_argument(
                        "--cluster-stress-lights requires 1..65536 lights");
                }
                config.clusterStressLightCount = static_cast<uint32_t>(count);
            }
            else if (argument == "--shadow-directional-resolution") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--shadow-directional-resolution requires 512, 1024, 2048, or 4096");
                }
                const uint64_t resolution = parseUnsigned(arguments[index],
                    "--shadow-directional-resolution");
                if (resolution != 512 && resolution != 1024 &&
                    resolution != 2048 && resolution != 4096) {
                    throw std::invalid_argument(
                        "--shadow-directional-resolution requires 512, 1024, 2048, or 4096");
                }
                config.shadowSettings.directionalResolution =
                    static_cast<uint32_t>(resolution);
            }
            else if (argument == "--shadow-directional-lights") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--shadow-directional-lights requires 1 or 2");
                }
                const uint64_t count = parseUnsigned(arguments[index],
                    "--shadow-directional-lights");
                if (count == 0 || count > kDirectionalShadowLightCapacity) {
                    throw std::invalid_argument(
                        "--shadow-directional-lights requires 1 or 2");
                }
                config.shadowSettings.maximumDirectionalLights =
                    static_cast<uint32_t>(count);
            }
            else if (argument == "--shadow-directional-source-diameter") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--shadow-directional-source-diameter requires degrees in [0, 5]");
                }
                config.shadowSettings.directionalSourceAngularDiameterDegrees =
                    static_cast<float>(parseFiniteRange(arguments[index],
                        "--shadow-directional-source-diameter", 0.0, 5.0));
            }
            else if (argument == "--shadow-filter") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--shadow-filter requires fixed or pcss");
                }
                if (arguments[index] == "fixed") {
                    config.shadowSettings.filterMode =
                        ShadowFilterMode::FixedPcf;
                }
                else if (arguments[index] == "pcss") {
                    config.shadowSettings.filterMode =
                        ShadowFilterMode::ContactHardeningPcss;
                }
                else {
                    throw std::invalid_argument(
                        "--shadow-filter requires fixed or pcss");
                }
            }
            else if (argument == "--shadow-spot-atlas-resolution") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192");
                }
                const uint64_t resolution = parseUnsigned(arguments[index],
                    "--shadow-spot-atlas-resolution");
                if (resolution != 2048 && resolution != 4096 &&
                    resolution != 8192) {
                    throw std::invalid_argument(
                        "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192");
                }
                config.shadowSettings.spotAtlasResolution =
                    static_cast<uint32_t>(resolution);
            }
            else if (argument == "--benchmark-disable-local-shadows") {
                config.disableBenchmarkLocalShadows = true;
            }
            else if (argument == "--gbuffer-layout") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--gbuffer-layout requires reference, quality, or compact");
                }
                const auto layout = parseGBufferLayout(arguments[index]);
                if (!layout) throw std::invalid_argument("unknown GBuffer layout");
                config.gBufferLayout = *layout;
            }
            else if (argument == "--hidden-window") {
                config.windowVisible = false;
                config.windowDecorated = false;
            }
            else if (argument == "--borderless-window") {
                config.windowDecorated = false;
            }
            else if (argument == "--show-profiler") {
                config.showProfiler = true;
            }
            else if (argument == "--show-material-diagnostics") {
                config.showMaterialDiagnostics = true;
            }
            else if (argument == "--select-benchmark-entity") {
                config.selectBenchmarkEntity = true;
            }
            else if (argument == "--wireframe") {
                config.forceWireframe = true;
            }
            else if (argument == "--exposure-ev") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--exposure-ev requires a value");
                }
                config.manualExposureEv = parseExposure(arguments[index]);
            }
            else if (argument == "--output-operator") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--output-operator requires aces2, legacy, or identity");
                }
                if (arguments[index] == "aces2") {
                    config.outputOperator = OutputTransformOperator::Aces2;
                }
                else if (arguments[index] == "legacy") {
                    config.outputOperator = OutputTransformOperator::AcesFittedLegacy;
                }
                else if (arguments[index] == "identity") {
                    config.outputOperator =
                        OutputTransformOperator::IdentityClampDiagnostic;
                }
                else {
                    throw std::invalid_argument(
                        "--output-operator requires aces2, legacy, or identity");
                }
            }
			else if (argument == "--output-transport") {
				if (++index >= arguments.size()) {
					throw std::invalid_argument(
						"--output-transport requires sdr, scrgb, or hdr10");
				}
				if (arguments[index] == "sdr") {
					config.outputTransport = Color::OutputTransport::SdrSrgb;
				}
				else if (arguments[index] == "scrgb") {
					config.outputTransport = Color::OutputTransport::ScRgb;
				}
				else if (arguments[index] == "hdr10") {
					config.outputTransport = Color::OutputTransport::Hdr10Pq;
				}
				else {
					throw std::invalid_argument(
						"--output-transport requires sdr, scrgb, or hdr10");
				}
			}
            else if (argument == "--paper-white-nits") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--paper-white-nits requires a value");
                }
                config.paperWhiteNits = parseLuminance(arguments[index],
                    "--paper-white-nits", 80.0, 1000.0);
            }
            else if (argument == "--peak-nits") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--peak-nits requires a value");
                }
                config.peakNits = parseLuminance(arguments[index],
                    "--peak-nits", 100.0, 10000.0);
            }
            else if (argument == "--debug-view") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--debug-view requires a view name");
                }
                const auto view = parseRenderDebugView(arguments[index]);
                if (!view) {
                    throw std::invalid_argument("Unknown debug view: " +
                        std::string(arguments[index]));
                }
                config.debugView = *view;
            }
            else if (argument == "--benchmark") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument("--benchmark requires a fixture ID");
                }
                config.benchmarkId = arguments[index];
            }
            else if (argument == "--benchmark-manifest") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument("--benchmark-manifest requires a path");
                }
                config.benchmarkManifest = std::string(arguments[index]);
            }
            else if (argument == "--cooked-model-artifact") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument(
                        "--cooked-model-artifact requires a path");
                }
                config.cookedModelArtifact = std::string(arguments[index]);
            }
            else if (argument == "--cooked-environment-artifact") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument(
                        "--cooked-environment-artifact requires a path");
                }
                config.cookedEnvironmentArtifact =
                    std::string(arguments[index]);
            }
            else if (argument == "--open-asset-viewer") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument(
                        "--open-asset-viewer requires an asset GUID");
                }
                config.editorAssetViewerGuid =
                    AssetGuid::parse(arguments[index]);
                if (!config.editorAssetViewerGuid ||
                    config.editorAssetViewerGuid->isNil()) {
                    throw std::invalid_argument(
                        "--open-asset-viewer requires a non-nil asset GUID");
                }
            }
            else if (argument == "--capture-frame") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--capture-frame requires a measured-frame index");
                }
                config.captureFrameIndex = parseUnsigned(
                    arguments[index], "--capture-frame");
            }
            else if (argument == "--capture-directory") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument("--capture-directory requires a path");
                }
                config.captureDirectory = std::string(arguments[index]);
            }
            else if (argument == "--capture-point") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        "--capture-point requires scene, final-sdr, or final-output");
                }
                if (arguments[index] == "scene") {
                    config.capturePoint = FrameCapturePoint::SceneLinear;
                }
                else if (arguments[index] == "final-sdr") {
                    config.capturePoint = FrameCapturePoint::FinalSdr;
                }
                else if (arguments[index] == "final-output") {
                    config.capturePoint = FrameCapturePoint::FinalOutput;
                }
                else {
                    throw std::invalid_argument(
                        "--capture-point requires scene, final-sdr, or final-output");
                }
            }
            else if (argument == "--profile-cpu-output") {
                if (++index >= arguments.size() || arguments[index].empty()) {
                    throw std::invalid_argument("--profile-cpu-output requires a path");
                }
                config.enableCpuProfiling = true;
                config.cpuProfileOutput = std::string(arguments[index]);
            }
            else if (argument == "--cache-state") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--cache-state requires a state name");
                }
                const std::string_view state = arguments[index];
                if (state != "warm-steady-state" &&
                    state != "fresh-process-os-driver-cache-uncontrolled" &&
                    state != "manually-cold-os-driver-cache") {
                    throw std::invalid_argument("Unknown cache state: " +
                        std::string(state));
                }
                config.cacheState = state;
            }
            else if (argument == "--frame-limit") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--frame-limit requires a frame count");
                }
                config.frameLimit = parseUnsigned(arguments[index], "--frame-limit");
                config.frameLimitSpecified = true;
            }
            else if (argument == "--warmup-frames") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--warmup-frames requires a frame count");
                }
                config.warmupFrameCount = parseUnsigned(arguments[index], "--warmup-frames");
                config.warmupFrameCountSpecified = true;
            }
            else if (argument == "--window-size") {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument("--window-size requires WIDTHxHEIGHT");
                }
                const auto [width, height] = parseWindowSize(arguments[index]);
                config.windowWidth = width;
                config.windowHeight = height;
            }
            else if (argument == "--help" || argument == "-h") {
                config.showHelp = true;
            }
            else {
                throw std::invalid_argument("Unknown option: " + std::string(argument));
            }
        }
        if (config.captureFrameIndex.has_value() != !config.captureDirectory.empty()) {
            throw std::invalid_argument(
                "--capture-frame and --capture-directory must be specified together");
        }
        if (config.outputTransport != Color::OutputTransport::SdrSrgb &&
            config.outputOperator != OutputTransformOperator::Aces2) {
            throw std::invalid_argument(
                "HDR output transports currently require --output-operator aces2");
        }
        if (config.peakNits < config.paperWhiteNits) {
            throw std::invalid_argument(
                "--peak-nits must be greater than or equal to --paper-white-nits");
        }
        if (config.capturePoint == FrameCapturePoint::FinalSdr &&
            config.outputTransport != Color::OutputTransport::SdrSrgb) {
            throw std::invalid_argument(
                "--capture-point final-sdr requires --output-transport sdr");
        }
        if (config.forceWireframe &&
            config.debugView != RenderDebugView::Final) {
            throw std::invalid_argument(
                "--wireframe cannot be combined with a material debug view");
        }
        if (config.validateLightTableScale != 0 &&
            config.clusterStressLightCount != 0) {
            throw std::invalid_argument(
                "Light-table and cluster-stress generators are mutually exclusive");
        }
        return config;
    }

    std::string applicationUsage() {
        return
            "Usage: IridiumEngine [options]\n"
            "  --validation                  Enable Vulkan validation\n"
            "  --no-validation               Disable Vulkan validation\n"
            "  --profile-cpu                 Collect bounded CPU frame telemetry\n"
            "  --profile-gpu                 Collect delayed Vulkan GPU timestamps\n"
            "  --profile-transparent-overdraw Collect optional transparent fragment workload\n"
            "  --validate-texture-residency-churn Exercise fallback and fence-delayed index reuse\n"
            "  --validate-reflection-probes   Generate a resident local-probe GPU fixture\n"
            "  --validate-texture-table-scale COUNT Grow and populate indexed view/sampler tables\n"
            "  --validate-material-table-scale COUNT Grow and populate the indexed GPU material table\n"
            "  --validate-light-table-scale COUNT Grow and populate the GPU light record table\n"
            "  --cluster-tile-size SIZE       Diagnostic bake-off: 16 or 32 (default) pixels\n"
            "  --cluster-depth-slices COUNT  Diagnostic bake-off: 24 (default) or 32 logarithmic slices\n"
            "  --cluster-stress-lights COUNT Generate a clustered local-light stress fixture\n"
            "  --shadow-directional-resolution SIZE Directional map size: 512, 1024, 2048, or 4096\n"
            "  --shadow-directional-lights COUNT Concurrent shadowed directional lights: 1 or 2\n"
            "  --shadow-directional-source-diameter DEGREES Directional emitter diameter: 0 to 5 degrees\n"
            "  --shadow-filter NAME           fixed or pcss (default)\n"
            "  --shadow-spot-atlas-resolution SIZE Persistent spot atlas size: 2048, 4096, or 8192\n"
            "  --benchmark-disable-local-shadows Disable castsShadows on generated spot/point benchmark lights\n"
            "  --gbuffer-layout NAME         reference (production) or quality/compact experiments\n"
            "  --profile-cpu-output PATH     Collect and write JSON Lines telemetry\n"
            "  --cache-state NAME            warm-steady-state, fresh-process-os-driver-cache-uncontrolled, or manually-cold-os-driver-cache\n"
            "  --hidden-window               Create a hidden benchmark surface\n"
            "  --borderless-window           Remove decorations from a visible surface\n"
            "  --show-profiler               Open the editor profiler panel\n"
            "  --show-material-diagnostics   Open the source-to-GPU material inspector\n"
            "  --select-benchmark-entity     Select the first deterministic fixture entity\n"
            "  --wireframe                   Capture the editor opaque-wireframe diagnostic\n"
            "  --exposure-ev EV              Manual output exposure in [-16,+16] stops\n"
            "  --output-operator NAME        aces2 (default), legacy, or identity\n"
			"  --output-transport NAME       sdr (default), scrgb, or hdr10\n"
            "  --paper-white-nits NITS      HDR UI/reference-white luminance (default 203)\n"
            "  --peak-nits NITS             HDR mastering/display peak (default 1000)\n"
            "  --debug-view NAME             final, base-color, normal, roughness, metallic, emissive, depth, ao, f0, f90, material-id, material-flags, closure-class, cluster-occupancy, cluster-overflow, direct-lighting, shadow-cascade, or shadow-visibility\n"
            "  --benchmark ID                Run a deterministic benchmark fixture\n"
            "  --benchmark-manifest PATH     Override the M0 benchmark manifest\n"
            "  --cooked-model-artifact PATH  Load a self-contained cooked model instead of source\n"
            "  --cooked-environment-artifact PATH Load a cooked cubemap/IBL environment product\n"
            "  --open-asset-viewer GUID      Open a model or material in the isolated editor viewer\n"
            "  --capture-frame INDEX         Capture zero-based measured frame INDEX\n"
            "  --capture-directory PATH      Write a stable .tga/.json capture pair under PATH\n"
            "  --capture-point NAME          Capture scene (default), final-sdr, or final-output\n"
            "  --warmup-frames COUNT         Run COUNT unmeasured frames first\n"
            "  --frame-limit COUNT           Exit after COUNT measured frames\n"
            "  --window-size WIDTHxHEIGHT    Set the render-window dimensions\n"
            "  --help, -h                    Show this help\n";
    }

} // namespace Iridium
