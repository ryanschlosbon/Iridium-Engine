#pragma once
#include <glm/glm.hpp>

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
    float innerConeAngle = 12.5f;
    float outerConeAngle = 45.0f;

    // Shadows
    bool castsShadows = true;

    // Editor UI helper (Optional, keeps UI clean)
    // bool showHelper = true; 
};