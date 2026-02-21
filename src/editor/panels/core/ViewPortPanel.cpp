#include "ViewportPanel.h"
#include "../../scene/Components.h"
#include <glm/gtc/type_ptr.hpp> // For glm::value_ptr

void ViewportPanel::render(VkDescriptorSet sceneTexture,
    int& currentRenderMode,
    ImGuizmo::OPERATION& currentGizmoOperation,
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix,
    TransformComponent* selectedTransform) {

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });
    ImGui::Begin("Scene Viewport");
    
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::SetWindowFocus();
    }

    isFocused = ImGui::IsWindowFocused(); // This returns true if the window or any of its children are focused

    // ========================================================
    // 1. THE INTEGRATED TOOLBAR
    // ========================================================
    ImGui::SetCursorPos(ImVec2(10, 30)); // Float it slightly down so it doesn't overlap the tab bar

    // Move Button
    bool isMoveActive = (currentGizmoOperation == ImGuizmo::TRANSLATE);
    if (isMoveActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
    if (ImGui::Button("Move")) currentGizmoOperation = ImGuizmo::TRANSLATE;
    if (isMoveActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Rotate Button
    bool isRotateActive = (currentGizmoOperation == ImGuizmo::ROTATE);
    if (isRotateActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
    if (ImGui::Button("Rotate")) currentGizmoOperation = ImGuizmo::ROTATE;
    if (isRotateActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Scale Button
    bool isScaleActive = (currentGizmoOperation == ImGuizmo::SCALE);
    if (isScaleActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.66f, 0.7f, 0.7f));
    if (ImGui::Button("Scale")) currentGizmoOperation = ImGuizmo::SCALE;
    if (isScaleActive) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("  |  View Mode:");
    ImGui::SameLine();

    // Render Mode Combo Box
    const char* items[] = { "Standard", "Wireframe", "Outline Only" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##renderMode", &currentRenderMode, items, IM_ARRAYSIZE(items));

    ImGui::Dummy(ImVec2(0.0f, 5.0f)); // Add a little breathing room below the toolbar


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

    // Prevent crashing if window is too small
    if (viewportWidth > 0.0f && viewportHeight > 0.0f) {
        ImGui::Image((ImTextureID)sceneTexture, availSize);
    }

// ========================================================
    // 3. DRAW GIZMOS (Must happen AFTER the image is drawn!)
    // ========================================================
    if (selectedTransform) {
        ImGuizmo::SetDrawlist();

        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        ImGuizmo::SetRect(imgMin.x, imgMin.y, imgMax.x - imgMin.x, imgMax.y - imgMin.y);

        // USE YOUR COMPONENT'S ACTUAL MATRIX
        glm::mat4 transformMatrix = selectedTransform->worldMatrix;

        if (ImGuizmo::Manipulate(glm::value_ptr(viewMatrix),
            glm::value_ptr(projectionMatrix),
            currentGizmoOperation,
            ImGuizmo::LOCAL,
            glm::value_ptr(transformMatrix))) {

            float translation[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(transformMatrix), translation, rotation, scale);

            // MAP TO YOUR COMPONENT'S EXACT VARIABLE NAMES
            selectedTransform->position = glm::make_vec3(translation);
            selectedTransform->rotation = glm::make_vec3(rotation);
            selectedTransform->scale = glm::make_vec3(scale);

            // Flag it as dirty so your TransformSystem recalculates the matrix next frame!
            selectedTransform->isDirty = true;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}