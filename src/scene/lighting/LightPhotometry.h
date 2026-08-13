#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/glm.hpp>

namespace Iridium {

    inline constexpr float kPhotometricToSceneScale = 1.0e-4f;

    [[nodiscard]] inline float pointLumensFromCandela(float candela) noexcept {
        return std::isfinite(candela) && candela >= 0.0f
            ? candela * 4.0f * std::numbers::pi_v<float> : 0.0f;
    }

    [[nodiscard]] inline float pointCandelaFromLumens(float lumens) noexcept {
        return std::isfinite(lumens) && lumens >= 0.0f
            ? lumens / (4.0f * std::numbers::pi_v<float>) : 0.0f;
    }

    [[nodiscard]] inline float spotEffectiveSolidAngle(
        float innerConeDegrees, float outerConeDegrees) noexcept {
        if (!std::isfinite(innerConeDegrees) ||
            !std::isfinite(outerConeDegrees) || innerConeDegrees < 0.0f ||
            outerConeDegrees < innerConeDegrees || outerConeDegrees > 90.0f) {
            return 0.0f;
        }
        const float inner = glm::radians(innerConeDegrees);
        const float outer = glm::radians(outerConeDegrees);
        return 2.0f * std::numbers::pi_v<float> *
            (1.0f - 0.5f * (std::cos(inner) + std::cos(outer)));
    }

    [[nodiscard]] inline float spotLumensFromCandela(float candela,
        float innerConeDegrees, float outerConeDegrees) noexcept {
        if (!std::isfinite(candela) || candela < 0.0f) return 0.0f;
        return candela * spotEffectiveSolidAngle(
            innerConeDegrees, outerConeDegrees);
    }

    [[nodiscard]] inline float spotCandelaFromLumens(float lumens,
        float innerConeDegrees, float outerConeDegrees) noexcept {
        if (!std::isfinite(lumens) || lumens < 0.0f) return 0.0f;
        const float solidAngle = spotEffectiveSolidAngle(
            innerConeDegrees, outerConeDegrees);
        return solidAngle > 0.0f ? lumens / solidAngle : 0.0f;
    }

    [[nodiscard]] inline float linearToSrgb(float value) noexcept {
        const float safe = std::max(value, 0.0f);
        return safe <= 0.0031308f
            ? safe * 12.92f
            : 1.055f * std::pow(safe, 1.0f / 2.4f) - 0.055f;
    }

    [[nodiscard]] inline float srgbToLinear(float value) noexcept {
        const float safe = std::max(value, 0.0f);
        return safe <= 0.04045f
            ? safe / 12.92f
            : std::pow((safe + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] inline glm::vec3 linearRec709ToSrgb(glm::vec3 value) noexcept {
        return { linearToSrgb(value.x), linearToSrgb(value.y),
            linearToSrgb(value.z) };
    }

    [[nodiscard]] inline glm::vec3 srgbToLinearRec709(glm::vec3 value) noexcept {
        return { srgbToLinear(value.x), srgbToLinear(value.y),
            srgbToLinear(value.z) };
    }

    [[nodiscard]] inline glm::vec3 normalizedAp1LightChromaticity(
        glm::vec3 linearRec709) noexcept {
        if (!std::isfinite(linearRec709.x) || !std::isfinite(linearRec709.y) ||
            !std::isfinite(linearRec709.z) ||
            glm::any(glm::lessThan(linearRec709, glm::vec3(0.0f)))) return {};
        const glm::mat3 rec709ToAp1(
            0.6130974024f, 0.0701937225f, 0.0206155929f,
            0.3395231462f, 0.9163538791f, 0.1095697729f,
            0.0473794514f, 0.0134523984f, 0.8698146342f);
        const glm::vec3 ap1 = rec709ToAp1 * linearRec709;
        return ap1.y > 0.0f && std::isfinite(ap1.y) ? ap1 / ap1.y : glm::vec3{};
    }

} // namespace Iridium
