#include "editor/CoreEditorComponentRegistry.h"

#include "scene/components/LightComponent.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/BakedLightingSetComponent.h"
#include "scene/components/TransformComponent.h"
#include "scene/components/SkyComponent.h"
#include "scene/runtime/CoreComponentIds.h"

namespace Iridium {

    CoreEditorComponentRegistryResult createCoreEditorComponentRegistry() {
        EditorComponentRegistry registry;
        const auto add = [&](EditorComponentDescriptor descriptor) {
            return registry.add(std::move(descriptor));
        };

        auto name = editorComponentDescriptor<NameComponent>(
            CoreNameComponentId, "Name", 0, 80.0f,
            false, true, false);
        name.properties.push_back(
            editorPropertyDescriptor(
                "value", "Name", 0, PropertyValueType::String,
                &NameComponent::name));
        auto status = add(std::move(name));
        if (!status) return { std::move(registry), std::move(status) };

        auto transform = editorComponentDescriptor<TransformComponent>(
            CoreTransformComponentId, "Transform", 1, 220.0f,
            true, true, false);
        transform.properties.push_back(editorPropertyDescriptor(
            "position", "Position", 0, PropertyValueType::Float32x3,
            &TransformComponent::position, false, false,
            std::array<float, 3>{ 0.0f, 0.0f, 0.0f }));
        transform.properties.push_back(editorPropertyDescriptor(
            "rotation", "Rotation", 1, PropertyValueType::Float32x3,
            &TransformComponent::rotation, false, false,
            std::array<float, 3>{ 0.0f, 0.0f, 0.0f }));
        transform.properties.push_back(editorPropertyDescriptor(
            "scale", "Scale", 2, PropertyValueType::Float32x3,
            &TransformComponent::scale, false, false,
            std::array<float, 3>{ 1.0f, 1.0f, 1.0f }));
        status = add(std::move(transform));
        if (!status) return { std::move(registry), std::move(status) };

        auto relationship = editorComponentDescriptor<RelationshipComponent>(
            CoreRelationshipComponentId, "Relationship", 2, 180.0f,
            true, true, false);
        relationship.properties.push_back(editorPropertyDescriptor(
            "parent", "Parent", 0, PropertyValueType::EntityReference,
            &RelationshipComponent::parent, true, true, nullptr));
        relationship.properties.push_back(editorPropertyDescriptor(
            "sibling_order", "Sibling order", 1, PropertyValueType::Int32,
            &RelationshipComponent::siblingOrder, true, false, int32_t{ 0 }));
        status = add(std::move(relationship));
        if (!status) return { std::move(registry), std::move(status) };

        auto mesh = editorComponentDescriptor<MeshComponent>(
            CoreMeshComponentId, "Mesh", 3, 760.0f,
            true, false, true);
        mesh.properties.push_back(editorPropertyDescriptor(
            "enabled", "Enabled", 0, PropertyValueType::Boolean,
            &MeshComponent::enabled, false, false, true));
        mesh.properties.push_back(editorPropertyDescriptor(
            "model", "Model", 1, PropertyValueType::AssetReference,
            &MeshComponent::requestedAssetGuid, false, true, nullptr));
        mesh.properties.push_back(editorPropertyDescriptor(
            "material_overrides", "Material overrides", 2,
            PropertyValueType::Collection,
            &MeshComponent::materialOverrides, false, false,
            EmptyCollectionDefault{}));
        status = add(std::move(mesh));
        if (!status) return { std::move(registry), std::move(status) };

        auto light = editorComponentDescriptor<LightComponent>(
            CoreLightComponentId, "Light", 4, 180.0f,
            true, false, true);
        light.properties.push_back(editorPropertyDescriptor(
            "type", "Type", 0, PropertyValueType::Enum,
            &LightComponent::type, false, false, int32_t{ 0 },
            std::nullopt, std::nullopt,
            { "Directional", "Point", "Spot", "Area (unsupported)" }));
        light.properties.push_back(editorPropertyDescriptor(
            "color_linear_rec709", "Color (linear Rec.709/D65)", 1,
            PropertyValueType::Float32x3,
            &LightComponent::colorLinearRec709, false, false,
            std::array<float, 3>{ 1.0f, 1.0f, 1.0f }));
        light.properties.push_back(editorPropertyDescriptor(
            "illuminance_lux", "Illuminance (lux)", 2,
            PropertyValueType::Float32,
            &LightComponent::illuminanceLux, false, false, 100000.0f,
            0.0f, 10000000.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "luminous_intensity_candela", "Luminous intensity (cd)", 3,
            PropertyValueType::Float32,
            &LightComponent::luminousIntensityCandela, false, false, 10000.0f,
            0.0f, 1000000000.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "range_meters", "Range (m)", 4, PropertyValueType::Float32,
            &LightComponent::rangeMeters, false, false, 10.0f,
            0.0f, 1000.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "source_radius_meters", "Source radius (m)", 5,
            PropertyValueType::Float32,
            &LightComponent::sourceRadiusMeters, false, false, 0.05f,
            0.0f, 50.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "inner_cone_degrees", "Inner cone (degrees)", 6,
            PropertyValueType::Float32,
            &LightComponent::innerConeDegrees, false, false, 12.5f,
            0.0f, 90.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "outer_cone_degrees", "Outer cone (degrees)", 7,
            PropertyValueType::Float32,
            &LightComponent::outerConeDegrees, false, false, 45.0f,
            0.0f, 90.0f));
        light.properties.push_back(editorPropertyDescriptor(
            "casts_shadows", "Casts shadows", 8,
            PropertyValueType::Boolean, &LightComponent::castsShadows,
            false, false, true));
        light.properties.push_back(editorPropertyDescriptor(
            "shadow_quality", "Shadow quality", 9,
            PropertyValueType::Enum, &LightComponent::shadowQuality,
            false, false, int32_t{ 2 }, std::nullopt, std::nullopt,
            { "Low", "Medium", "High", "Ultra" }));
        light.properties.push_back(editorPropertyDescriptor(
            "priority", "Priority", 10, PropertyValueType::Int32,
            &LightComponent::priority, false, false, int32_t{ 0 }));
        status = add(std::move(light));
        if (!status) return { std::move(registry), std::move(status) };

        auto sky = editorComponentDescriptor<SkyComponent>(
            CoreSkyComponentId, "Sky", 5, 360.0f,
            true, false, true);
        sky.properties.push_back(editorPropertyDescriptor(
            "enabled", "Enabled", 0, PropertyValueType::Boolean,
            &SkyComponent::enabled, false, false, true));
        sky.properties.push_back(editorPropertyDescriptor(
            "mode", "Mode", 1, PropertyValueType::Enum,
            &SkyComponent::mode, false, false, int32_t{ 1 },
            std::nullopt, std::nullopt,
            { "Skybox", "HDRI", "Simulated" }));
        sky.properties.push_back(editorPropertyDescriptor(
            "priority", "Priority", 2, PropertyValueType::Int32,
            &SkyComponent::priority, false, false, int32_t{ 0 }));
        status = add(std::move(sky));
        if (!status) return { std::move(registry), std::move(status) };

        auto reflectionProbe = editorComponentDescriptor<ReflectionProbeComponent>(
            CoreReflectionProbeComponentId, "Reflection Probe", 6, 540.0f,
            true, false, true);
        reflectionProbe.properties.push_back(editorPropertyDescriptor(
            "enabled", "Enabled", 0, PropertyValueType::Boolean,
            &ReflectionProbeComponent::enabled, false, false, true));
        reflectionProbe.properties.push_back(editorPropertyDescriptor(
            "shape", "Shape", 1, PropertyValueType::Enum,
            &ReflectionProbeComponent::shape, false, false, int32_t{ 1 },
            std::nullopt, std::nullopt, { "Sphere", "Box" }));
        reflectionProbe.properties.push_back(editorPropertyDescriptor(
            "priority", "Priority", 2, PropertyValueType::Int32,
            &ReflectionProbeComponent::priority, false, false, int32_t{ 0 }));
        status = add(std::move(reflectionProbe));
        if (!status) return { std::move(registry), std::move(status) };

        auto bakedLighting = editorComponentDescriptor<BakedLightingSetComponent>(
            CoreBakedLightingSetComponentId, "Baked Lighting Set", 7, 320.0f,
            true, false, true);
        bakedLighting.properties.push_back(editorPropertyDescriptor(
            "enabled", "Enabled", 0, PropertyValueType::Boolean,
            &BakedLightingSetComponent::enabled, false, false, true));
        bakedLighting.properties.push_back(editorPropertyDescriptor(
            "lighting_asset", "Lighting asset", 1,
            PropertyValueType::AssetReference,
            &BakedLightingSetComponent::requestedLightingAssetGuid,
            false, true, nullptr));
        bakedLighting.properties.push_back(editorPropertyDescriptor(
            "diffuse_intensity", "Diffuse intensity", 2,
            PropertyValueType::Float32,
            &BakedLightingSetComponent::diffuseIntensity, false, false, 1.0f,
            0.0f, 64.0f));
        bakedLighting.properties.push_back(editorPropertyDescriptor(
            "specular_intensity", "Specular intensity", 3,
            PropertyValueType::Float32,
            &BakedLightingSetComponent::specularIntensity, false, false, 1.0f,
            0.0f, 64.0f));
        status = add(std::move(bakedLighting));
        if (!status) return { std::move(registry), std::move(status) };

        status = registry.freezeAndValidate();
        return { std::move(registry), std::move(status) };
    }

} // namespace Iridium
