#pragma once
#include <cstdint>

#include <glm/glm.hpp>

enum class LightType {
    Directional = 0, // Sun
    Point = 1,       // Light bulb
    Spot = 2,        // Flashlight / Street lamp
    Area = 3         // Readable legacy value; strict M5 cook rejects it.
};

enum class LightShadowQuality {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
};

struct LightComponent {
    LightType type = LightType::Directional;

    // Canonical persisted chromaticity. The editor presents an sRGB picker but
    // converts to/from this nonnegative linear Rec.709/D65 value.
    glm::vec3 colorLinearRec709{ 1.0f };

    // Physical intensity fields remain separate so changing the light type never
    // silently changes units. Only the field appropriate to type is shaded.
    // Practical creation defaults: clear-day sun and a useful local fixture.
    // Persisted values remain physical lux/candela and existing scenes retain
    // their explicitly authored values.
    float illuminanceLux = 100'000.0f;
    float luminousIntensityCandela = 10'000.0f;

    float rangeMeters = 10.0f;
    float sourceRadiusMeters = 0.05f;
    float innerConeDegrees = 12.5f;
    float outerConeDegrees = 45.0f;
    bool castsShadows = true;
    LightShadowQuality shadowQuality = LightShadowQuality::High;
    int32_t priority = 0;
};
