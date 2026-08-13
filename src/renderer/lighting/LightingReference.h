#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Iridium::LightingReference {

    inline constexpr double MinimumDistanceSquared = 1.0e-8;
    inline constexpr double PhotometricToSceneScale = 1.0e-4;

    [[nodiscard]] inline double pointCandelaFromLumens(double lumens) noexcept {
        if (!std::isfinite(lumens) || lumens < 0.0) return 0.0;
        return lumens / (4.0 * std::numbers::pi);
    }

    // The angles are cone half-angles in radians. This is the exact integral of
    // the M5 cosine-domain smoothstep profile between the inner and outer cones.
    [[nodiscard]] inline double spotEffectiveSolidAngle(double innerAngle,
        double outerAngle) noexcept {
        if (!std::isfinite(innerAngle) || !std::isfinite(outerAngle) ||
            innerAngle < 0.0 || innerAngle > outerAngle ||
            outerAngle > std::numbers::pi) {
            return 0.0;
        }
        return 2.0 * std::numbers::pi *
            (1.0 - 0.5 * (std::cos(innerAngle) + std::cos(outerAngle)));
    }

    [[nodiscard]] inline double spotCandelaFromLumens(double lumens,
        double innerAngle, double outerAngle) noexcept {
        if (!std::isfinite(lumens) || lumens < 0.0) return 0.0;
        const double solidAngle = spotEffectiveSolidAngle(innerAngle, outerAngle);
        return solidAngle > 0.0 ? lumens / solidAngle : 0.0;
    }

    [[nodiscard]] inline double smoothRangeWindow(double distance,
        double range) noexcept {
        if (!std::isfinite(distance) || !std::isfinite(range) ||
            distance < 0.0 || range <= 0.0 || distance >= range) {
            return 0.0;
        }
        const double ratio = distance / range;
        const double ratioSquared = ratio * ratio;
        const double base = std::clamp(
            1.0 - ratioSquared * ratioSquared, 0.0, 1.0);
        return base * base;
    }

    [[nodiscard]] inline double inverseSquareRangeAttenuation(double distance,
        double range, double sourceRadius) noexcept {
        if (!std::isfinite(distance) || !std::isfinite(range) ||
            !std::isfinite(sourceRadius) || distance < 0.0 || range <= 0.0 ||
            sourceRadius < 0.0) {
            return 0.0;
        }
        const double distanceSquared = distance * distance;
        const double radiusSquared = sourceRadius * sourceRadius;
        return smoothRangeWindow(distance, range) /
            std::max({ distanceSquared, radiusSquared, MinimumDistanceSquared });
    }

    [[nodiscard]] inline double spotConeAttenuation(double cosineTheta,
        double innerCosine, double outerCosine) noexcept {
        if (!std::isfinite(cosineTheta) || !std::isfinite(innerCosine) ||
            !std::isfinite(outerCosine) || innerCosine < outerCosine ||
            innerCosine < -1.0 || innerCosine > 1.0 ||
            outerCosine < -1.0 || outerCosine > 1.0) {
            return 0.0;
        }
        if (innerCosine == outerCosine) {
            return cosineTheta >= innerCosine ? 1.0 : 0.0;
        }
        const double t = std::clamp(
            (cosineTheta - outerCosine) / (innerCosine - outerCosine),
            0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    [[nodiscard]] inline double directionalRadiance(double chromaticity,
        double illuminanceLux) noexcept {
        if (!std::isfinite(chromaticity) || !std::isfinite(illuminanceLux) ||
            chromaticity < 0.0 || illuminanceLux < 0.0) {
            return 0.0;
        }
        return chromaticity * illuminanceLux * PhotometricToSceneScale;
    }

    [[nodiscard]] inline double localRadiance(double chromaticity,
        double luminousIntensityCandela, double distance, double range,
        double sourceRadius, double coneAttenuation = 1.0) noexcept {
        if (!std::isfinite(chromaticity) ||
            !std::isfinite(luminousIntensityCandela) ||
            !std::isfinite(coneAttenuation) || chromaticity < 0.0 ||
            luminousIntensityCandela < 0.0 || coneAttenuation < 0.0) {
            return 0.0;
        }
        return chromaticity * luminousIntensityCandela *
            PhotometricToSceneScale *
            inverseSquareRangeAttenuation(distance, range, sourceRadius) *
            coneAttenuation;
    }

    [[nodiscard]] inline double lambertianDiffuse(double reflectance) noexcept {
        if (!std::isfinite(reflectance) || reflectance < 0.0) return 0.0;
        return reflectance / std::numbers::pi;
    }

    // Frozen split-sum composition used by the future cooked BRDF LUT. The LUT
    // stores scale and bias so non-unit F90 remains representable.
    [[nodiscard]] inline double iblSplitSum(double f0, double f90,
        double scale, double bias) noexcept {
        if (!std::isfinite(f0) || !std::isfinite(f90) ||
            !std::isfinite(scale) || !std::isfinite(bias)) {
            return 0.0;
        }
        return f0 * scale + f90 * bias;
    }

} // namespace Iridium::LightingReference
