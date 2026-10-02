#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Iridium {

    enum class EditorLengthUnit : uint8_t {
        Millimetres,
        Centimetres,
        Metres,
        Kilometres,
    };

    enum class EditorTransformSpace : uint8_t {
        World,
        Local,
    };

    [[nodiscard]] inline constexpr float metresPerUnit(
        EditorLengthUnit unit) noexcept {
        switch (unit) {
        case EditorLengthUnit::Millimetres: return 0.001f;
        case EditorLengthUnit::Centimetres: return 0.01f;
        case EditorLengthUnit::Kilometres: return 1000.0f;
        case EditorLengthUnit::Metres: return 1.0f;
        }
        return 1.0f;
    }

    [[nodiscard]] inline constexpr std::string_view lengthUnitSymbol(
        EditorLengthUnit unit) noexcept {
        switch (unit) {
        case EditorLengthUnit::Millimetres: return "mm";
        case EditorLengthUnit::Centimetres: return "cm";
        case EditorLengthUnit::Kilometres: return "km";
        case EditorLengthUnit::Metres: return "m";
        }
        return "m";
    }

    [[nodiscard]] inline float adaptiveGridSpacingMeters(
        float cameraDistanceMeters, float baseSpacingMeters) noexcept {
        if (!std::isfinite(baseSpacingMeters) || baseSpacingMeters <= 0.0f) {
            baseSpacingMeters = 1.0f;
        }
        if (!std::isfinite(cameraDistanceMeters) ||
            cameraDistanceMeters <= 0.0f) {
            return baseSpacingMeters;
        }
        const float ratio = cameraDistanceMeters /
            (baseSpacingMeters * 8.0f);
        const float decade = std::pow(10.0f,
            std::floor(std::log10((std::max)(ratio, 1.0e-6f))));
        return std::clamp(baseSpacingMeters * decade,
            baseSpacingMeters * 0.001f,
            baseSpacingMeters * 100000.0f);
    }

    [[nodiscard]] inline constexpr bool transformSnapActive(
        bool automaticSnapEnabled, bool shiftHeld,
        bool controlHeld) noexcept {
        return !controlHeld && (automaticSnapEnabled || shiftHeld);
    }

    struct EditorTransformSettings {
        bool gridVisible = true;
        bool gridFollowsTranslation = true;
        EditorLengthUnit worldUnit = EditorLengthUnit::Metres;
        float gridStepInWorldUnits = 1.0f;
        bool translationSnapEnabled = false;
        bool translationSnapUsesGridStep = true;
        float translationSnapInWorldUnits = 1.0f;
        bool rotationSnapEnabled = false;
        float rotationSnapDegrees = 15.0f;
        bool scaleSnapEnabled = false;
        float scaleSnapIncrement = 0.1f;
        EditorTransformSpace transformSpace = EditorTransformSpace::World;

        [[nodiscard]] float gridStepMeters() const noexcept {
            return (std::max)(gridStepInWorldUnits, 1.0e-6f) *
                metresPerUnit(worldUnit);
        }

        [[nodiscard]] float translationSnapMeters() const noexcept {
            return (std::max)(translationSnapInWorldUnits, 1.0e-6f) *
                metresPerUnit(worldUnit);
        }

        [[nodiscard]] float effectiveTranslationSnapMeters() const noexcept {
            return translationSnapUsesGridStep
                ? gridStepMeters() : translationSnapMeters();
        }
    };

} // namespace Iridium
