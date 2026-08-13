#include "scene/authoring/CoreComponentCodecs.h"

#include <string_view>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] SourcePropertyBinding binding(
            std::string_view id, std::string_view sourceName) {
            return { *PropertyId::parse(id), std::string(sourceName) };
        }

        [[nodiscard]] SourceComponentCodec codec(
            std::string_view id,
            uint32_t order,
            std::vector<SourcePropertyBinding> properties,
            const SourceComponentCallbacks& callbacks) {
            return {
                .componentId = *ComponentTypeId::parse(id),
                .currentSourceVersion = callbacks.currentSourceVersion,
                .sourceOrder = order,
                .properties = std::move(properties),
                .serializeSource = callbacks.serializeSource,
                .deserializeLocal = callbacks.deserializeLocal,
                .validateLocal = callbacks.validateLocal,
                .migrations = callbacks.migrations,
            };
        }

    } // namespace

    CoreSourceRegistryResult createSourceComponentSerializerRegistry(
        const RuntimeComponentRegistry& runtimeRegistry,
        const CoreSourceComponentCallbacks& callbacks) {
        ComponentSerializerRegistry registry;
        const auto add = [&](SourceComponentCodec value) {
            return registry.add(std::move(value));
        };

        SourceRegistryResult status = add(codec("iridium.component.name", 0,
            { binding("value", "value") }, callbacks.name));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.transform", 1, {
            binding("position", "position"),
            binding("rotation", "rotation"),
            binding("scale", "scale"),
        }, callbacks.transform));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.relationship", 2, {
            binding("parent", "parent"),
            binding("sibling_order", "siblingOrder"),
        }, callbacks.relationship));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.mesh", 3, {
            binding("enabled", "enabled"),
            binding("model", "model"),
            binding("material_overrides", "materialOverrides"),
        }, callbacks.mesh));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.light", 4, {
            binding("type", "type"),
            binding("color_linear_rec709", "colorLinearRec709"),
            binding("illuminance_lux", "illuminanceLux"),
            binding("luminous_intensity_candela", "luminousIntensityCandela"),
            binding("range_meters", "rangeMeters"),
            binding("source_radius_meters", "sourceRadiusMeters"),
            binding("inner_cone_degrees", "innerConeDegrees"),
            binding("outer_cone_degrees", "outerConeDegrees"),
            binding("casts_shadows", "castsShadows"),
            binding("shadow_quality", "shadowQuality"),
            binding("priority", "priority"),
        }, callbacks.light));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.sky", 5, {
            binding("enabled", "enabled"),
            binding("mode", "mode"),
            binding("skybox_asset", "skyboxAsset"),
            binding("skybox_intensity", "skyboxIntensity"),
            binding("skybox_rotation_degrees", "skyboxRotationDegrees"),
            binding("skybox_visible_to_camera", "skyboxVisibleToCamera"),
            binding("hdri_environment", "hdriEnvironment"),
            binding("hdri_lighting_intensity", "hdriLightingIntensity"),
            binding("hdri_background_intensity", "hdriBackgroundIntensity"),
            binding("hdri_rotation_degrees", "hdriRotationDegrees"),
            binding("hdri_visible_to_camera", "hdriVisibleToCamera"),
            binding("hdri_affects_lighting", "hdriAffectsLighting"),
            binding("simulated_turbidity", "simulatedTurbidity"),
            binding("simulated_ozone", "simulatedOzone"),
            binding("simulated_ground_albedo", "simulatedGroundAlbedo"),
            binding("simulated_atmosphere_height_km", "simulatedAtmosphereHeightKm"),
            binding("simulated_sun_disk", "simulatedSunDisk"),
            binding("simulated_aerial_perspective", "simulatedAerialPerspective"),
            binding("priority", "priority"),
        }, callbacks.sky));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.reflection_probe", 6, {
            binding("enabled", "enabled"),
            binding("shape", "shape"),
            binding("sphere_radius_meters", "sphereRadiusMeters"),
            binding("box_extents_meters", "boxExtentsMeters"),
            binding("blend_distance_meters", "blendDistanceMeters"),
            binding("intensity", "intensity"),
            binding("priority", "priority"),
            binding("update_mode", "updateMode"),
            binding("parallax_mode", "parallaxMode"),
            binding("capture_resolution", "captureResolution"),
            binding("capture_near_meters", "captureNearMeters"),
            binding("capture_far_meters", "captureFarMeters"),
            binding("capture_sky", "captureSky"),
            binding("environment", "environment"),
        }, callbacks.reflectionProbe));
        if (!status) return { std::move(registry), std::move(status) };
        status = add(codec("iridium.component.baked_lighting_set", 7, {
            binding("enabled", "enabled"),
            binding("lighting_asset", "lightingAsset"),
            binding("diffuse_intensity", "diffuseIntensity"),
            binding("specular_intensity", "specularIntensity"),
            binding("apply_lightmaps", "applyLightmaps"),
            binding("apply_probe_volumes", "applyProbeVolumes"),
            binding("apply_visibility", "applyVisibility"),
        }, callbacks.bakedLightingSet));
        if (!status) return { std::move(registry), std::move(status) };
        status = registry.freezeAndValidate(runtimeRegistry);
        return { std::move(registry), std::move(status) };
    }

} // namespace Iridium
