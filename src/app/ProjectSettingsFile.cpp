#include "app/ProjectSettingsFile.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace Iridium {

    namespace {

        using Json = nlohmann::json;
        constexpr int SchemaVersion = 1;

        // Reads `object[key]` into `value` when present and of a compatible
        // type; anything else keeps the current value.
        template <class T>
        void read(const Json& object, const char* key, T& value) {
            const auto found = object.find(key);
            if (found == object.end()) return;
            try {
                value = found->get<T>();
            }
            catch (const Json::exception&) {
            }
        }

        template <class Enum, size_t N>
        void readEnum(const Json& object, const char* key, Enum& value,
            const std::pair<Enum, const char*> (&names)[N]) {
            const auto found = object.find(key);
            if (found == object.end() || !found->is_string()) return;
            const std::string text = found->get<std::string>();
            for (const auto& [candidate, name] : names)
                if (text == name) value = candidate;
        }

        template <class Enum, size_t N>
        const char* enumName(Enum value, const std::pair<Enum, const char*> (&names)[N]) {
            for (const auto& [candidate, name] : names)
                if (candidate == value) return name;
            return names[0].second;
        }

        constexpr std::pair<AntiAliasingMode, const char*> AntiAliasingNames[]{
            { AntiAliasingMode::None, "none" }, { AntiAliasingMode::Taa, "taa" } };
        constexpr std::pair<ExposureMode, const char*> ExposureNames[]{
            { ExposureMode::Manual, "manual" }, { ExposureMode::Auto, "auto" } };
        constexpr std::pair<ExposureAdaptation, const char*> AdaptationNames[]{
            { ExposureAdaptation::Exponential, "exponential" },
            { ExposureAdaptation::Linear, "linear" } };
        constexpr std::pair<BloomKarisMode, const char*> KarisNames[]{
            { BloomKarisMode::Auto, "auto" }, { BloomKarisMode::Off, "off" },
            { BloomKarisMode::On, "on" } };
        constexpr std::pair<ShadowFilterMode, const char*> FilterNames[]{
            { ShadowFilterMode::FixedPcf, "fixed_pcf" },
            { ShadowFilterMode::ContactHardeningPcss, "contact_hardening_pcss" } };
        constexpr std::pair<ShadowQualityProfile, const char*> QualityNames[]{
            { ShadowQualityProfile::Low, "low" }, { ShadowQualityProfile::Medium, "medium" },
            { ShadowQualityProfile::High, "high" }, { ShadowQualityProfile::Ultra, "ultra" },
            { ShadowQualityProfile::Cinematic, "cinematic" } };

        Json taaJson(const TemporalAntiAliasingTuning& t) {
            return { { "minimum_history_weight", t.minimumHistoryWeight },
                { "maximum_history_weight", t.maximumHistoryWeight },
                { "motion_pixels_for_minimum", t.motionPixelsForMinimum },
                { "variance_gamma", t.varianceGamma },
                { "reconstruction_sharpness", t.reconstructionSharpness },
                { "still_variance_gamma", t.staticVarianceGamma },
                { "still_history_weight", t.stillHistoryWeight } };
        }
        void readTaa(const Json& j, TemporalAntiAliasingTuning& t) {
            read(j, "minimum_history_weight", t.minimumHistoryWeight);
            read(j, "maximum_history_weight", t.maximumHistoryWeight);
            read(j, "motion_pixels_for_minimum", t.motionPixelsForMinimum);
            read(j, "variance_gamma", t.varianceGamma);
            read(j, "reconstruction_sharpness", t.reconstructionSharpness);
            read(j, "still_variance_gamma", t.staticVarianceGamma);
            read(j, "still_history_weight", t.stillHistoryWeight);
        }

        Json exposureJson(const AutoExposureSettings& s) {
            return { { "adaptation", enumName(s.adaptation, AdaptationNames) },
                { "speed_up", s.speedUpEvPerSecond }, { "speed_down", s.speedDownEvPerSecond },
                { "maximum_ev_per_second", s.maximumEvPerSecond },
                { "minimum_ev100", s.minimumEv100 }, { "maximum_ev100", s.maximumEv100 },
                { "low_percentile", s.lowPercentile }, { "high_percentile", s.highPercentile },
                { "centre_weight", s.centreWeight },
                { "histogram_min_ev100", s.histogramMinEv100 },
                { "histogram_max_ev100", s.histogramMaxEv100 } };
        }
        void readExposure(const Json& j, AutoExposureSettings& s) {
            readEnum(j, "adaptation", s.adaptation, AdaptationNames);
            read(j, "speed_up", s.speedUpEvPerSecond);
            read(j, "speed_down", s.speedDownEvPerSecond);
            read(j, "maximum_ev_per_second", s.maximumEvPerSecond);
            read(j, "minimum_ev100", s.minimumEv100);
            read(j, "maximum_ev100", s.maximumEv100);
            read(j, "low_percentile", s.lowPercentile);
            read(j, "high_percentile", s.highPercentile);
            read(j, "centre_weight", s.centreWeight);
            read(j, "histogram_min_ev100", s.histogramMinEv100);
            read(j, "histogram_max_ev100", s.histogramMaxEv100);
        }

        Json bloomJson(const BloomSettings& b) {
            return { { "enabled", b.enabled }, { "intensity", b.intensity },
                { "radius", b.radius }, { "threshold", b.threshold }, { "knee", b.knee },
                { "tint", b.tint }, { "levels", b.levels },
                { "anti_firefly", enumName(b.karis, KarisNames) } };
        }
        void readBloom(const Json& j, BloomSettings& b) {
            read(j, "enabled", b.enabled);
            read(j, "intensity", b.intensity);
            read(j, "radius", b.radius);
            read(j, "threshold", b.threshold);
            read(j, "knee", b.knee);
            read(j, "tint", b.tint);
            read(j, "levels", b.levels);
            readEnum(j, "anti_firefly", b.karis, KarisNames);
        }

        Json shadowJson(const ProjectShadowSettings& s) {
            return { { "filter", enumName(s.filterMode, FilterNames) },
                { "quality", enumName(s.qualityProfile, QualityNames) },
                { "directional_source_angular_diameter_degrees", s.directionalSourceAngularDiameterDegrees },
                { "maximum_penumbra_texels", s.maximumPenumbraTexels },
                { "directional_resolution", s.directionalResolution },
                { "maximum_directional_lights", s.maximumDirectionalLights },
                { "maximum_cascade_updates_per_light", s.maximumCascadeUpdatesPerLight },
                { "directional_maximum_distance_meters", s.directionalMaximumDistanceMeters },
                { "directional_split_lambda", s.directionalSplitLambda },
                { "directional_guard_band_fraction", s.directionalGuardBandFraction },
                { "directional_depth_padding_meters", s.directionalDepthPaddingMeters },
                { "directional_receiver_depth_bias_texels", s.directionalReceiverDepthBiasTexels },
                { "directional_receiver_plane_clamp_texels", s.directionalReceiverPlaneClampTexels },
                { "directional_normal_offset_texels", s.directionalNormalOffsetTexels },
                { "spot_atlas_resolution", s.spotAtlasResolution },
                { "maximum_spot_rendered_texels_per_frame", s.maximumSpotRenderedTexelsPerFrame },
                { "maximum_compatible_spot_stale_frames", s.maximumCompatibleSpotStaleFrames },
                { "point_pool_256_capacity", s.pointPool256Capacity },
                { "point_pool_512_capacity", s.pointPool512Capacity },
                { "point_pool_1024_capacity", s.pointPool1024Capacity },
                { "maximum_point_rendered_texels_per_frame", s.maximumPointRenderedTexelsPerFrame },
                { "maximum_compatible_point_stale_frames", s.maximumCompatiblePointStaleFrames } };
        }
        void readShadows(const Json& j, ProjectShadowSettings& s) {
            readEnum(j, "filter", s.filterMode, FilterNames);
            readEnum(j, "quality", s.qualityProfile, QualityNames);
            read(j, "directional_source_angular_diameter_degrees", s.directionalSourceAngularDiameterDegrees);
            read(j, "maximum_penumbra_texels", s.maximumPenumbraTexels);
            read(j, "directional_resolution", s.directionalResolution);
            read(j, "maximum_directional_lights", s.maximumDirectionalLights);
            read(j, "maximum_cascade_updates_per_light", s.maximumCascadeUpdatesPerLight);
            read(j, "directional_maximum_distance_meters", s.directionalMaximumDistanceMeters);
            read(j, "directional_split_lambda", s.directionalSplitLambda);
            read(j, "directional_guard_band_fraction", s.directionalGuardBandFraction);
            read(j, "directional_depth_padding_meters", s.directionalDepthPaddingMeters);
            read(j, "directional_receiver_depth_bias_texels", s.directionalReceiverDepthBiasTexels);
            read(j, "directional_receiver_plane_clamp_texels", s.directionalReceiverPlaneClampTexels);
            read(j, "directional_normal_offset_texels", s.directionalNormalOffsetTexels);
            read(j, "spot_atlas_resolution", s.spotAtlasResolution);
            read(j, "maximum_spot_rendered_texels_per_frame", s.maximumSpotRenderedTexelsPerFrame);
            read(j, "maximum_compatible_spot_stale_frames", s.maximumCompatibleSpotStaleFrames);
            read(j, "point_pool_256_capacity", s.pointPool256Capacity);
            read(j, "point_pool_512_capacity", s.pointPool512Capacity);
            read(j, "point_pool_1024_capacity", s.pointPool1024Capacity);
            read(j, "maximum_point_rendered_texels_per_frame", s.maximumPointRenderedTexelsPerFrame);
            read(j, "maximum_compatible_point_stale_frames", s.maximumCompatiblePointStaleFrames);
        }

        Json probeJson(const ProjectReflectionProbeSettings& p) {
            return { { "maximum_rendered_texels_per_frame", p.maximumRenderedTexelsPerFrame },
                { "maximum_faces_per_probe_per_frame", p.maximumFacesPerProbePerFrame },
                { "maximum_captures_in_flight", p.maximumCapturesInFlight },
                { "minimum_realtime_frames_between_captures", p.minimumRealtimeFramesBetweenCaptures },
                { "prefilter_sample_count", p.prefilterSampleCount } };
        }
        void readProbes(const Json& j, ProjectReflectionProbeSettings& p) {
            read(j, "maximum_rendered_texels_per_frame", p.maximumRenderedTexelsPerFrame);
            read(j, "maximum_faces_per_probe_per_frame", p.maximumFacesPerProbePerFrame);
            read(j, "maximum_captures_in_flight", p.maximumCapturesInFlight);
            read(j, "minimum_realtime_frames_between_captures", p.minimumRealtimeFramesBetweenCaptures);
            read(j, "prefilter_sample_count", p.prefilterSampleCount);
        }

        // Settings are single precision; write them at that precision
        // (0.7, not 0.699999988079071) so the file reads and diffs cleanly.
        void roundFloats(Json& value) {
            if (value.is_number_float()) {
                char text[32];
                std::snprintf(text, sizeof(text), "%.7g", value.get<double>());
                value = std::strtod(text, nullptr);
            }
            else if (value.is_structured()) {
                for (Json& child : value) roundFloats(child);
            }
        }

    } // namespace

    std::filesystem::path defaultProjectSettingsPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "project.settings.json";
    }

    bool loadProjectSettings(const std::filesystem::path& path, ApplicationConfig& config,
        std::string& diagnostic) {
        diagnostic.clear();
        std::error_code error;
        if (!std::filesystem::exists(path, error)) return true;
        Json root;
        try {
            std::ifstream stream(path, std::ios::binary);
            root = Json::parse(stream);
        }
        catch (const std::exception& exception) {
            diagnostic = "Project settings ignored (" + path.string() + "): " + exception.what();
            return false;
        }
        if (!root.is_object()) {
            diagnostic = "Project settings ignored (" + path.string() + "): not a JSON object";
            return false;
        }
        if (const auto display = root.find("display"); display != root.end() && display->is_object()) {
            read(*display, "exposure_ev", config.manualExposureEv);
            read(*display, "paper_white_nits", config.paperWhiteNits);
            read(*display, "peak_nits", config.peakNits);
        }
        if (const auto aa = root.find("anti_aliasing"); aa != root.end() && aa->is_object()) {
            readEnum(*aa, "mode", config.antiAliasing, AntiAliasingNames);
            if (const auto taa = aa->find("taa"); taa != aa->end() && taa->is_object()) {
                TemporalAntiAliasingTuning tuning = config.taaTuning.value_or(TemporalAntiAliasingTuning{});
                readTaa(*taa, tuning);
                config.taaTuning = tuning;
            }
        }
        if (const auto exposure = root.find("exposure"); exposure != root.end() && exposure->is_object()) {
            readEnum(*exposure, "mode", config.exposureMode, ExposureNames);
            AutoExposureSettings settings = config.autoExposureSettings.value_or(AutoExposureSettings{});
            readExposure(*exposure, settings);
            config.autoExposureSettings = settings;
        }
        if (const auto bloom = root.find("bloom"); bloom != root.end() && bloom->is_object())
            readBloom(*bloom, config.bloom);
        if (const auto shadows = root.find("shadows"); shadows != root.end() && shadows->is_object())
            readShadows(*shadows, config.shadowSettings);
        if (const auto probes = root.find("reflection_probes"); probes != root.end() && probes->is_object())
            readProbes(*probes, config.reflectionProbeSettings);
        return true;
    }

    bool saveProjectSettings(const std::filesystem::path& path, const ApplicationConfig& config,
        std::string& diagnostic) {
        diagnostic.clear();
        Json root{
            { "schema_version", SchemaVersion },
            { "display", { { "exposure_ev", config.manualExposureEv },
                { "paper_white_nits", config.paperWhiteNits },
                { "peak_nits", config.peakNits } } },
            { "anti_aliasing", { { "mode", enumName(config.antiAliasing, AntiAliasingNames) },
                { "taa", taaJson(config.taaTuning.value_or(TemporalAntiAliasingTuning{})) } } },
            { "exposure", [&] {
                Json exposure = exposureJson(config.autoExposureSettings.value_or(AutoExposureSettings{}));
                exposure["mode"] = enumName(config.exposureMode, ExposureNames);
                return exposure;
            }() },
            { "bloom", bloomJson(config.bloom) },
            { "shadows", shadowJson(config.shadowSettings) },
            { "reflection_probes", probeJson(config.reflectionProbeSettings) },
        };
        roundFloats(root);
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                diagnostic = "Could not write " + temporary.string();
                return false;
            }
            stream << root.dump(2) << '\n';
            if (!stream) {
                diagnostic = "Could not write " + temporary.string();
                return false;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            diagnostic = "Could not replace " + path.string() + ": " + error.message();
            return false;
        }
        return true;
    }

} // namespace Iridium
