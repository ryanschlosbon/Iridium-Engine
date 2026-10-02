#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>

namespace Iridium {

    inline constexpr float TransparencyMinimumIor = 1.0e-4f;
    inline constexpr float TransparencyMaximumIor = 10.0f;
    inline constexpr float TransparencyMinimumAttenuationColor = 1.0e-6f;
    inline constexpr float TransparencyMinimumAttenuationDistance = 1.0e-6f;
    inline constexpr float TransparencyMinimumIncidenceCosine = 0.05f;
    inline constexpr float TransparencyMaximumConeTangent = 1.0f;
    inline constexpr float TransparencyMinimumEdgeBandPixels = 8.0f;

    struct DielectricFresnelResult {
        float reflectance = 1.0f;
        float transmittedCosine = 0.0f;
        bool totalInternalReflection = true;
        bool sanitized = false;
    };

    struct BeerLambertResult {
        glm::vec3 transmittance{ 1.0f };
        glm::vec3 extinction{ 0.0f };
        bool sanitized = false;
    };

    struct ThinSheetPathResult {
        float pathLengthMeters = 0.0f;
        float incidenceCosine = 1.0f;
        bool grazingClampApplied = false;
        bool sanitized = false;
    };

    struct ThinGlassLocalCompositionWeights {
        float effectiveTransmission = 0.0f;
        float interfaceOpacity = 1.0f;
        float destinationWeight = 0.0f;
        bool sanitized = false;
    };

    struct RefractionDirectionResult {
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
        bool totalInternalReflection = false;
        bool sanitized = false;
    };

    struct RefractionProjectionResult {
        glm::vec2 sourceUv{ 0.5f };
        glm::vec2 sampleUv{ 0.5f };
        float footprintPixels = 1.0f;
        float lod = 0.0f;
        float edgeConfidence = 0.0f;
        float expectedViewDepthMeters = 0.0f;
        bool onScreen = false;
        bool inFrontOfCamera = false;
        bool sanitized = false;
    };

    [[nodiscard]] inline uint32_t transparencyPyramidMipCount(
        uint32_t width, uint32_t height) noexcept {
        uint32_t maximum = (std::max)(width, height);
        uint32_t levels = 0;
        while (maximum != 0) {
            ++levels;
            maximum >>= 1u;
        }
        return levels;
    }

    [[nodiscard]] inline uint64_t transparencyPyramidTexelCount(
        uint32_t width, uint32_t height) noexcept {
        uint64_t texels = 0;
        while (width != 0 && height != 0) {
            texels += static_cast<uint64_t>(width) * height;
            width = (std::max)(width >> 1u, 1u);
            height = (std::max)(height >> 1u, 1u);
            if (width == 1u && height == 1u) {
                ++texels;
                break;
            }
        }
        return texels;
    }

    [[nodiscard]] inline RefractionDirectionResult refractTransparencyRay(
        glm::vec3 incidentDirection, glm::vec3 interfaceNormal,
        float incidentIor, float transmittedIor) noexcept {
        RefractionDirectionResult result{};
        const auto sanitizeIor = [&result](float value) {
            if (!std::isfinite(value) || value < TransparencyMinimumIor ||
                value > TransparencyMaximumIor) {
                result.sanitized = true;
                return std::clamp(std::isfinite(value) ? value : 1.0f,
                    TransparencyMinimumIor, TransparencyMaximumIor);
            }
            return value;
        };
        incidentIor = sanitizeIor(incidentIor);
        transmittedIor = sanitizeIor(transmittedIor);
        if (!std::isfinite(incidentDirection.x) ||
            !std::isfinite(incidentDirection.y) ||
            !std::isfinite(incidentDirection.z) ||
            glm::dot(incidentDirection, incidentDirection) <= 1.0e-12f) {
            incidentDirection = { 0.0f, 0.0f, -1.0f };
            result.sanitized = true;
        }
        if (!std::isfinite(interfaceNormal.x) ||
            !std::isfinite(interfaceNormal.y) ||
            !std::isfinite(interfaceNormal.z) ||
            glm::dot(interfaceNormal, interfaceNormal) <= 1.0e-12f) {
            interfaceNormal = { 0.0f, 0.0f, 1.0f };
            result.sanitized = true;
        }
        const glm::vec3 incident = glm::normalize(incidentDirection);
        glm::vec3 normal = glm::normalize(interfaceNormal);
        if (glm::dot(incident, normal) > 0.0f)
            normal = -normal;
        const float cosine = std::clamp(-glm::dot(incident, normal),
            0.0f, 1.0f);
        const float eta = incidentIor / transmittedIor;
        const float discriminant = 1.0f - eta * eta *
            (1.0f - cosine * cosine);
        if (discriminant <= 0.0f) {
            result.direction = glm::normalize(glm::reflect(incident, normal));
            result.totalInternalReflection = true;
            return result;
        }
        result.direction = glm::normalize(eta * incident +
            (eta * cosine - std::sqrt(discriminant)) * normal);
        return result;
    }

    [[nodiscard]] inline RefractionProjectionResult projectTransparencyRay(
        const glm::mat4& view, const glm::mat4& projection,
        glm::vec3 worldPosition, glm::vec3 transmittedDirection,
        float pathLengthMeters, float metresPerWorldUnit,
        float perceptualRoughness, float incidentToTransmittedEta,
        glm::uvec2 renderExtent, uint32_t mipLevels) noexcept {
        RefractionProjectionResult result{};
        const auto finiteVector = [](glm::vec3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        };
        if (!finiteVector(worldPosition)) {
            worldPosition = glm::vec3(0.0f);
            result.sanitized = true;
        }
        if (!finiteVector(transmittedDirection) ||
            glm::dot(transmittedDirection, transmittedDirection) <= 1.0e-12f) {
            transmittedDirection = { 0.0f, 0.0f, -1.0f };
            result.sanitized = true;
        }
        if (!std::isfinite(pathLengthMeters) || pathLengthMeters < 0.0f) {
            pathLengthMeters = 0.0f;
            result.sanitized = true;
        }
        if (!std::isfinite(metresPerWorldUnit) ||
            metresPerWorldUnit <= 1.0e-7f) {
            metresPerWorldUnit = 1.0f;
            result.sanitized = true;
        }
        if (!std::isfinite(perceptualRoughness)) {
            perceptualRoughness = 0.0f;
            result.sanitized = true;
        }
        if (!std::isfinite(incidentToTransmittedEta) ||
            incidentToTransmittedEta <= 0.0f) {
            incidentToTransmittedEta = 1.0f;
            result.sanitized = true;
        }
        if (renderExtent.x == 0u || renderExtent.y == 0u || mipLevels == 0u) {
            result.sanitized = true;
            return result;
        }

        const glm::vec3 direction = glm::normalize(transmittedDirection);
        const float pathWorld = pathLengthMeters / metresPerWorldUnit;
        const glm::vec3 endpoint = worldPosition + direction * pathWorld;
        const auto project = [&](glm::vec3 position, glm::vec2& uv,
                float& clipW) {
            const glm::vec4 viewPosition = view * glm::vec4(position, 1.0f);
            const glm::vec4 clip = projection * viewPosition;
            clipW = clip.w;
            if (!std::isfinite(clip.w) || std::abs(clip.w) <= 1.0e-7f)
                return false;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            uv = ndc * 0.5f + 0.5f;
            return std::isfinite(uv.x) && std::isfinite(uv.y);
        };
        float sourceW = 0.0f;
        float endpointW = 0.0f;
        const bool sourceValid = project(worldPosition, result.sourceUv, sourceW);
        const bool endpointValid = project(endpoint, result.sampleUv, endpointW);
        const glm::vec3 endpointView = glm::vec3(
            view * glm::vec4(endpoint, 1.0f));
        result.expectedViewDepthMeters = std::abs(endpointView.z) *
            metresPerWorldUnit;
        result.inFrontOfCamera = sourceValid && endpointValid &&
            sourceW > 0.0f && endpointW > 0.0f;

        const float alpha = std::clamp(perceptualRoughness,
            0.0f, 1.0f);
        const float coneTangent = std::clamp(alpha * alpha *
            incidentToTransmittedEta, 0.0f,
            TransparencyMaximumConeTangent);
        const float radiusWorld = pathWorld * coneTangent;
        if (radiusWorld > 0.0f && endpointValid) {
            const glm::vec3 reference = std::abs(direction.z) < 0.999f
                ? glm::vec3(0.0f, 0.0f, 1.0f)
                : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 tangent = glm::normalize(
                glm::cross(reference, direction));
            const glm::vec3 bitangent = glm::normalize(
                glm::cross(direction, tangent));
            glm::vec2 tangentUv{};
            glm::vec2 bitangentUv{};
            float tangentW = 0.0f;
            float bitangentW = 0.0f;
            const bool tangentValid = project(endpoint + tangent * radiusWorld,
                tangentUv, tangentW);
            const bool bitangentValid = project(
                endpoint + bitangent * radiusWorld, bitangentUv, bitangentW);
            const glm::vec2 extent(renderExtent);
            if (tangentValid && tangentW > 0.0f)
                result.footprintPixels = (std::max)(result.footprintPixels,
                    2.0f * glm::length((tangentUv - result.sampleUv) * extent));
            if (bitangentValid && bitangentW > 0.0f)
                result.footprintPixels = (std::max)(result.footprintPixels,
                    2.0f * glm::length((bitangentUv - result.sampleUv) * extent));
        }
        result.lod = std::clamp(std::log2((std::max)(
            result.footprintPixels, 1.0f)), 0.0f,
            static_cast<float>(mipLevels - 1u));
        result.onScreen = result.inFrontOfCamera &&
            result.sampleUv.x >= 0.0f && result.sampleUv.x <= 1.0f &&
            result.sampleUv.y >= 0.0f && result.sampleUv.y <= 1.0f;
        if (result.onScreen) {
            const glm::vec2 edgeUv = glm::min(result.sampleUv,
                glm::vec2(1.0f) - result.sampleUv);
            const float edgePixels = (std::min)(edgeUv.x * renderExtent.x,
                edgeUv.y * renderExtent.y);
            const float band = (std::max)(
                TransparencyMinimumEdgeBandPixels, result.footprintPixels);
            result.edgeConfidence = std::clamp(edgePixels / band,
                0.0f, 1.0f);
        }
        return result;
    }

    [[nodiscard]] inline DielectricFresnelResult dielectricFresnel(
        float incidentIor, float transmittedIor,
        float incidentCosine) noexcept {
        DielectricFresnelResult result{};
        const auto sanitizeIor = [&result](float value) {
            if (!std::isfinite(value) || value < TransparencyMinimumIor ||
                value > TransparencyMaximumIor) {
                result.sanitized = true;
                return std::clamp(std::isfinite(value) ? value : 1.0f,
                    TransparencyMinimumIor, TransparencyMaximumIor);
            }
            return value;
        };
        incidentIor = sanitizeIor(incidentIor);
        transmittedIor = sanitizeIor(transmittedIor);
        if (!std::isfinite(incidentCosine)) {
            incidentCosine = 1.0f;
            result.sanitized = true;
        }
        const float cosIncident = std::clamp(std::abs(incidentCosine), 0.0f, 1.0f);
        const float eta = incidentIor / transmittedIor;
        const float sinTransmittedSquared = eta * eta *
            (1.0f - cosIncident * cosIncident);
        if (sinTransmittedSquared >= 1.0f) {
            result.reflectance = 1.0f;
            result.transmittedCosine = 0.0f;
            result.totalInternalReflection = true;
            return result;
        }

        const float cosTransmitted = std::sqrt((std::max)(
            0.0f, 1.0f - sinTransmittedSquared));
        const float sDenominator = incidentIor * cosIncident +
            transmittedIor * cosTransmitted;
        const float pDenominator = incidentIor * cosTransmitted +
            transmittedIor * cosIncident;
        const float rs = sDenominator > 0.0f
            ? (incidentIor * cosIncident - transmittedIor * cosTransmitted) /
                sDenominator
            : 1.0f;
        const float rp = pDenominator > 0.0f
            ? (incidentIor * cosTransmitted - transmittedIor * cosIncident) /
                pDenominator
            : 1.0f;
        result.reflectance = std::clamp(0.5f * (rs * rs + rp * rp),
            0.0f, 1.0f);
        result.transmittedCosine = cosTransmitted;
        result.totalInternalReflection = false;
        return result;
    }

    [[nodiscard]] inline BeerLambertResult beerLambertTransmittance(
        glm::vec3 attenuationColor, float attenuationDistanceMeters,
        float pathLengthMeters) noexcept {
        BeerLambertResult result{};
        if (!std::isfinite(pathLengthMeters) || pathLengthMeters < 0.0f) {
            pathLengthMeters = 0.0f;
            result.sanitized = true;
        }
        if (pathLengthMeters == 0.0f ||
            std::isinf(attenuationDistanceMeters)) {
            return result;
        }
        if (!std::isfinite(attenuationDistanceMeters) ||
            attenuationDistanceMeters < TransparencyMinimumAttenuationDistance) {
            attenuationDistanceMeters = TransparencyMinimumAttenuationDistance;
            result.sanitized = true;
        }
        for (int channel = 0; channel < 3; ++channel) {
            float color = attenuationColor[channel];
            if (!std::isfinite(color) || color <
                    TransparencyMinimumAttenuationColor || color > 1.0f) {
                color = std::clamp(std::isfinite(color) ? color : 1.0f,
                    TransparencyMinimumAttenuationColor, 1.0f);
                result.sanitized = true;
            }
            result.extinction[channel] = -std::log(color) /
                attenuationDistanceMeters;
            result.transmittance[channel] = std::exp(
                -result.extinction[channel] * pathLengthMeters);
        }
        return result;
    }

    [[nodiscard]] inline ThinSheetPathResult thinSheetPathLength(
        float sheetThicknessMeters, float incidenceCosine,
        float worldThicknessScale = 1.0f) noexcept {
        ThinSheetPathResult result{};
        if (!std::isfinite(sheetThicknessMeters) ||
            sheetThicknessMeters < 0.0f) {
            sheetThicknessMeters = 0.0f;
            result.sanitized = true;
        }
        if (!std::isfinite(worldThicknessScale) || worldThicknessScale < 0.0f) {
            worldThicknessScale = 0.0f;
            result.sanitized = true;
        }
        if (!std::isfinite(incidenceCosine)) {
            incidenceCosine = 1.0f;
            result.sanitized = true;
        }
        result.incidenceCosine = std::clamp(std::abs(incidenceCosine),
            0.0f, 1.0f);
        if (sheetThicknessMeters == 0.0f || worldThicknessScale == 0.0f)
            return result;
        if (result.incidenceCosine < TransparencyMinimumIncidenceCosine) {
            result.incidenceCosine = TransparencyMinimumIncidenceCosine;
            result.grazingClampApplied = true;
        }
        result.pathLengthMeters = sheetThicknessMeters * worldThicknessScale /
            result.incidenceCosine;
        return result;
    }

    // A zero-distance ThinGlass interface cannot displace, blur, or absorb the
    // background. Express it as a local premultiplied blend so transparent work
    // already composed behind the interface remains visible. Nonzero-distance
    // transport still requires the scene-color pyramid.
    [[nodiscard]] inline ThinGlassLocalCompositionWeights
        thinGlassLocalCompositionWeights(float transmission, float metallic,
            glm::vec3 fresnel) noexcept {
        ThinGlassLocalCompositionWeights result{};
        const auto sanitizeUnit = [&result](float value, float fallback) {
            if (!std::isfinite(value)) {
                result.sanitized = true;
                value = fallback;
            }
            if (value < 0.0f || value > 1.0f) {
                result.sanitized = true;
                value = std::clamp(value, 0.0f, 1.0f);
            }
            return value;
        };
        transmission = sanitizeUnit(transmission, 0.0f);
        metallic = sanitizeUnit(metallic, 0.0f);
        for (int channel = 0; channel < 3; ++channel)
            fresnel[channel] = sanitizeUnit(fresnel[channel], 1.0f);
        result.effectiveTransmission = transmission * (1.0f - metallic);
        result.interfaceOpacity = (std::max)({ fresnel.r, fresnel.g,
            fresnel.b });
        result.destinationWeight = result.effectiveTransmission *
            (1.0f - result.interfaceOpacity);
        return result;
    }

} // namespace Iridium
