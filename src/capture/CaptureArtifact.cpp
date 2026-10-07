#include "capture/CaptureArtifact.h"

#include "capture/PfmImage.h"
#include "capture/TgaImage.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace Iridium {

    namespace {

        [[nodiscard]] std::string filenameToken(std::string_view value,
            std::string_view fallback) {
            std::string result;
            result.reserve(value.size());
            bool previousUnderscore = false;
            for (const char character : value) {
                const unsigned char code = static_cast<unsigned char>(character);
                const bool keep = std::isalnum(code) != 0 || character == '-';
                if (keep) {
                    result.push_back(static_cast<char>(std::tolower(code)));
                    previousUnderscore = false;
                }
                else if (!result.empty() && !previousUnderscore) {
                    result.push_back('_');
                    previousUnderscore = true;
                }
            }
            while (!result.empty() && result.back() == '_') result.pop_back();
            return result.empty() ? std::string(fallback) : result;
        }

        void requireNewArtifactPaths(const CaptureArtifactPaths& paths) {
            if (std::filesystem::exists(paths.image) ||
                std::filesystem::exists(paths.metadata)) {
                throw std::runtime_error(
                    "Refusing to overwrite an existing capture artifact: " +
                    paths.image.parent_path().generic_string());
            }
        }

        // Called only after the image writer has validated dimensions and storage.
        // Ignore alpha and row padding: an opaque black render is still black.
        [[nodiscard]] CaptureSignalSummary captureSignal(const FrameCapture& capture) {
            const bool floating = capture.pixelFormat == FrameCapturePixelFormat::Rgba32Float;
            const bool bgra = capture.pixelFormat == FrameCapturePixelFormat::Bgra8Srgb;
            const size_t pixelBytes = floating ? 4 * sizeof(float) : 4;
            uint64_t finitePixels = 0, nonzeroPixels = 0;
            std::array<double, 3> minimum{}, maximum{};
            for (uint32_t y = 0; y < capture.height; ++y) {
                const auto* row = capture.pixels.data() + size_t(y) * capture.rowPitchBytes;
                for (uint32_t x = 0; x < capture.width; ++x) {
                    const auto* pixel = row + size_t(x) * pixelBytes;
                    std::array<double, 3> rgb{};
                    for (size_t channel = 0; channel < 3; ++channel) {
                        if (floating) {
                            float value;
                            std::memcpy(&value, pixel + channel * sizeof(float), sizeof(float));
                            rgb[channel] = value;
                        } else {
                            rgb[channel] = std::to_integer<uint8_t>(
                                pixel[bgra ? 2 - channel : channel]) / 255.0;
                        }
                    }
                    if (!std::ranges::all_of(rgb, [](double value) { return std::isfinite(value); }))
                        continue;
                    for (size_t channel = 0; channel < 3; ++channel) {
                        minimum[channel] = finitePixels ? std::min(minimum[channel], rgb[channel]) : rgb[channel];
                        maximum[channel] = finitePixels ? std::max(maximum[channel], rgb[channel]) : rgb[channel];
                    }
                    ++finitePixels;
                    nonzeroPixels += std::ranges::any_of(rgb, [](double value) { return value != 0.0; });
                }
            }
            double range = 0.0;
            for (size_t channel = 0; channel < 3; ++channel)
                range = std::max(range, maximum[channel] - minimum[channel]);
            return CaptureSignalSummary{
                .floating = floating,
                .pixelCount = uint64_t(capture.width) * capture.height,
                .finitePixels = finitePixels,
                .nonzeroPixels = nonzeroPixels,
                .minimum = minimum,
                .maximum = maximum,
                .maximumRange = range,
            };
        }

        [[nodiscard]] bool hasFiniteSpatialSignal(
            const CaptureSignalSummary& signal) noexcept {
            return signal.finitePixels == signal.pixelCount &&
                signal.maximumRange > 0.0;
        }

        [[nodiscard]] nlohmann::json signalJson(const CaptureSignalSummary& signal) {
            const uint64_t finitePixels = signal.finitePixels;
            return {
                { "sample_domain", signal.floating ? "source_linear_rgb" : "encoded_rgb_normalized_0_1" },
                { "pixel_count", signal.pixelCount }, { "finite_rgb_pixels", finitePixels },
                { "nonzero_finite_rgb_pixels", signal.nonzeroPixels },
                { "finite_pixel_rgb_min", finitePixels ? nlohmann::json(signal.minimum) : nlohmann::json(nullptr) },
                { "finite_pixel_rgb_max", finitePixels ? nlohmann::json(signal.maximum) : nlohmann::json(nullptr) },
                { "maximum_spatial_channel_range", signal.maximumRange },
                { "has_finite_spatial_signal", hasFiniteSpatialSignal(signal) },
            };
        }

        [[nodiscard]] nlohmann::json temporalJitterJson(
            const std::optional<CaptureTemporalJitter>& jitter) {
            if (!jitter) return nlohmann::json(nullptr);
            return {
                { "enabled", jitter->enabled },
                { "sequence_length", jitter->sequenceLength },
                { "sequence_index", jitter->sequenceIndex },
                { "offset_pixels", jitter->offsetPixels },
                { "offset_ndc", jitter->offsetNdc },
                { "turns_since_cut", jitter->turnsSinceCut },
                { "history_reset", jitter->historyReset },
            };
        }

        struct CaptureKind {
            bool sceneLinear = false;
            bool displayLinearHdr = false;
            bool modernFinal = false;
            bool finalSdr = false;
        };

        [[nodiscard]] CaptureKind captureKind(FrameCapturePixelFormat pixelFormat,
            FrameCaptureColorDomain colorDomain) noexcept {
            CaptureKind kind{};
            kind.sceneLinear =
                pixelFormat == FrameCapturePixelFormat::Rgba32Float &&
                colorDomain == FrameCaptureColorDomain::SceneLinearAcesCg;
            kind.displayLinearHdr =
                pixelFormat == FrameCapturePixelFormat::Rgba32Float &&
                colorDomain == FrameCaptureColorDomain::DisplayLinearHdr;
            kind.modernFinal =
                colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr ||
                colorDomain == FrameCaptureColorDomain::DisplayLinearHdr;
            kind.finalSdr =
                (pixelFormat == FrameCapturePixelFormat::Rgba8Srgb ||
                    pixelFormat == FrameCapturePixelFormat::Bgra8Srgb) &&
                (colorDomain == FrameCaptureColorDomain::LegacyDisplayReferred ||
                    colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr);
            return kind;
        }

        [[nodiscard]] std::string captureStem(const CaptureArtifactMetadata& metadata,
            uint32_t width, uint32_t height, FrameCaptureColorDomain colorDomain,
            const CaptureKind& kind) {
            std::string stem = makeCaptureArtifactStem(metadata, width, height);
            if (colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr) {
                stem += "__final-sdr";
            }
            else if (kind.displayLinearHdr) {
                stem += "__final-hdr";
            }
            return stem;
        }

    } // namespace

    std::string makeCaptureArtifactStem(const CaptureArtifactMetadata& metadata,
        uint32_t width, uint32_t height) {
        return filenameToken(metadata.fixtureId, "interactive") + "__r" +
            std::to_string(metadata.fixtureRevision) + "__" +
            filenameToken(metadata.cameraId, "interactive_camera") + "__" +
            filenameToken(metadata.debugView, "final") + "__" +
            std::to_string(width) + "x" + std::to_string(height) + "__mf" +
            std::to_string(metadata.measuredFrameIndex);
    }

    PendingCaptureImage writeCaptureImage(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata) {
        if (directory.empty()) {
            throw std::invalid_argument("Capture artifact directory cannot be empty.");
        }
        const CaptureKind kind = captureKind(capture.pixelFormat, capture.colorDomain);
        if (!kind.sceneLinear && !kind.displayLinearHdr && !kind.finalSdr) {
            throw std::invalid_argument(
                "Capture pixel format and color domain are incompatible.");
        }
        PendingCaptureImage image{};
        image.stem = captureStem(metadata, capture.width, capture.height,
            capture.colorDomain, kind);
        image.captureId = capture.captureId;
        image.width = capture.width;
        image.height = capture.height;
        image.pixelFormat = capture.pixelFormat;
        image.colorDomain = capture.colorDomain;
        CaptureArtifactPaths& paths = image.paths;
        paths.image = directory / (image.stem +
            ((kind.sceneLinear || kind.displayLinearHdr) ? ".pfm" : ".tga"));
        paths.metadata = directory / (image.stem + ".json");
        requireNewArtifactPaths(paths);
        CaptureArtifactPaths temporary = paths;
        temporary.image += ".tmp";
        temporary.metadata += ".tmp";
        requireNewArtifactPaths(temporary);
        image.temporaryImage = temporary.image;
        image.temporaryMetadata = temporary.metadata;
        std::filesystem::create_directories(directory);

        try {
            if (kind.sceneLinear || kind.displayLinearHdr) writeFrameCapturePfm(temporary.image, capture);
            else writeFrameCaptureTga(temporary.image, capture);
            image.signal = captureSignal(capture);
            if (metadata.requireSpatialSignal && !hasFiniteSpatialSignal(image.signal)) {
                throw std::runtime_error(
                    "Capture signal check failed: RGB image is constant or contains nonfinite pixels.");
            }
            paths.imageSha256 = sha256File(temporary.image);
        }
        catch (...) {
            discardCaptureImage(image);
            throw;
        }
        return image;
    }

    void discardCaptureImage(const PendingCaptureImage& image) noexcept {
        std::error_code ignored;
        if (!image.temporaryImage.empty())
            std::filesystem::remove(image.temporaryImage, ignored);
        if (!image.temporaryMetadata.empty())
            std::filesystem::remove(image.temporaryMetadata, ignored);
    }

    CaptureArtifactPaths writeCaptureArtifact(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata) {
        const PendingCaptureImage image = writeCaptureImage(directory, capture, metadata);
        return commitCaptureArtifact(image, metadata);
    }

    CaptureArtifactPaths commitCaptureArtifact(const PendingCaptureImage& image,
        const CaptureArtifactMetadata& metadata) {
        // The sidecar describes the pending image under the same names.
        const PendingCaptureImage& capture = image;
        const CaptureKind kind = captureKind(capture.pixelFormat, capture.colorDomain);
        const bool sceneLinear = kind.sceneLinear;
        const bool displayLinearHdr = kind.displayLinearHdr;
        const bool modernFinal = kind.modernFinal;
        const CaptureArtifactPaths paths = image.paths;
        CaptureArtifactPaths temporary = paths;
        temporary.image = image.temporaryImage;
        temporary.metadata = image.temporaryMetadata;

        bool imageCommitted = false;
        try {
        if (captureStem(metadata, capture.width, capture.height, capture.colorDomain,
                kind) != image.stem) {
            throw std::logic_error(
                "Capture metadata names a different artifact than its pending image.");
        }
        requireNewArtifactPaths(paths);
        nlohmann::json contentHashes = nlohmann::json::array();
        for (const auto& [path, sha] : metadata.contentHashes) {
            contentHashes.push_back({ { "path", path }, { "sha256", sha } });
        }
        nlohmann::json document{
            { "schema", "iridium.frame_capture" },
            { "schema_version", 1 },
            { "image", {
                { "path", paths.image.filename().generic_string() },
                { "sha256", paths.imageSha256 },
                { "encoding", (sceneLinear || displayLinearHdr) ? "pfm_rgb32f_little_endian" :
                    "tga_bgra8_uncompressed" },
                { "origin", "top_left" },
                { "width", capture.width },
                { "height", capture.height },
                { "row_pitch_bytes", (sceneLinear || displayLinearHdr) ? capture.width * 3u * 4u :
                    capture.width * 4u },
                { "source_pixel_format", frameCapturePixelFormatName(capture.pixelFormat) },
                { "signal", signalJson(image.signal) }
            } },
            { "capture", {
                { "capture_id", capture.captureId },
                { "spatial_signal_required", metadata.requireSpatialSignal },
                { "point", sceneLinear ?
                    "post_transparency_pre_output_scene_color" :
                    (modernFinal ? "post_output_transform_pre_ui_display_color" :
                        "post_transparency_pre_ui_scene_color") },
                { "color_domain", frameCaptureColorDomainName(capture.colorDomain) },
                { "transfer", (sceneLinear || displayLinearHdr) ? "linear" : "srgb_target_encoding" },
                { "primaries", sceneLinear ? "acescg_ap1" :
                    (displayLinearHdr &&
                        (metadata.displayProfile.find("rec2020") != std::string::npos ||
                            metadata.displayProfile.find("rec2100") != std::string::npos)
                        ? "rec2020" : "srgb_rec709") },
                { "range", "full" },
                { "output_operator", sceneLinear ? "none" :
                    (modernFinal ? metadata.outputOperator :
                        "legacy_aces_fitted_in_lighting_shader") },
                { "exposure", sceneLinear ? nlohmann::json("unapplied") :
                    (modernFinal ? nlohmann::json(metadata.manualExposureEv) :
                        nlohmann::json("no_explicit_final_exposure; environment_background_scale_1.5")) },
                { "source_scene_color_domain", "scene_linear_acescg_ap1" },
                { "gamut_mapping", sceneLinear ? "none" : metadata.gamutMapping },
                { "display_profile", sceneLinear ? "none" : metadata.displayProfile },
                { "paper_white_nits", sceneLinear ? nlohmann::json(nullptr) :
                    nlohmann::json(metadata.paperWhiteNits) },
                { "peak_nits", sceneLinear ? nlohmann::json(nullptr) :
                    nlohmann::json(metadata.peakNits) },
                { "aces_package_version", sceneLinear ? "none" :
                    metadata.acesPackageVersion },
                { "aces_transform_id", sceneLinear ? "none" :
                    metadata.acesTransformId }
            } },
            { "source", {
                { "commit", metadata.sourceCommit },
                { "branch", metadata.sourceBranch },
                { "dirty_at_configure", metadata.sourceDirtyAtConfigure }
            } },
            { "run", {
                { "build_configuration", metadata.buildConfiguration },
                { "validation_enabled", metadata.validationEnabled },
                { "cpu_profiling_enabled", metadata.cpuProfilingEnabled },
                { "gpu_profiling_requested", metadata.gpuProfilingRequested },
                { "gpu_profiling_available", metadata.gpuProfilingAvailable },
                { "window_visible", metadata.windowVisible },
                { "window_decorated", metadata.windowDecorated },
                { "warmup_frames", metadata.warmupFrameCount },
                { "measured_frame_index", metadata.measuredFrameIndex },
                { "application_frame_index", metadata.applicationFrameIndex },
                { "benchmark_state_frame_index", metadata.benchmarkStateFrameIndex }
            } },
            { "environment", {
                { "compiler", metadata.compiler },
                { "shader_compiler", metadata.shaderCompiler },
                { "operating_system", metadata.operatingSystem },
                { "cpu", metadata.cpuName },
                { "system_memory_bytes", metadata.systemMemoryBytes },
                { "gpu", metadata.gpuName },
                { "gpu_uuid", metadata.gpuUuid },
                { "gpu_driver", metadata.gpuDriver },
                { "vulkan_device_api", metadata.vulkanDeviceApiVersion },
                { "vulkan_loader_api", metadata.vulkanLoaderApiVersion },
                { "vulkan_sdk", metadata.vulkanSdkVersion },
                { "application_enabled_layers", metadata.applicationEnabledLayers },
                { "active_tools", metadata.activeTools }
            } },
            { "render_configuration", {
                { "base_resolution", {
                    { "width", capture.width }, { "height", capture.height }
                } },
                { "reconstruction_mode", metadata.reconstructionMode },
                { "temporal_jitter", temporalJitterJson(metadata.temporalJitter) },
                { "output_mode", metadata.outputMode },
                { "swapchain_format", metadata.swapchainFormat },
                { "swapchain_color_space", metadata.swapchainColorSpace },
                { "present_mode", metadata.presentMode },
                { "quality_settings", metadata.qualitySettings },
                { "cache_state", metadata.cacheState },
                { "output_operator", metadata.outputOperator },
                { "manual_exposure_ev", metadata.manualExposureEv },
                { "gamut_mapping", metadata.gamutMapping },
                { "display_profile", metadata.displayProfile },
                { "output_transfer", metadata.outputTransfer },
                { "paper_white_nits", metadata.paperWhiteNits },
                { "peak_nits", metadata.peakNits }
            } },
            { "performance", {
                { "capture_frame_perturbed", true },
                { "use_for_percentile_timing", false }
            } },
            { "benchmark", {
                { "fixture_id", metadata.fixtureId },
                { "fixture_revision", metadata.fixtureRevision },
                { "camera_id", metadata.cameraId },
                { "manifest_path", metadata.manifestPath },
                { "manifest_sha256", metadata.manifestSha256 },
                { "content_hashes", std::move(contentHashes) }
            } },
            { "model_input", {
                { "load_mode", metadata.modelLoadMode },
                { "location", metadata.modelLocation },
                { "asset_guid", metadata.modelAssetGuid },
                { "artifact_cook_key",
                    metadata.modelArtifactCookKey }
            } },
            { "environment_input", {
                { "load_mode", metadata.environmentLoadMode },
                { "location", metadata.environmentLocation },
                { "asset_guid", metadata.environmentAssetGuid },
                { "artifact_cook_key",
                    metadata.environmentArtifactCookKey },
                { "source_texture_guid",
                    metadata.environmentSourceTextureGuid },
                { "source_primaries",
                    metadata.environmentSourcePrimaries },
                { "source_radiance_scale",
                    metadata.environmentRadianceScale }
            } },
            { "directional_shadow", {
                { "active", metadata.directionalShadowActive },
                { "owner_count", metadata.directionalShadowOwnerCount },
                { "owner_uuid", metadata.directionalShadowOwner },
                { "light_slot", metadata.directionalShadowLightSlot },
                { "resolution", metadata.directionalShadowResolution },
                { "cascade_count", metadata.directionalShadowCascadeCount },
                { "sampleable_mask",
                    metadata.directionalShadowSampleableMask },
                { "omitted_directional_lights",
                    metadata.omittedShadowDirectionalLights },
                { "format", metadata.directionalShadowFormat },
                { "filter", metadata.directionalShadowFilter },
                { "source_angular_diameter_degrees",
                    metadata.directionalShadowSourceAngularDiameterDegrees },
                { "maximum_penumbra_texels",
                    metadata.directionalShadowMaximumPenumbraTexels },
                { "receiver_depth_bias_texels",
                    metadata.directionalShadowReceiverDepthBiasTexels },
                { "receiver_plane_clamp_texels",
                    metadata.directionalShadowReceiverPlaneClampTexels },
                { "normal_offset_texels",
                    metadata.directionalShadowNormalOffsetTexels },
                { "blocker_search_samples",
                    metadata.directionalShadowBlockerSearchSamples },
                { "filter_samples",
                    metadata.directionalShadowFilterSamples }
            } },
            { "debug_view", {
                { "name", metadata.debugView },
                { "semantics", metadata.debugViewSemantics }
            } },
            { "unavailable_fields", metadata.unavailableFields }
        };

        std::ofstream output(temporary.metadata, std::ios::out | std::ios::binary);
        if (!output) {
            throw std::runtime_error("Failed to open capture metadata output: " +
                paths.metadata.generic_string());
        }
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error("Failed while writing capture metadata: " +
                paths.metadata.generic_string());
        }
        output.close();
        std::filesystem::rename(temporary.image, paths.image);
        imageCommitted = true;
        std::filesystem::rename(temporary.metadata, paths.metadata);
        }
        catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary.image, ignored);
            std::filesystem::remove(temporary.metadata, ignored);
            if (imageCommitted) {
                std::filesystem::remove(paths.image, ignored);
            }
            throw;
        }
        return paths;
    }

} // namespace Iridium
