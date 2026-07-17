#include "ViewportPanel.h"
#include "imgui.h"
#include "vendor/imguizmo/ImGuizmo.h"

// Bring in the RHI and Component definitions to fix "incomplete type" errors
#include "renderer/rhi/Mesh.h" 
#include "scene/components/TransformComponent.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

void ViewportPanel::render(void* sceneTextureID, void* glassDepthTextureID,
    int& currentRenderMode, ImGuizmo::OPERATION& currentGizmoOperation,
    const glm::mat4& view, const glm::mat4& proj,
    TransformComponent* selectedTransform) {

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });

    // Using "Scene Viewport" as the consistent window name
    ImGui::Begin("Scene Viewport");

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::SetWindowFocus();
    }

    isFocused = ImGui::IsWindowFocused();

    // ========================================================
    // 1. THE INTEGRATED TOOLBAR
    // ========================================================
    ImGui::SetCursorPos(ImVec2(10, 30));

    // Move Button logic
    bool isMoveActive = (currentGizmoOperation == ImGuizmo::TRANSLATE);
    if (isMoveActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
    if (ImGui::Button("Move")) currentGizmoOperation = ImGuizmo::TRANSLATE;
    if (isMoveActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Rotate Button logic
    bool isRotateActive = (currentGizmoOperation == ImGuizmo::ROTATE);
    if (isRotateActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
    if (ImGui::Button("Rotate")) currentGizmoOperation = ImGuizmo::ROTATE;
    if (isRotateActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Scale Button logic
    bool isScaleActive = (currentGizmoOperation == ImGuizmo::SCALE);
    if (isScaleActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.66f, 0.7f, 0.7f));
    if (ImGui::Button("Scale")) currentGizmoOperation = ImGuizmo::SCALE;
    if (isScaleActive) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("  |  View Mode:");
    ImGui::SameLine();

    // Render Mode Selection
    const char* items[] = { "Standard", "Wireframe", "Glass Depth" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##renderMode", &currentRenderMode, items, IM_ARRAYSIZE(items));

    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    // ========================================================
    // 2. MOUSE MATH & IMAGE DRAWING
    // ========================================================
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    ImVec2 availSize = ImGui::GetContentRegionAvail();

    viewportWidth = availSize.x;
    viewportHeight = availSize.y;

    ImVec2 absoluteMousePos = ImGui::GetMousePos();
    mouseX = absoluteMousePos.x - screenPos.x;
    mouseY = absoluteMousePos.y - screenPos.y;
    isHovered = ImGui::IsWindowHovered();

    // Fix: Using the void* handles passed from the backend
    void* textureToDraw = sceneTextureID;
    if (currentRenderMode == 2) { // 2 matches "Glass Depth"
        textureToDraw = glassDepthTextureID;
    }


    // Drawing the viewport image using the API-agnostic handle
    if (viewportWidth > 0.0f && viewportHeight > 0.0f) {
        ImGui::Image((ImTextureID)textureToDraw, availSize);
    }

    // ========================================================
    // 3. DRAW GIZMOS
    // ========================================================
    if (selectedTransform) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();

        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        ImGuizmo::SetRect(imgMin.x, imgMin.y, imgMax.x - imgMin.x, imgMax.y - imgMin.y);

        // Fix: Use the actual worldMatrix from your component
        glm::mat4 transformMatrix = selectedTransform->worldMatrix;

        // Vulkan flip for ImGuizmo compatibility
        glm::mat4 correctedProj = proj;
        correctedProj[1][1] *= -1;

        if (ImGuizmo::Manipulate(glm::value_ptr(view),
            glm::value_ptr(correctedProj),
            currentGizmoOperation,
            ImGuizmo::LOCAL,
            glm::value_ptr(transformMatrix))) {

            float translation[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(transformMatrix), translation, rotation, scale);

            // ImGuizmo decomposes Euler rotation in degrees, which is also the
            // engine/editor-facing unit used by TransformComponent.
            selectedTransform->position = glm::vec3(translation[0], translation[1], translation[2]);
            selectedTransform->rotation = glm::vec3(rotation[0], rotation[1], rotation[2]);
            selectedTransform->scale = glm::vec3(scale[0], scale[1], scale[2]);

            // Flag for the TransformSystem to update the world matrix next frame
            selectedTransform->isDirty = true;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
