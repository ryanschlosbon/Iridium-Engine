#pragma once
#include "imgui.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>

// --- MACROS ---
// Use these inside your OnInspector() function
#define PROPERTY(variable) Reflection::DrawField(#variable, variable)
#define PROPERTY_R(variable, min, max) Reflection::DrawField(#variable, variable, min, max)

namespace Reflection {
    // 1. Float
    static void DrawField(const char* name, float& val, float min = 0.0f, float max = 0.0f) {
        if (min == 0.0f && max == 0.0f) ImGui::DragFloat(name, &val, 0.1f);
        else ImGui::SliderFloat(name, &val, min, max);
    }

    // 2. Int
    static void DrawField(const char* name, int& val, int min = 0, int max = 0) {
        if (min == 0 && max == 0) ImGui::DragInt(name, &val);
        else ImGui::SliderInt(name, &val, min, max);
    }

    // 3. Bool
    static void DrawField(const char* name, bool& val) {
        ImGui::Checkbox(name, &val);
    }

    // 4. Vector3
    static void DrawField(const char* name, glm::vec3& val) {
        ImGui::DragFloat3(name, glm::value_ptr(val), 0.1f);
    }

    // 5. Color Helper
    static void DrawColor(const char* name, glm::vec3& val) {
        ImGui::ColorEdit3(name, glm::value_ptr(val));
    }
}