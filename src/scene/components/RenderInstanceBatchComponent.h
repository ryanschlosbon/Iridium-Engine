#pragma once

#include <vector>

#include <glm/glm.hpp>

// Runtime-only instance data for renderer-generated effects. This component is
// deliberately absent from scene serialization/editor authoring until the
// production particle and foliage contracts are defined. Each cached bound is
// the union of one model submesh transformed by every local transform.
struct RenderInstanceBatchComponent {
    struct SubMeshBounds {
        glm::vec3 minimum{};
        glm::vec3 maximum{};
        bool valid = false;
    };

    std::vector<glm::mat4> localTransforms;
    std::vector<SubMeshBounds> subMeshBounds;
};
