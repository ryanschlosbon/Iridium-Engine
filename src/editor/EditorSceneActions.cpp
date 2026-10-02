#include "editor/EditorSceneActions.h"

#include "assets/BuiltInAssets.h"
#include "ecs/Registry.h"
#include "scene/Components.h"

#include <algorithm>
#include <set>

namespace Iridium {
    std::string uniqueEntityName(
        Registry& registry,
        std::string_view preferred,
        Entity excludedEntity) {
        const std::string base =
            preferred.empty()
            ? "Entity"
            : std::string(preferred);
        std::set<std::string> names;
        auto* pool =
            registry.getPool<NameComponent>();
        for (size_t index = 0;
            index < pool->components.size();
            ++index) {
            if (pool->entities[index] ==
                excludedEntity) {
                continue;
            }
            names.insert(
                pool->components[index].name);
        }
        if (!names.contains(base)) return base;
        for (uint32_t suffix = 2;
            suffix != UINT32_MAX; ++suffix) {
            const std::string candidate =
                base + " (" +
                std::to_string(suffix) + ")";
            if (!names.contains(candidate)) {
                return candidate;
            }
        }
        return base;
    }

    Entity createEmptyEditorEntity(
        Registry& registry,
        std::string_view preferredName,
        glm::vec3 position) {
        const Entity entity =
            registry.createEntity();
        registry.addComponent<NameComponent>(
            entity, uniqueEntityName(
                registry, preferredName));
        auto& transform =
            registry.addComponent<
                TransformComponent>(entity);
        transform.position = position;
        transform.isDirty = true;
        auto* relationships =
            registry.getPool<
                RelationshipComponent>();
        int nextOrder = 0;
        for (const RelationshipComponent&
                relationship :
            relationships->components) {
            nextOrder = std::max(
                nextOrder,
                relationship.siblingOrder + 1);
        }
        auto& relationship =
            registry.addComponent<
                RelationshipComponent>(entity);
        relationship.siblingOrder =
            nextOrder;
        return entity;
    }

    Entity createModelEditorEntity(
        Registry& registry,
        AssetGuid modelGuid,
        std::string_view preferredName,
        glm::vec3 position) {
        const Entity entity =
            createEmptyEditorEntity(
                registry, preferredName,
                position);
        auto& mesh =
            registry.addComponent<
                MeshComponent>(entity);
        mesh.assetGuid = modelGuid;
        mesh.requestedAssetGuid =
            modelGuid;
        return entity;
    }

    std::string_view editorEntityPresetName(
        EditorEntityPreset preset) noexcept {
        switch (preset) {
        case EditorEntityPreset::Empty: return "Entity";
        case EditorEntityPreset::Cube: return "Cube";
        case EditorEntityPreset::DirectionalLight: return "Sun";
        case EditorEntityPreset::PointLight: return "Point Light";
        case EditorEntityPreset::SpotLight: return "Spot Light";
        case EditorEntityPreset::HdriSky: return "HDRI Sky";
        }
        return "Entity";
    }

    Entity createEditorEntityPreset(
        Registry& registry,
        EditorEntityPreset preset,
        glm::vec3 position) {
        const Entity entity = createEmptyEditorEntity(
            registry, editorEntityPresetName(preset), position);
        switch (preset) {
        case EditorEntityPreset::Empty:
            break;
        case EditorEntityPreset::Cube: {
            auto& mesh = registry.addComponent<MeshComponent>(entity);
            mesh.assetGuid = kBuiltInCubeAssetGuid;
            mesh.requestedAssetGuid = kBuiltInCubeAssetGuid;
            break;
        }
        case EditorEntityPreset::DirectionalLight:
        case EditorEntityPreset::PointLight:
        case EditorEntityPreset::SpotLight: {
            auto& light = registry.addComponent<LightComponent>(entity);
            light.type = preset == EditorEntityPreset::DirectionalLight
                ? LightType::Directional
                : preset == EditorEntityPreset::PointLight
                    ? LightType::Point : LightType::Spot;
            if (light.type == LightType::Directional) {
                auto& transform = registry.getComponent<TransformComponent>(entity);
                transform.rotation = { 45.0f, -30.0f, 0.0f };
                transform.isDirty = true;
            }
            break;
        }
        case EditorEntityPreset::HdriSky: {
            auto& sky = registry.addComponent<SkyComponent>(entity);
            sky.mode = SkyMode::Hdri;
            sky.hdri.environmentAssetGuid =
                kDefaultEditorEnvironmentAssetGuid;
            sky.requestedEnvironmentAssetGuid =
                kDefaultEditorEnvironmentAssetGuid;
            break;
        }
        }
        return entity;
    }

} // namespace Iridium
