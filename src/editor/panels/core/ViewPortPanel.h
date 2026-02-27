#pragma once
#include <vulkan/vulkan.h>
#include <imgui.h>
#include "vendor/imguizmo/ImGuizmo.h"
#include <glm/glm.hpp>

struct TransformComponent;

class ViewportPanel {
public:
    // We pass in the texture and render mode so the panel can draw them
    void render(VkDescriptorSet sceneTexture,
        VkDescriptorSet glassDepthTexture,
        int& currentRenderMode,
        ImGuizmo::OPERATION& currentGizmoOperation,
        const glm::mat4& viewMatrix,
        const glm::mat4& projectionMatrix,
        TransformComponent* selectedEntityTransform = nullptr);

    // Application.cpp will read these to figure out what the mouse is doing!
    bool isHovered = false;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float screenPosX = 0.0f;
    float screenPosY = 0.0f;
    bool isFocused = false; 
};