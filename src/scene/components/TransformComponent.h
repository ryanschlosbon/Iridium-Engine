#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../../editor/Reflection.h"

struct TransformComponent {
    // Raw Data
    glm::vec3 position = { 0.0f, 0.0f, 0.0f };
    glm::vec3 rotation = { 0.0f, 0.0f, 0.0f };
    glm::vec3 scale = { 1.0f, 1.0f, 1.0f };

    // Matrices (The new part)
    glm::mat4 worldMatrix = glm::mat4(1.0f);
    glm::mat4 localMatrix = glm::mat4(1.0f);
    bool isDirty = true;

    // Helper: Update Local Matrix
// Helper to build the local matrix from pos/rot/scale
    void updateLocalMatrix() {
        glm::mat4 mat = glm::mat4(1.0f);
        mat = glm::translate(mat, position);

        mat = glm::rotate(mat, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        mat = glm::rotate(mat, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        mat = glm::rotate(mat, glm::radians(rotation.x), glm::vec3(1, 0, 0));

        mat = glm::scale(mat, scale);
        localMatrix = mat;
        isDirty = true;
    }

    // Setters
    void setPosition(const glm::vec3& newPos) { position = newPos; isDirty = true; }
    void setRotation(const glm::vec3& newRot) { rotation = newRot; isDirty = true; }
    void setScale(const glm::vec3& newScale) { scale = newScale; isDirty = true; }

    void OnInspector() {
        bool changed = false;

        // If any of these get edited, changed becomes true
        changed |= PROPERTY(position);
        changed |= PROPERTY(rotation);
        changed |= PROPERTY(scale);

        if (changed) {
            isDirty = true; // Now the TransformSystem will catch it next frame!
        }
    }
};