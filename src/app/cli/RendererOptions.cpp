#include "app/cli/ApplicationCliOptions.h"

#include "app/ApplicationConfig.h"
#include "app/cli/CliValueParsers.h"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Iridium::AppCli {

    namespace {

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

        // Shadow/GPU/probe LOD caps share one shape: "<name> requires 0..15".
        uint32_t parseLodLevel(std::string_view text, const char* option) {
            const uint64_t level = parseUnsigned(text, option);
            if (level > 15u) {
                throw std::invalid_argument(std::string(option) + " requires 0..15");
            }
            return static_cast<uint32_t>(level);
        }

    } // namespace

    void registerRendererOptions(Cli::CliOptionRegistry& registry,
        ApplicationConfig& config) {
        const std::string_view owner = kRendererOwner;
        ApplicationConfig& c = config;

        addValueOption(registry, owner, "--cluster-tile-size", "SIZE",
            "Diagnostic bake-off: 16 or 32 (default) pixels",
            "--cluster-tile-size requires 16 or 32",
            [&c](std::string_view value) {
                const uint64_t size = parseUnsigned(value, "--cluster-tile-size");
                if (size != 16 && size != 32) {
                    throw std::invalid_argument("--cluster-tile-size requires 16 or 32");
                }
                c.clusterTileSize = static_cast<uint32_t>(size);
            });
        addValueOption(registry, owner, "--cluster-depth-slices", "COUNT",
            "Diagnostic bake-off: 24 (default) or 32 logarithmic slices",
            "--cluster-depth-slices requires 24 or 32",
            [&c](std::string_view value) {
                const uint64_t slices = parseUnsigned(value, "--cluster-depth-slices");
                if (slices != 24 && slices != 32) {
                    throw std::invalid_argument("--cluster-depth-slices requires 24 or 32");
                }
                c.clusterDepthSlices = static_cast<uint32_t>(slices);
            });
        addValueOption(registry, owner, "--shadow-directional-resolution", "SIZE",
            "Directional map size: 512, 1024, 2048, or 4096",
            "--shadow-directional-resolution requires 512, 1024, 2048, or 4096",
            [&c](std::string_view value) {
                const uint64_t resolution =
                    parseUnsigned(value, "--shadow-directional-resolution");
                if (resolution != 512 && resolution != 1024 &&
                    resolution != 2048 && resolution != 4096) {
                    throw std::invalid_argument(
                        "--shadow-directional-resolution requires 512, 1024, 2048, or 4096");
                }
                c.shadowSettings.directionalResolution = static_cast<uint32_t>(resolution);
            });
        addValueOption(registry, owner, "--shadow-directional-lights", "COUNT",
            "Concurrent shadowed directional lights: 1 or 2",
            "--shadow-directional-lights requires 1 or 2",
            [&c](std::string_view value) {
                const uint64_t count = parseUnsigned(value, "--shadow-directional-lights");
                if (count == 0 || count > kDirectionalShadowLightCapacity) {
                    throw std::invalid_argument("--shadow-directional-lights requires 1 or 2");
                }
                c.shadowSettings.maximumDirectionalLights = static_cast<uint32_t>(count);
            });
        addValueOption(registry, owner, "--shadow-directional-source-diameter", "DEGREES",
            "Directional emitter diameter: 0 to 5 degrees",
            "--shadow-directional-source-diameter requires degrees in [0, 5]",
            [&c](std::string_view value) {
                c.shadowSettings.directionalSourceAngularDiameterDegrees =
                    static_cast<float>(parseFiniteRange(value,
                        "--shadow-directional-source-diameter", 0.0, 5.0));
            });
        addValueOption(registry, owner, "--shadow-directional-distance", "METRES",
            "Maximum camera-relative directional shadow coverage",
            "--shadow-directional-distance requires metres in [1, 100000]",
            [&c](std::string_view value) {
                c.shadowSettings.directionalMaximumDistanceMeters =
                    static_cast<float>(parseFiniteRange(value,
                        "--shadow-directional-distance", 1.0, 100'000.0));
            });
        addValueOption(registry, owner, "--shadow-directional-receiver-bias", "TEXELS",
            "Constant receiver-depth bias: 0 to 8 shadow texels",
            "--shadow-directional-receiver-bias requires shadow texels in [0, 8]",
            [&c](std::string_view value) {
                c.shadowSettings.directionalReceiverDepthBiasTexels =
                    static_cast<float>(parseFiniteRange(value,
                        "--shadow-directional-receiver-bias", 0.0, 8.0));
            });
        addValueOption(registry, owner, "--shadow-directional-receiver-plane-clamp", "TEXELS",
            "Receiver-plane correction clamp: 0 to 8 shadow texels",
            "--shadow-directional-receiver-plane-clamp requires shadow texels in [0, 8]",
            [&c](std::string_view value) {
                c.shadowSettings.directionalReceiverPlaneClampTexels =
                    static_cast<float>(parseFiniteRange(value,
                        "--shadow-directional-receiver-plane-clamp", 0.0, 8.0));
            });
        addValueOption(registry, owner, "--shadow-directional-normal-offset", "TEXELS",
            "Geometric-normal receiver offset: 0 to 4 shadow texels",
            "--shadow-directional-normal-offset requires shadow texels in [0, 4]",
            [&c](std::string_view value) {
                c.shadowSettings.directionalNormalOffsetTexels =
                    static_cast<float>(parseFiniteRange(value,
                        "--shadow-directional-normal-offset", 0.0, 4.0));
            });
        addValueOption(registry, owner, "--shadow-filter", "NAME",
            "fixed or pcss (default)",
            "--shadow-filter requires fixed or pcss",
            [&c](std::string_view value) {
                if (value == "fixed") {
                    c.shadowSettings.filterMode = ShadowFilterMode::FixedPcf;
                }
                else if (value == "pcss") {
                    c.shadowSettings.filterMode = ShadowFilterMode::ContactHardeningPcss;
                }
                else {
                    throw std::invalid_argument("--shadow-filter requires fixed or pcss");
                }
            });
        addValueOption(registry, owner, "--shadow-spot-atlas-resolution", "SIZE",
            "Persistent spot atlas size: 2048, 4096, or 8192",
            "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192",
            [&c](std::string_view value) {
                const uint64_t resolution =
                    parseUnsigned(value, "--shadow-spot-atlas-resolution");
                if (resolution != 2048 && resolution != 4096 && resolution != 8192) {
                    throw std::invalid_argument(
                        "--shadow-spot-atlas-resolution requires 2048, 4096, or 8192");
                }
                c.shadowSettings.spotAtlasResolution = static_cast<uint32_t>(resolution);
            });
        addValueOption(registry, owner, "--gbuffer-layout", "NAME",
            "reference (production) or quality/compact experiments",
            "--gbuffer-layout requires reference, quality, or compact",
            [&c](std::string_view value) {
                const auto layout = parseGBufferLayout(value);
                if (!layout) throw std::invalid_argument("unknown GBuffer layout");
                c.gBufferLayout = *layout;
            });
        addValueOption(registry, owner, "--exposure-ev", "EV",
            "Manual output exposure in [-16,+16] stops",
            "--exposure-ev requires a value",
            [&c](std::string_view value) { c.manualExposureEv = parseExposure(value); });
        addValueOption(registry, owner, "--output-operator", "NAME",
            "aces2 (default), legacy, or identity",
            "--output-operator requires aces2, legacy, or identity",
            [&c](std::string_view value) {
                if (value == "aces2") {
                    c.outputOperator = OutputTransformOperator::Aces2;
                }
                else if (value == "legacy") {
                    c.outputOperator = OutputTransformOperator::AcesFittedLegacy;
                }
                else if (value == "identity") {
                    c.outputOperator = OutputTransformOperator::IdentityClampDiagnostic;
                }
                else {
                    throw std::invalid_argument(
                        "--output-operator requires aces2, legacy, or identity");
                }
            });
        addValueOption(registry, owner, "--output-transport", "NAME",
            "auto, sdr (default), scrgb, or hdr10",
            "--output-transport requires auto, sdr, scrgb, or hdr10",
            [&c](std::string_view value) {
                if (value == "auto") {
                    c.outputTransport = Color::OutputTransport::Automatic;
                }
                else if (value == "sdr") {
                    c.outputTransport = Color::OutputTransport::SdrSrgb;
                }
                else if (value == "scrgb") {
                    c.outputTransport = Color::OutputTransport::ScRgb;
                }
                else if (value == "hdr10") {
                    c.outputTransport = Color::OutputTransport::Hdr10Pq;
                }
                else {
                    throw std::invalid_argument(
                        "--output-transport requires auto, sdr, scrgb, or hdr10");
                }
            });
        addValueOption(registry, owner, "--paper-white-nits", "NITS",
            "HDR UI/reference-white luminance (default 203)",
            "--paper-white-nits requires a value",
            [&c](std::string_view value) {
                c.paperWhiteNits = parseLuminance(value, "--paper-white-nits", 80.0, 1000.0);
            });
        addValueOption(registry, owner, "--peak-nits", "NITS",
            "HDR mastering/display peak (default 1000)",
            "--peak-nits requires a value",
            [&c](std::string_view value) {
                c.peakNits = parseLuminance(value, "--peak-nits", 100.0, 10000.0);
            });
        addValueOption(registry, owner, "--debug-view", "NAME",
            "final, base-color, normal, roughness, metallic, emissive, depth, ao, f0, f90, "
            "material-id, material-flags, closure-class, cluster-occupancy, cluster-overflow, "
            "direct-lighting, shadow-cascade, shadow-visibility, transparency-class, "
            "transparency-fallback, transparency-interval, transparency-pyramid-mip, "
            "transparency-layers, or transparency-overflow",
            "--debug-view requires a view name",
            [&c](std::string_view value) {
                const auto view = parseRenderDebugView(value);
                if (!view) {
                    throw std::invalid_argument("Unknown debug view: " + std::string(value));
                }
                c.debugView = *view;
            });
        addValueOption(registry, owner, "--experimental-shadow-lod-error-texels", "N",
            "Opt-in directional/spot/point shadow-map LOD (0.01..64 texels)",
            "--experimental-shadow-lod-error-texels requires a texel threshold",
            [&c](std::string_view value) {
                c.experimentalShadowLodErrorTexels = static_cast<float>(parseFiniteRange(
                    value, "--experimental-shadow-lod-error-texels", 0.01, 64.0));
            });
        addSwitch(registry, owner, "--experimental-virtual-shadow-resources",
            "M7.8 live depth page demand (no virtual sampling)",
            [&c] { c.experimentalVirtualShadowResources = true; });
        addValueOption(registry, owner, "--shadow-lod-max-level", "N",
            "Shadow-map LOD cap 0..15; zero pins LOD0",
            "--shadow-lod-max-level requires 0..15 (zero pins LOD0)",
            [&c](std::string_view value) {
                c.shadowLodMaximumLevel = parseLodLevel(value, "--shadow-lod-max-level");
            });
        addValueOption(registry, owner, "--experimental-gpu-lod-error-pixels", "N",
            "Opt-in main-GBuffer LOD (0.01..64 pixels)",
            "--experimental-gpu-lod-error-pixels requires a pixel threshold",
            [&c](std::string_view value) {
                c.experimentalGpuLodErrorPixels = static_cast<float>(parseFiniteRange(
                    value, "--experimental-gpu-lod-error-pixels", 0.01, 64.0));
            });
        addValueOption(registry, owner, "--gpu-lod-max-level", "N",
            "Experimental LOD cap 0..15; zero pins LOD0",
            "--gpu-lod-max-level requires 0..15 (zero pins LOD0)",
            [&c](std::string_view value) {
                c.gpuLodMaximumLevel = parseLodLevel(value, "--gpu-lod-max-level");
            });
        addValueOption(registry, owner, "--gpu-lod-hysteresis-fraction", "N",
            "Coarsening margin 0..0.5 (default 0.15)",
            "--gpu-lod-hysteresis-fraction requires 0..0.5",
            [&c](std::string_view value) {
                c.gpuLodHysteresisFraction = static_cast<float>(parseFiniteRange(
                    value, "--gpu-lod-hysteresis-fraction", 0.0, 0.5));
            });
        addValueOption(registry, owner, "--experimental-probe-lod-error-pixels", "N",
            "Opt-in face-invariant probe-capture LOD (0.01..64 pixels)",
            "--experimental-probe-lod-error-pixels requires a pixel threshold",
            [&c](std::string_view value) {
                c.experimentalProbeLodErrorPixels = static_cast<float>(parseFiniteRange(
                    value, "--experimental-probe-lod-error-pixels", 0.01, 64.0));
            });
        addValueOption(registry, owner, "--probe-lod-max-level", "N",
            "Probe-capture LOD cap 0..15; zero pins LOD0",
            "--probe-lod-max-level requires 0..15 (zero pins LOD0)",
            [&c](std::string_view value) {
                c.probeLodMaximumLevel = parseLodLevel(value, "--probe-lod-max-level");
            });
        addSwitch(registry, owner, "--experimental-depth-pyramid",
            "Build scene-depth mips for M7.6 qualification (no culling)",
            [&c] { c.experimentalDepthPyramid = true; });
        addSwitch(registry, owner, "--experimental-depth-occlusion-query",
            "Query eligible history without rejecting draws",
            [&c] {
                c.experimentalDepthPyramid = true;
                c.experimentalDepthOcclusionQuery = true;
            });
        addSwitch(registry, owner, "--experimental-depth-occlusion-rejection",
            "Experimental qualified Hi-Z indirect rejection",
            [&c] {
                c.experimentalDepthPyramid = true;
                c.experimentalDepthOcclusionQuery = true;
                c.experimentalDepthOcclusionRejection = true;
            });

        addValueOption(registry, owner, "--pipeline-cache", "DIR|off",
            "Pipeline cache directory (default: user cache), or off",
            "--pipeline-cache requires a directory or off",
            [&c](std::string_view value) {
                if (value.empty()) {
                    throw std::invalid_argument(
                        "--pipeline-cache requires a directory or off");
                }
                if (value == "off") {
                    c.pipelineCacheEnabled = false;
                    c.pipelineCacheDirectory.clear();
                }
                else {
                    c.pipelineCacheEnabled = true;
                    c.pipelineCacheDirectory = std::filesystem::path(value);
                }
            });

        addValueOption(registry, owner, "--render-graph-aliasing", "on|off",
            "Share memory between transient graph images (default on)",
            "--render-graph-aliasing requires on or off",
            [&c](std::string_view value) {
                if (value == "on") c.renderGraphAliasing = true;
                else if (value == "off") c.renderGraphAliasing = false;
                else throw std::invalid_argument("--render-graph-aliasing requires on or off");
            });

        addValueOption(registry, owner, "--temporal-jitter", "on|off",
            "Sub-pixel raster jitter for temporal resolve (default off)",
            "--temporal-jitter requires on or off",
            [&c](std::string_view value) {
                if (value == "on") c.temporalJitter = true;
                else if (value == "off") c.temporalJitter = false;
                else throw std::invalid_argument("--temporal-jitter requires on or off");
            });

        addValueOption(registry, owner, "--upload-queue", "auto|graphics|legacy-blocking",
            "Upload queue: transfer queue + timelines, graphics only, or pre-R4d blocking",
            "--upload-queue requires auto, graphics or legacy-blocking",
            [&c](std::string_view value) {
                if (value == "auto") c.uploadQueue = UploadQueueMode::Auto;
                else if (value == "graphics") c.uploadQueue = UploadQueueMode::Graphics;
                else if (value == "legacy-blocking")
                    c.uploadQueue = UploadQueueMode::LegacyBlocking;
                else throw std::invalid_argument(
                    "--upload-queue requires auto, graphics or legacy-blocking");
            });

        registry.addValidator(std::string(owner), [&c] {
            if (c.outputTransport != Color::OutputTransport::SdrSrgb &&
                c.outputOperator != OutputTransformOperator::Aces2) {
                throw std::invalid_argument(
                    "HDR output transports currently require --output-operator aces2");
            }
        }, kValidateHdrOperator);
        registry.addValidator(std::string(owner), [&c] {
            if (c.peakNits < c.paperWhiteNits) {
                throw std::invalid_argument(
                    "--peak-nits must be greater than or equal to --paper-white-nits");
            }
        }, kValidateNitsOrdering);
    }

} // namespace Iridium::AppCli
