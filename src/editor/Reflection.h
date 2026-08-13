#pragma once
#include "imgui.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>

namespace Reflection {
    inline void ExplainTextEntry() {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Double-click or Ctrl-click to type an exact value.");
    }

    // 1. Float
    static bool DrawField(const char* name, float& val, float min = 0.0f, float max = 0.0f) {
        const bool changed = min == 0.0f && max == 0.0f
            ? ImGui::DragFloat(name, &val, 0.1f)
            : ImGui::DragFloat(name, &val, (max - min) / 500.0f, min, max,
                "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ExplainTextEntry();
        return changed;
    }

    // 2. Int
    static bool DrawField(const char* name, int& val, int min = 0, int max = 0) {
        const bool changed = min == 0 && max == 0
            ? ImGui::DragInt(name, &val)
            : ImGui::DragInt(name, &val, 1.0f, min, max, "%d",
                ImGuiSliderFlags_AlwaysClamp);
        ExplainTextEntry();
        return changed;
    }

    // 3. Bool
    static bool DrawField(const char* name, bool& val) {
        return ImGui::Checkbox(name, &val);
    }

    // 4. Vector3
    static bool DrawField(const char* name, glm::vec3& val) {
        const bool changed = ImGui::DragFloat3(name, glm::value_ptr(val), 0.1f);
        ExplainTextEntry();
        return changed;
    }

    // 5. Color Helper
    static bool DrawColor(const char* name, glm::vec3& val) {
        return ImGui::ColorEdit3(name, glm::value_ptr(val));
    }
}
