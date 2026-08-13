#include "capture/CaptureArtifact.h"

#include "capture/PfmImage.h"
#include "capture/TgaImage.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <cctype>
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

    CaptureArtifactPaths writeCaptureArtifact(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata) {
        if (directory.empty()) {
            throw std::invalid_argument("Capture artifact directory cannot be empty.");
        }
        const bool sceneLinear =
            capture.pixelFormat == FrameCapturePixelFormat::Rgba32Float &&
            capture.colorDomain == FrameCaptureColorDomain::SceneLinearAcesCg;
        const bool displayLinearHdr =
            capture.pixelFormat == FrameCapturePixelFormat::Rgba32Float &&
            capture.colorDomain == FrameCaptureColorDomain::DisplayLinearHdr;
        const bool modernFinal =
            capture.colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr ||
            capture.colorDomain == FrameCaptureColorDomain::DisplayLinearHdr;
        const bool finalSdr =
            (capture.pixelFormat == FrameCapturePixelFormat::Rgba8Srgb ||
                capture.pixelFormat == FrameCapturePixelFormat::Bgra8Srgb) &&
            (capture.colorDomain == FrameCaptureColorDomain::LegacyDisplayReferred ||
                capture.colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr);
        if (!sceneLinear && !displayLinearHdr && !finalSdr) {
            throw std::invalid_argument(
                "Capture pixel format and color domain are incompatible.");
        }
        std::string stem = makeCaptureArtifactStem(
            metadata, capture.width, capture.height);
        if (capture.colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr) {
            stem += "__final-sdr";
        }
        else if (displayLinearHdr) {
            stem += "__final-hdr";
        }
        CaptureArtifactPaths paths{};
        paths.image = directory / (stem +
            ((sceneLinear || displayLinearHdr) ? ".pfm" : ".tga"));
        paths.metadata = directory / (stem + ".json");
        requireNewArtifactPaths(paths);
        CaptureArtifactPaths temporary = paths;
        temporary.image += ".tmp";
        temporary.metadata += ".tmp";
        requireNewArtifactPaths(temporary);
        std::filesystem::create_directories(directory);

        bool imageCommitted = false;
        try {
        if (sceneLinear || displayLinearHdr) writeFrameCapturePfm(temporary.image, capture);
        else writeFrameCaptureTga(temporary.image, capture);
        paths.imageSha256 = sha256File(temporary.image);

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
                { "source_pixel_format", frameCapturePixelFormatName(capture.pixelFormat) }
            } },
            { "capture", {
                { "capture_id", capture.captureId },
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
