#pragma once
#include <glm/glm.hpp>
#include "../../editor/Reflection.h"

enum class LightType {
    Directional = 0, // Sun
    Point = 1,       // Light bulb
    Spot = 2,        // Flashlight / Street lamp
    Area = 3         // Rectangular light (Soft shadows - requires RT usually)
};

struct LightComponent {
    LightType type = LightType::Directional;

    // Core Properties
    glm::vec3 color = { 1.0f, 1.0f, 1.0f }; // RGB
    float intensity = 1.0f;                 // Multiplier (Lumens/Lux)

    // Range / Falloff (For Point/Spot)
    float range = 10.0f;       // How far the light reaches
    float radius = 0.5f;       // Source radius (for soft shadows/Area lights)

    // Spot Light Specifics
    float innerCone = 12.5f;
    float outerCone = 45.0f;

    // Shadows
    bool castsShadows = true;

    void OnInspector() {
        // Manual dropdown logic
        int t = (int)type;
        if (ImGui::Combo("Type", &t, "Directional\0Point\0Spot\0Area\0")) {
            type = (LightType)t;
        }

        Reflection::DrawColor("Color", color);
        PROPERTY_R(intensity, 0.0f, 100.0f);
        PROPERTY(castsShadows);

        if (type != LightType::Directional) {
            PROPERTY_R(range, 0.0f, 1000.0f);
            PROPERTY_R(radius, 0.0f, 50.0f);
        }

        if (type == LightType::Spot) {
            ImGui::Text("Spot Angles");
            PROPERTY_R(innerCone, 0.0f, 90.0f);
            PROPERTY_R(outerCone, 0.0f, 90.0f);
        }
    }
};