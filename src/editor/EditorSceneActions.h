#pragma once

#include "assets/AssetGuid.h"
#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

class Registry;

namespace Iridium {

    enum class EditorEntityPreset {
        Empty,
        Cube,
        DirectionalLight,
        PointLight,
        SpotLight,
        HdriSky,
    };

    inline constexpr AssetGuid kDefaultEditorEnvironmentAssetGuid{
        AssetGuid::Bytes{ 0x01, 0x9c, 0x5d, 0x3a, 0x12, 0x34, 0x7a, 0xbc,
            0x8d, 0xef, 0x10, 0x29, 0x38, 0x47, 0x56, 0xaa }
    };

    [[nodiscard]] std::string uniqueEntityName(
        Registry& registry,
        std::string_view preferred,
        Entity excludedEntity = NULL_ENTITY);
    [[nodiscard]] Entity createEmptyEditorEntity(
        Registry& registry,
        std::string_view preferredName = "Entity",
        glm::vec3 position = {});
    [[nodiscard]] Entity createModelEditorEntity(
        Registry& registry,
        AssetGuid modelGuid,
        std::string_view preferredName = "Model",
        glm::vec3 position = {});
    [[nodiscard]] std::string_view editorEntityPresetName(
        EditorEntityPreset preset) noexcept;
    [[nodiscard]] Entity createEditorEntityPreset(
        Registry& registry,
        EditorEntityPreset preset,
        glm::vec3 position = {});
} // namespace Iridium
