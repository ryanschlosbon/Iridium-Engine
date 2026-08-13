#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace Iridium {

    [[nodiscard]] std::optional<glm::vec3>
        viewportDropWorldPosition(
            float mouseX,
            float mouseY,
            float viewportWidth,
            float viewportHeight,
            const glm::mat4& view,
            const glm::mat4& projection,
            float groundPlaneY = 0.0f) noexcept;

} // namespace Iridium
