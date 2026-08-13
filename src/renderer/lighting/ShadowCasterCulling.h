#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace Iridium {

    struct ShadowCasterSphere {
        glm::vec3 center{};
        float radius = -1.0f;
    };

    [[nodiscard]] inline ShadowCasterSphere transformShadowCasterSphere(
        glm::vec3 localCenter, float localRadius,
        const glm::mat4& worldTransform) noexcept {
        // Zero is also treated as unknown because legacy/runtime-authored
        // submeshes may not have populated bounds yet.
        if (!std::isfinite(localRadius) || localRadius <= 0.0f)
            return {};
        const glm::vec3 center = glm::vec3(
            worldTransform * glm::vec4(localCenter, 1.0f));
        const float maximumScale = (std::max)({
            glm::length(glm::vec3(worldTransform[0])),
            glm::length(glm::vec3(worldTransform[1])),
            glm::length(glm::vec3(worldTransform[2])),
        });
        const float radius = localRadius * maximumScale;
        if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
            !std::isfinite(center.z) || !std::isfinite(radius))
            return {};
        return { center, radius };
    }

    // Vulkan clip volume: -w <= x,y <= w and 0 <= z <= w. Unknown or
    // degenerate bounds are retained so culling can never remove valid casters.
    [[nodiscard]] inline bool shadowCasterSphereIntersectsClipVolume(
        const glm::mat4& worldToClip, glm::vec3 center,
        float radius) noexcept {
        if (!std::isfinite(radius) || radius < 0.0f)
            return true;
        const auto row = [&worldToClip](uint32_t index) {
            return glm::vec4(worldToClip[0][index], worldToClip[1][index],
                worldToClip[2][index], worldToClip[3][index]);
        };
        const glm::vec4 row0 = row(0);
        const glm::vec4 row1 = row(1);
        const glm::vec4 row2 = row(2);
        const glm::vec4 row3 = row(3);
        const std::array planes{
            row3 + row0, row3 - row0,
            row3 + row1, row3 - row1,
            row2, row3 - row2,
        };
        const glm::vec4 homogeneousCenter(center, 1.0f);
        for (const glm::vec4& plane : planes) {
            const float normalLength = glm::length(glm::vec3(plane));
            if (!std::isfinite(normalLength) || normalLength <= 1.0e-8f)
                return true;
            if (glm::dot(plane, homogeneousCenter) <
                -radius * normalLength)
                return false;
        }
        return true;
    }

} // namespace Iridium
