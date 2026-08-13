#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Iridium {

    inline constexpr float MaterialPi = 3.14159265358979323846f;
    inline constexpr float MaterialNormalEpsilon = 1.0e-12f;
    inline constexpr float MaterialMinPerceptualRoughness = 0.04f;
    inline constexpr float MaterialBsdfDenominatorEpsilon = 1.0e-7f;

    struct StandardTangentFrame {
        glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
        glm::vec3 tangent{ 1.0f, 0.0f, 0.0f };
        glm::vec3 bitangent{ 0.0f, 1.0f, 0.0f };
    };

    [[nodiscard]] inline bool finiteMaterialVector(glm::vec3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    [[nodiscard]] inline glm::vec3 materialFallbackTangent(
        glm::vec3 unitNormal) noexcept {
        const glm::vec3 axis = std::abs(unitNormal.z) < 0.999f
            ? glm::vec3(0.0f, 0.0f, 1.0f)
            : glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::normalize(glm::cross(axis, unitNormal));
    }

    [[nodiscard]] inline StandardTangentFrame buildStandardTangentFrame(
        glm::vec3 geometricNormal, glm::vec3 tangent, float handedness,
        bool doubleSided = false, bool frontFacing = true) {
        if (!finiteMaterialVector(geometricNormal) ||
            glm::dot(geometricNormal, geometricNormal) <= MaterialNormalEpsilon)
            throw std::domain_error("geometric normal must be finite and nonzero");
        if (!std::isfinite(handedness) || handedness == 0.0f)
            throw std::domain_error("tangent handedness must be finite and nonzero");
        StandardTangentFrame frame{};
        frame.normal = glm::normalize(geometricNormal);
        if (doubleSided && !frontFacing) frame.normal = -frame.normal;
        tangent -= frame.normal * glm::dot(frame.normal, tangent);
        frame.tangent = !finiteMaterialVector(tangent) ||
            glm::dot(tangent, tangent) <= MaterialNormalEpsilon
            ? materialFallbackTangent(frame.normal)
            : glm::normalize(tangent);
        frame.bitangent = glm::normalize(glm::cross(frame.normal,
            frame.tangent)) * (handedness < 0.0f ? -1.0f : 1.0f);
        return frame;
    }

    [[nodiscard]] inline glm::vec3 applyStandardTangentNormal(
        const StandardTangentFrame& frame, glm::vec3 encodedNormal,
        float normalScale) {
        if (!finiteMaterialVector(encodedNormal) || !std::isfinite(normalScale))
            throw std::domain_error("tangent normal inputs must be finite");
        glm::vec3 tangentNormal{
            (encodedNormal.x * 2.0f - 1.0f) * normalScale,
            (encodedNormal.y * 2.0f - 1.0f) * normalScale,
            encodedNormal.z * 2.0f - 1.0f };
        if (glm::dot(tangentNormal, tangentNormal) <= MaterialNormalEpsilon)
            throw std::domain_error("sampled tangent normal must be nonzero");
        tangentNormal = glm::normalize(tangentNormal);
        return glm::normalize(frame.tangent * tangentNormal.x +
            frame.bitangent * tangentNormal.y + frame.normal * tangentNormal.z);
    }

    [[nodiscard]] inline float sanitizePerceptualRoughness(float value) noexcept {
        return std::clamp(value, 0.0f, 1.0f);
    }

    [[nodiscard]] inline float materialGgxAlpha(float perceptualRoughness) noexcept {
        const float roughness = (std::max)(sanitizePerceptualRoughness(
            perceptualRoughness), MaterialMinPerceptualRoughness);
        return roughness * roughness;
    }

    [[nodiscard]] inline glm::vec3 materialFresnelSchlick(glm::vec3 f0,
        glm::vec3 f90, float cosTheta) noexcept {
        const float weight = std::pow(std::clamp(1.0f - cosTheta,
            0.0f, 1.0f), 5.0f);
        return f0 + (f90 - f0) * weight;
    }

    [[nodiscard]] inline float materialDistributionGgx(float noH,
        float perceptualRoughness) noexcept {
        const float alpha = materialGgxAlpha(perceptualRoughness);
        const float alphaSquared = alpha * alpha;
        const float clampedNoH = std::clamp(noH, 0.0f, 1.0f);
        const float denominatorTerm = clampedNoH * clampedNoH *
            (alphaSquared - 1.0f) + 1.0f;
        return alphaSquared / (std::max)(MaterialPi * denominatorTerm *
            denominatorTerm, MaterialBsdfDenominatorEpsilon);
    }

    [[nodiscard]] inline float materialGeometrySchlickGgx(float noX,
        float perceptualRoughness) noexcept {
        const float roughness = sanitizePerceptualRoughness(perceptualRoughness);
        const float radius = roughness + 1.0f;
        const float k = radius * radius / 8.0f;
        const float clampedNoX = std::clamp(noX, 0.0f, 1.0f);
        return clampedNoX / (std::max)(clampedNoX * (1.0f - k) + k,
            MaterialBsdfDenominatorEpsilon);
    }

    [[nodiscard]] inline float materialGeometrySmith(float noV, float noL,
        float perceptualRoughness) noexcept {
        return materialGeometrySchlickGgx(noV, perceptualRoughness) *
            materialGeometrySchlickGgx(noL, perceptualRoughness);
    }

    [[nodiscard]] inline glm::vec3 materialDiffuseWeight(glm::vec3 fresnel,
        float metallic) noexcept {
        return (glm::vec3(1.0f) - fresnel) *
            (1.0f - std::clamp(metallic, 0.0f, 1.0f));
    }

    [[nodiscard]] inline glm::vec3 materialEvaluateStandardBrdf(
        glm::vec3 baseColor, glm::vec3 f0, glm::vec3 f90, float metallic,
        float perceptualRoughness, glm::vec3 normal, glm::vec3 view,
        glm::vec3 light) noexcept {
        const glm::vec3 halfVector = glm::normalize(view + light);
        const float noV = (std::max)(glm::dot(normal, view), 0.0f);
        const float noL = (std::max)(glm::dot(normal, light), 0.0f);
        const float distribution = materialDistributionGgx(
            (std::max)(glm::dot(normal, halfVector), 0.0f), perceptualRoughness);
        const float geometry = materialGeometrySmith(noV, noL,
            perceptualRoughness);
        const glm::vec3 fresnel = materialFresnelSchlick(f0, f90,
            (std::max)(glm::dot(halfVector, view), 0.0f));
        const glm::vec3 specular = distribution * geometry * fresnel /
            (std::max)(4.0f * noV * noL, MaterialBsdfDenominatorEpsilon);
        return materialDiffuseWeight(fresnel, metallic) * baseColor /
            MaterialPi + specular;
    }

} // namespace Iridium
