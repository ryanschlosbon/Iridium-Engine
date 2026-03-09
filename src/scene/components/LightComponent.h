#pragma once
#include <glm/glm.hpp>
#include "editor/Reflection.h"

enum class LightType { Directional = 0, Point = 1, Spot = 2 };

struct LightComponent {
    glm::vec3 color = { 1.0f, 1.0f, 1.0f }; // The ImGuiArchive will automatically make this a Color Picker!
    float intensity = 1.0f;               // This will be a standard float slider
    int type = 0;                         // This will be an int slider

    // Expose the variables to the Engine
    REFLECT_BEGIN()
        PROPERTY(color)
        PROPERTY(intensity)
        PROPERTY(type)
        REFLECT_END()
};

// Magic: Tell the engine this exists!
AUTO_REGISTER_COMPONENT(LightComponent)