#include "editor/ViewportPlacement.h"

#include <cmath>

namespace Iridium {

    std::optional<glm::vec3>
        viewportDropWorldPosition(
            float mouseX,
            float mouseY,
            float viewportWidth,
            float viewportHeight,
            const glm::mat4& view,
            const glm::mat4& projection,
            float groundPlaneY) noexcept {
        if (!std::isfinite(mouseX) ||
            !std::isfinite(mouseY) ||
            !std::isfinite(viewportWidth) ||
            !std::isfinite(viewportHeight) ||
            viewportWidth <= 0.0f ||
            viewportHeight <= 0.0f) {
            return std::nullopt;
        }
        const float ndcX =
            mouseX / viewportWidth * 2.0f -
            1.0f;
        const float ndcY =
            1.0f -
            mouseY / viewportHeight * 2.0f;
        const glm::mat4 inverse =
            glm::inverse(projection * view);
        glm::vec4 nearPoint =
            inverse * glm::vec4(
                ndcX, ndcY, 0.0f, 1.0f);
        glm::vec4 farPoint =
            inverse * glm::vec4(
                ndcX, ndcY, 1.0f, 1.0f);
        if (std::abs(nearPoint.w) <=
                1.0e-8f ||
            std::abs(farPoint.w) <=
                1.0e-8f) {
            return std::nullopt;
        }
        const glm::vec3 origin =
            glm::vec3(nearPoint) /
            nearPoint.w;
        const glm::vec3 far =
            glm::vec3(farPoint) /
            farPoint.w;
        const glm::vec3 direction =
            glm::normalize(far - origin);
        if (!std::isfinite(direction.x) ||
            !std::isfinite(direction.y) ||
            !std::isfinite(direction.z)) {
            return std::nullopt;
        }
        if (std::abs(direction.y) >
            1.0e-5f) {
            const float distance =
                (groundPlaneY - origin.y) /
                direction.y;
            if (std::isfinite(distance) &&
                distance >= 0.0f) {
                return origin +
                    direction * distance;
            }
        }
        return origin + direction * 5.0f;
    }

} // namespace Iridium
