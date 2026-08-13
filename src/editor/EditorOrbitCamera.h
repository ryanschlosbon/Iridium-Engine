#pragma once

#include <glm/glm.hpp>

namespace Iridium {

    struct EditorOrbitCameraState {
        glm::vec3 target{ 0.0f };
        float yawDegrees = -45.0f;
        float pitchDegrees = 20.0f;
        float distance = 3.0f;
        float verticalFovDegrees = 45.0f;
        float nearPlane = 0.01f;
        float farPlane = 1000.0f;

    };

    class EditorOrbitCamera {
    public:
        [[nodiscard]] const EditorOrbitCameraState& state() const noexcept {
            return state_;
        }
        void setState(EditorOrbitCameraState state);

        void orbit(float deltaX, float deltaY) noexcept;
        void pan(float deltaX, float deltaY,
            float viewportHeight) noexcept;
        void dolly(float wheelDelta) noexcept;
        void frameBounds(const glm::vec3& minimum,
            const glm::vec3& maximum, float aspect) noexcept;

        [[nodiscard]] glm::vec3 position() const noexcept;
        [[nodiscard]] glm::mat4 viewMatrix() const noexcept;
        [[nodiscard]] glm::mat4 projectionMatrix(float aspect) const noexcept;

    private:
        [[nodiscard]] glm::vec3 forward() const noexcept;
        EditorOrbitCameraState state_;
    };

} // namespace Iridium
