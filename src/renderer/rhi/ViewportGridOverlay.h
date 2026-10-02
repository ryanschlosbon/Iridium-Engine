#pragma once

#include <glm/glm.hpp>

namespace Iridium {

    // Backend-neutral description of the editor's procedural world grid.
    // The renderer owns depth testing and rasterization; the editor owns the
    // active construction plane and artist-facing spacing controls.
    struct ViewportGridOverlay {
        glm::mat4 inverseViewProjection{ 1.0f };
        glm::vec3 origin{ 0.0f };
        glm::vec3 axisU{ 1.0f, 0.0f, 0.0f };
        glm::vec3 axisV{ 0.0f, 0.0f, 1.0f };
        float baseSpacingMeters = 1.0f;
        float opacity = 1.0f;
        bool visible = false;
    };

} // namespace Iridium
