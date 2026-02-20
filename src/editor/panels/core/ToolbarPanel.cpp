#include "ToolbarPanel.h"
#include <imgui.h>

ToolbarPanel::ToolbarPanel(int* toolModePtr)
    : currentToolMode(toolModePtr) {
}

void ToolbarPanel::OnImGuiRender(Registry& registry, AssetManager* assetManager) {
    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    ImVec2 toolbarPos;
    toolbarPos.x = workPos.x + workSize.x - 220.0f;
    toolbarPos.y = workPos.y + PAD;

    ImGui::SetNextWindowPos(toolbarPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);

    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("Toolbar", nullptr, toolbarFlags)) {
        // Mode 0: Select 
        bool isSelectActive = (*currentToolMode == 0);
        if (isSelectActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.6f));
        if (ImGui::Button("Select")) *currentToolMode = 0;
        if (isSelectActive) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select Only (No Gizmo)");

        ImGui::SameLine();

        // Mode 1: Translate
        bool isMoveActive = (*currentToolMode == 1);
        if (isMoveActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
        if (ImGui::Button("Move")) *currentToolMode = 1;
        if (isMoveActive) ImGui::PopStyleColor();

        ImGui::SameLine();

        // Mode 2: Rotate
        bool isRotateActive = (*currentToolMode == 2);
        if (isRotateActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
        if (ImGui::Button("Rotate")) *currentToolMode = 2;
        if (isRotateActive) ImGui::PopStyleColor();

        ImGui::SameLine();

        // Mode 3: Scale
        bool isScaleActive = (*currentToolMode == 3);
        if (isScaleActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.66f, 0.7f, 0.7f));
        if (ImGui::Button("Scale")) *currentToolMode = 3;
        if (isScaleActive) ImGui::PopStyleColor();
    }
    ImGui::End();
}