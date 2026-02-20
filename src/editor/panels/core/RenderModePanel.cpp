#include "RenderModePanel.h"
#include <imgui.h>

RenderModePanel::RenderModePanel(int* renderModePtr)
    : currentRenderMode(renderModePtr) {
}

void RenderModePanel::OnImGuiRender(Registry& registry, AssetManager* assetManager) {
    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    ImVec2 windowPos;
    windowPos.x = workPos.x + workSize.x - PAD;
    windowPos.y = workPos.y + PAD;

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("RenderModeOverlay", nullptr, toolbarFlags)) {
        ImGui::Text("View Mode");
        ImGui::SameLine();
        const char* items[] = { "Standard", "Wireframe", "Outline Only" };
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##renderMode", currentRenderMode, items, IM_ARRAYSIZE(items));
    }
    ImGui::End();
}