#pragma once

#include <glm/glm.hpp>

#include <cmath>

namespace Iridium {

    [[nodiscard]] inline glm::vec2 evaluateMaterialTextureUv(glm::vec2 uv,
        glm::vec2 offset, glm::vec2 scale, float rotationRadians) noexcept {
        const glm::vec2 scaled = uv * scale;
        const float cosine = std::cos(rotationRadians);
        const float sine = std::sin(rotationRadians);
        return offset + glm::vec2(
            cosine * scaled.x - sine * scaled.y,
            sine * scaled.x + cosine * scaled.y);
    }

} // namespace Iridium
