#include "scene/runtime/CoreComponentRegistry.h"
#include "scene/runtime/CoreComponentIds.h"

#include <initializer_list>
#include <string_view>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] PropertyDescriptor property(
            std::string_view id,
            PropertyValueType type,
            uint32_t order,
            bool required = false,
            bool nullable = false,
            PropertyDefaultValue defaultValue = {},
            CollectionOrdering collectionOrdering = CollectionOrdering::Preserve) {
            PropertyReferenceKind reference = PropertyReferenceKind::None;
            if (type == PropertyValueType::EntityReference) {
                reference = PropertyReferenceKind::Entity;
            }
            else if (type == PropertyValueType::AssetReference) {
                reference = PropertyReferenceKind::Asset;
            }
            else if (type == PropertyValueType::SubassetReference) {
                reference = PropertyReferenceKind::Subasset;
            }
            return {
                .id = *PropertyId::parse(id),
                .valueType = type,
                .referenceKind = reference,
                .serializationOrder = order,
                .required = required,
                .nullable = nullable,
                .defaultValue = std::move(defaultValue),
                .collectionOrdering = collectionOrdering,
            };
        }

        [[nodiscard]] RuntimeComponentDescriptor descriptor(
            std::string_view id,
            std::string_view section,
            std::vector<PropertyDescriptor> properties,
            const RuntimeComponentCallbacks& callbacks,
            uint32_t cookedVersion = 1) {
            return {
                .id = *ComponentTypeId::parse(id),
                .cookedSectionId = *CookedSectionId::parse(section),
                .currentCookedVersion = cookedVersion,
                .properties = std::move(properties),
                .resolveReferences = callbacks.resolveReferences,
                .postLoadValidate = callbacks.postLoadValidate,
                .encodeCooked = callbacks.encodeCooked,
                .decodeCooked = callbacks.decodeCooked,
            };
        }

    } // namespace

    CoreRuntimeRegistryResult createRuntimeSceneComponentRegistry(
        const CoreRuntimeComponentCallbacks& callbacks) {
        RuntimeComponentRegistry registry;
        const auto add = [&](RuntimeComponentDescriptor value) {
            return registry.add(std::move(value));
        };

        ComponentRegistryResult status = add(descriptor(
            CoreNameComponentId, "NAM1",
            { property("value", PropertyValueType::String, 0, true) },
            callbacks.name));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreTransformComponentId, "TRN1", {
            property("position", PropertyValueType::Float32x3, 0, false, false,
                std::array<float, 3>{ 0.0f, 0.0f, 0.0f }),
            property("rotation", PropertyValueType::Float32x3, 1, false, false,
                std::array<float, 3>{ 0.0f, 0.0f, 0.0f }),
            property("scale", PropertyValueType::Float32x3, 2, false, false,
                std::array<float, 3>{ 1.0f, 1.0f, 1.0f }),
        }, callbacks.transform));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreRelationshipComponentId, "REL1", {
            property("parent", PropertyValueType::EntityReference, 0, false, true,
                nullptr),
            property("sibling_order", PropertyValueType::Int32, 1, false, false,
                int32_t{ 0 }),
        }, callbacks.relationship));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreMeshComponentId, "MSH1", {
            property("enabled", PropertyValueType::Boolean, 0, false, false, true),
            property("model", PropertyValueType::AssetReference, 1, false, true,
                nullptr),
            property("material_overrides", PropertyValueType::Collection, 2,
                false, false, EmptyCollectionDefault{},
                CollectionOrdering::SourceSubassetGuid),
        }, callbacks.mesh));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreLightComponentId, "LGT1", {
            property("type", PropertyValueType::Enum, 0, false, false, int32_t{ 0 }),
            property("color_linear_rec709", PropertyValueType::Float32x3, 1, false, false,
                std::array<float, 3>{ 1.0f, 1.0f, 1.0f }),
            property("illuminance_lux", PropertyValueType::Float32, 2, false, false, 1.0f),
            property("luminous_intensity_candela", PropertyValueType::Float32, 3, false, false, 1.0f),
            property("range_meters", PropertyValueType::Float32, 4, false, false, 10.0f),
            property("source_radius_meters", PropertyValueType::Float32, 5, false, false, 0.5f),
            property("inner_cone_degrees", PropertyValueType::Float32, 6, false, false, 12.5f),
            property("outer_cone_degrees", PropertyValueType::Float32, 7, false, false, 45.0f),
            property("casts_shadows", PropertyValueType::Boolean, 8, false, false,
                true),
            property("shadow_quality", PropertyValueType::Enum, 9, false, false,
                int32_t{ 2 }),
            property("priority", PropertyValueType::Int32, 10, false, false,
                int32_t{ 0 }),
        }, callbacks.light, 2));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreSkyComponentId, "SKY1", {
            property("enabled", PropertyValueType::Boolean, 0, false, false, true),
            property("mode", PropertyValueType::Enum, 1, false, false, int32_t{ 1 }),
            property("skybox_asset", PropertyValueType::AssetReference, 2,
                false, true, nullptr),
            property("skybox_intensity", PropertyValueType::Float32, 3, false,
                false, 1.0f),
            property("skybox_rotation_degrees", PropertyValueType::Float32, 4,
                false, false, 0.0f),
            property("skybox_visible_to_camera", PropertyValueType::Boolean, 5,
                false, false, true),
            property("hdri_environment", PropertyValueType::AssetReference, 6,
                false, true, nullptr),
            property("hdri_lighting_intensity", PropertyValueType::Float32, 7,
                false, false, 1.0f),
            property("hdri_background_intensity", PropertyValueType::Float32, 8,
                false, false, 1.0f),
            property("hdri_rotation_degrees", PropertyValueType::Float32, 9,
                false, false, 0.0f),
            property("hdri_visible_to_camera", PropertyValueType::Boolean, 10,
                false, false, true),
            property("hdri_affects_lighting", PropertyValueType::Boolean, 11,
                false, false, true),
            property("simulated_turbidity", PropertyValueType::Float32, 12,
                false, false, 2.0f),
            property("simulated_ozone", PropertyValueType::Float32, 13,
                false, false, 0.35f),
            property("simulated_ground_albedo", PropertyValueType::Float32, 14,
                false, false, 0.30f),
            property("simulated_atmosphere_height_km", PropertyValueType::Float32,
                15, false, false, 100.0f),
            property("simulated_sun_disk", PropertyValueType::Boolean, 16,
                false, false, true),
            property("simulated_aerial_perspective", PropertyValueType::Boolean,
                17, false, false, true),
            property("priority", PropertyValueType::Int32, 18, false, false,
                int32_t{ 0 }),
        }, callbacks.sky));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreReflectionProbeComponentId, "RFP1", {
            property("enabled", PropertyValueType::Boolean, 0, false, false, true),
            property("shape", PropertyValueType::Enum, 1, false, false,
                int32_t{ 1 }),
            property("sphere_radius_meters", PropertyValueType::Float32, 2,
                false, false, 5.0f),
            property("box_extents_meters", PropertyValueType::Float32x3, 3,
                false, false, std::array<float, 3>{ 5.0f, 5.0f, 5.0f }),
            property("blend_distance_meters", PropertyValueType::Float32, 4,
                false, false, 1.0f),
            property("intensity", PropertyValueType::Float32, 5, false, false,
                1.0f),
            property("priority", PropertyValueType::Int32, 6, false, false,
                int32_t{ 0 }),
            property("update_mode", PropertyValueType::Enum, 7, false, false,
                int32_t{ 1 }),
            property("parallax_mode", PropertyValueType::Enum, 8, false, false,
                int32_t{ 1 }),
            property("capture_resolution", PropertyValueType::Int32, 9,
                false, false, int32_t{ 512 }),
            property("capture_near_meters", PropertyValueType::Float32, 10,
                false, false, 0.1f),
            property("capture_far_meters", PropertyValueType::Float32, 11,
                false, false, 100.0f),
            property("capture_sky", PropertyValueType::Boolean, 12, false,
                false, true),
            property("environment", PropertyValueType::AssetReference, 13,
                false, true, nullptr),
        }, callbacks.reflectionProbe));
        if (!status) return { std::move(registry), std::move(status) };

        status = add(descriptor(CoreBakedLightingSetComponentId, "BLS1", {
            property("enabled", PropertyValueType::Boolean, 0, false, false,
                true),
            property("lighting_asset", PropertyValueType::AssetReference, 1,
                false, true, nullptr),
            property("diffuse_intensity", PropertyValueType::Float32, 2,
                false, false, 1.0f),
            property("specular_intensity", PropertyValueType::Float32, 3,
                false, false, 1.0f),
            property("apply_lightmaps", PropertyValueType::Boolean, 4,
                false, false, true),
            property("apply_probe_volumes", PropertyValueType::Boolean, 5,
                false, false, true),
            property("apply_visibility", PropertyValueType::Boolean, 6,
                false, false, true),
        }, callbacks.bakedLightingSet));
        if (!status) return { std::move(registry), std::move(status) };

        status = registry.freezeAndValidate();
        return { std::move(registry), std::move(status) };
    }

} // namespace Iridium
