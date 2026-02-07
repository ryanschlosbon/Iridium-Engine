#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct TransformComponent {
    glm::vec3 translation{ 0.0f };
    glm::vec3 rotation{ 0.0f };
    glm::vec3 scale{ 1.0f };

    glm::mat4 mat4() const {
        glm::mat4 res = glm::translate(glm::mat4(1.0f), translation);
        res = glm::rotate(res, glm::radians(rotation.x), { 1, 0, 0 });
        res = glm::rotate(res, glm::radians(rotation.y), { 0, 1, 0 });
        res = glm::rotate(res, glm::radians(rotation.z), { 0, 0, 1 });
        return glm::scale(res, scale);
    }
};