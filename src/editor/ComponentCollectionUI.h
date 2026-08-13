#pragma once

#include <algorithm>
#include <span>

#include <imgui.h>

namespace Iridium::EditorComponentUI {

    enum class CollectionLayout {
        Grid,
        List,
    };

    struct CollectionViewState {
        CollectionLayout layout =
            CollectionLayout::Grid;
        int zoomLevel = 1;
    };

    inline void drawCollectionControls(
        const char* id,
        CollectionViewState& state,
        std::span<const float> thumbnailExtents) {
        if (thumbnailExtents.empty()) {
            return;
        }
        state.zoomLevel = std::clamp(
            state.zoomLevel, 0,
            static_cast<int>(
                thumbnailExtents.size()) - 1);
        ImGui::PushID(id);
        ImGui::BeginDisabled(
            state.zoomLevel == 0);
        if (ImGui::Button("-")) {
            --state.zoomLevel;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Zoom %d",
            state.zoomLevel + 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(
            state.zoomLevel + 1 >=
                static_cast<int>(
                    thumbnailExtents.size()));
        if (ImGui::Button("+")) {
            ++state.zoomLevel;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool grid =
            state.layout ==
            CollectionLayout::Grid;
        if (ImGui::Button(
                grid ? "List" : "Grid")) {
            state.layout =
                grid
                ? CollectionLayout::List
                : CollectionLayout::Grid;
        }
        ImGui::PopID();
    }

    [[nodiscard]] inline float thumbnailExtent(
        const CollectionViewState& state,
        std::span<const float> thumbnailExtents) {
        if (thumbnailExtents.empty()) {
            return 0.0f;
        }
        return thumbnailExtents[
            static_cast<size_t>(std::clamp(
                state.zoomLevel, 0,
                static_cast<int>(
                    thumbnailExtents.size()) -
                    1))];
    }

    inline bool beginResizableRegion(
        const char* id,
        float defaultHeight,
        ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_None) {
        return ImGui::BeginChild(
            id,
            ImVec2(0.0f, defaultHeight),
            ImGuiChildFlags_Borders |
                ImGuiChildFlags_ResizeY,
            windowFlags);
    }

    inline void endResizableRegion() {
        ImGui::EndChild();
    }

    inline bool beginComponentBody(
        const char* id,
        float defaultHeight) {
        return beginResizableRegion(
            id, defaultHeight);
    }

    inline void endComponentBody() {
        endResizableRegion();
    }

} // namespace Iridium::EditorComponentUI
