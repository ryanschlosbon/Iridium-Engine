#pragma once

#include "assets/AssetGuid.h"
#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

class Registry;

namespace Iridium {

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
} // namespace Iridium
