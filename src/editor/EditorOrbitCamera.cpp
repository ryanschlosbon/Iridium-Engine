#include "editor/EditorOrbitCamera.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

namespace Iridium {

    namespace {
        constexpr float MinimumDistance = 0.01f;
        constexpr float MaximumDistance = 1.0e7f;
        constexpr float OrbitDegreesPerPixel = 0.25f;
        constexpr float DollyScale = 0.15f;
    }

    void EditorOrbitCamera::setState(EditorOrbitCameraState state) {
        if (!std::isfinite(state.distance) || state.distance < MinimumDistance ||
            !std::isfinite(state.verticalFovDegrees) ||
            state.verticalFovDegrees <= 1.0f || state.verticalFovDegrees >= 179.0f ||
            !std::isfinite(state.nearPlane) || !std::isfinite(state.farPlane) ||
            state.nearPlane <= 0.0f || state.farPlane <= state.nearPlane) {
            throw std::invalid_argument("Invalid orbit camera state");
        }
        state.pitchDegrees = std::clamp(state.pitchDegrees, -89.0f, 89.0f);
        state.distance = std::clamp(state.distance,
            MinimumDistance, MaximumDistance);
        state_ = state;
    }

    void EditorOrbitCamera::orbit(float deltaX, float deltaY) noexcept {
        if (!std::isfinite(deltaX) || !std::isfinite(deltaY)) return;
        state_.yawDegrees += deltaX * OrbitDegreesPerPixel;
        state_.pitchDegrees = std::clamp(
            state_.pitchDegrees + deltaY * OrbitDegreesPerPixel,
            -89.0f, 89.0f);
        state_.yawDegrees = std::remainder(state_.yawDegrees, 360.0f);
    }

    void EditorOrbitCamera::pan(float deltaX, float deltaY,
        float viewportHeight) noexcept {
        if (!std::isfinite(deltaX) || !std::isfinite(deltaY) ||
            !std::isfinite(viewportHeight) || viewportHeight <= 0.0f) {
            return;
        }
        const glm::vec3 direction = forward();
        glm::vec3 right = glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f));
        const float rightLength = glm::length(right);
        if (rightLength <= 1.0e-6f) return;
        right /= rightLength;
        const glm::vec3 up = glm::normalize(glm::cross(right, direction));
        const float worldPerPixel =
            2.0f * state_.distance *
            std::tan(glm::radians(state_.verticalFovDegrees) * 0.5f) /
            viewportHeight;
        state_.target += (-right * deltaX + up * deltaY) * worldPerPixel;
    }

    void EditorOrbitCamera::dolly(float wheelDelta) noexcept {
        if (!std::isfinite(wheelDelta)) return;
        state_.distance = std::clamp(
            state_.distance * std::exp(-wheelDelta * DollyScale),
            MinimumDistance, MaximumDistance);
        state_.nearPlane = std::max(0.001f,
            std::min(state_.nearPlane, state_.distance * 0.25f));
        state_.farPlane = std::max(state_.nearPlane + 1.0f,
            std::max(state_.farPlane, state_.distance * 4.0f));
    }

    void EditorOrbitCamera::frameBounds(const glm::vec3& minimum,
        const glm::vec3& maximum, float aspect) noexcept {
        if (!std::isfinite(aspect) || aspect <= 0.0f ||
            glm::any(glm::isnan(minimum)) || glm::any(glm::isnan(maximum))) {
            return;
        }
        const glm::vec3 orderedMinimum = glm::min(minimum, maximum);
        const glm::vec3 orderedMaximum = glm::max(minimum, maximum);
        state_.target = (orderedMinimum + orderedMaximum) * 0.5f;
        const float radius = std::max(
            glm::length(orderedMaximum - orderedMinimum) * 0.5f,
            0.01f);
        const float verticalHalfFov =
            glm::radians(state_.verticalFovDegrees) * 0.5f;
        const float horizontalHalfFov =
            std::atan(std::tan(verticalHalfFov) * aspect);
        const float limitingHalfFov =
            std::max(0.01f, std::min(verticalHalfFov, horizontalHalfFov));
        state_.distance = std::clamp(
            radius / std::sin(limitingHalfFov) * 1.10f,
            MinimumDistance, MaximumDistance);
        state_.nearPlane = std::max(0.001f,
            state_.distance - radius * 1.5f);
        state_.farPlane = std::max(state_.nearPlane + 1.0f,
            state_.distance + radius * 4.0f);
    }

    glm::vec3 EditorOrbitCamera::position() const noexcept {
        return state_.target - forward() * state_.distance;
    }

    glm::mat4 EditorOrbitCamera::viewMatrix() const noexcept {
        return glm::lookAt(position(), state_.target,
            glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 EditorOrbitCamera::projectionMatrix(float aspect) const noexcept {
        if (!std::isfinite(aspect) || aspect <= 0.0f) aspect = 1.0f;
        glm::mat4 projection = glm::perspective(
            glm::radians(state_.verticalFovDegrees), aspect,
            state_.nearPlane, state_.farPlane);
        projection[1][1] *= -1.0f;
        return projection;
    }

    glm::vec3 EditorOrbitCamera::forward() const noexcept {
        const float yaw = glm::radians(state_.yawDegrees);
        const float pitch = glm::radians(state_.pitchDegrees);
        return glm::normalize(glm::vec3(
            std::cos(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::sin(yaw) * std::cos(pitch)));
    }

} // namespace Iridium
