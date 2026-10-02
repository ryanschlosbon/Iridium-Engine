#pragma once

#include "material/MaterialCompiler.h"
#include "material/StandardMaterialShading.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <type_traits>

namespace Iridium {

    struct ComplexShadingInputs {
        glm::vec3 baseColor{ 1.0f };
        glm::vec3 f0{ 0.04f };
        glm::vec3 f90{ 1.0f };
        float metallic = 0.0f;
        float perceptualRoughness = 1.0f;
        StandardTangentFrame frame{};
        glm::vec3 view{ 0.0f, 0.0f, 1.0f };
        glm::vec3 light{ 0.0f, 0.0f, 1.0f };
    };

    struct ComplexShadingResult {
        glm::vec3 reflectionBrdf{ 0.0f };
        float transmission = 0.0f;
        float transmissionIor = 1.5f;
        float volumeThickness = 0.0f;
        float attenuationDistance = std::numeric_limits<float>::infinity();
        glm::vec3 attenuationColor{ 1.0f };
        float dispersion = 0.0f;
        float diffuseTransmission = 0.0f;
        glm::vec3 diffuseTransmissionColor{ 1.0f };
    };

    [[nodiscard]] inline glm::vec3 materialEvaluateSpecularLobe(
        glm::vec3 f0, glm::vec3 f90, float perceptualRoughness,
        glm::vec3 normal, glm::vec3 view, glm::vec3 light) noexcept {
        const glm::vec3 halfVector = glm::normalize(view + light);
        const float noV = (std::max)(glm::dot(normal, view), 0.0f);
        const float noL = (std::max)(glm::dot(normal, light), 0.0f);
        const float distribution = materialDistributionGgx(
            (std::max)(glm::dot(normal, halfVector), 0.0f), perceptualRoughness);
        const float geometry = materialGeometrySmith(noV, noL, perceptualRoughness);
        const glm::vec3 fresnel = materialFresnelSchlick(f0, f90,
            (std::max)(glm::dot(halfVector, view), 0.0f));
        return distribution * geometry * fresnel /
            (std::max)(4.0f * noV * noL, MaterialBsdfDenominatorEpsilon);
    }

    [[nodiscard]] inline glm::vec3 materialIridescenceTint(float ior,
        float thicknessNm, float cosTheta) noexcept {
        const glm::vec3 wavelengths{ 650.0f, 510.0f, 475.0f };
        const glm::vec3 phase = 4.0f * MaterialPi * (std::max)(ior, 1.0f) *
            (std::max)(thicknessNm, 0.0f) * (std::max)(cosTheta, 0.0f) /
            wavelengths;
        return glm::clamp(glm::vec3(0.5f) + 0.5f * glm::cos(phase),
            glm::vec3(0.0f), glm::vec3(1.0f));
    }

    [[nodiscard]] inline float materialDistributionAnisotropicGgx(
        glm::vec3 normal, glm::vec3 tangent, glm::vec3 bitangent,
        glm::vec3 halfVector, float perceptualRoughness,
        float anisotropy) noexcept {
        const float alpha = materialGgxAlpha(perceptualRoughness);
        const float aspect = std::sqrt((std::max)(1.0f - 0.9f *
            std::clamp(anisotropy, 0.0f, 1.0f), 0.1f));
        const float alphaX = (std::max)(alpha / aspect, 0.001f);
        const float alphaY = (std::max)(alpha * aspect, 0.001f);
        const float tx = glm::dot(tangent, halfVector) / alphaX;
        const float by = glm::dot(bitangent, halfVector) / alphaY;
        const float nh = (std::max)(glm::dot(normal, halfVector), 0.0f);
        const float denominator = tx * tx + by * by + nh * nh;
        return 1.0f / (std::max)(MaterialPi * alphaX * alphaY *
            denominator * denominator, MaterialBsdfDenominatorEpsilon);
    }

    [[nodiscard]] inline glm::vec3 materialEvaluateAnisotropicSpecular(
        glm::vec3 f0, glm::vec3 f90, float perceptualRoughness,
        float anisotropy, float rotation, const StandardTangentFrame& frame,
        glm::vec3 view, glm::vec3 light) noexcept {
        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        const glm::vec3 tangent = frame.tangent * c + frame.bitangent * s;
        const glm::vec3 bitangent = -frame.tangent * s + frame.bitangent * c;
        const glm::vec3 halfVector = glm::normalize(view + light);
        const float noV = (std::max)(glm::dot(frame.normal, view), 0.0f);
        const float noL = (std::max)(glm::dot(frame.normal, light), 0.0f);
        const float distribution = materialDistributionAnisotropicGgx(
            frame.normal, tangent, bitangent, halfVector,
            perceptualRoughness, anisotropy);
        const float geometry = materialGeometrySmith(noV, noL,
            perceptualRoughness);
        const glm::vec3 fresnel = materialFresnelSchlick(f0, f90,
            (std::max)(glm::dot(halfVector, view), 0.0f));
        return distribution * geometry * fresnel /
            (std::max)(4.0f * noV * noL, MaterialBsdfDenominatorEpsilon);
    }

    [[nodiscard]] inline ComplexShadingResult evaluateComplexMaterial(
        const ComplexShadingInputs& inputs,
        std::span<const ComplexLobeRecord> lobes) noexcept {
        ComplexShadingResult result{};
        result.reflectionBrdf = materialEvaluateStandardBrdf(inputs.baseColor,
            inputs.f0, inputs.f90, inputs.metallic, inputs.perceptualRoughness,
            inputs.frame.normal, inputs.view, inputs.light);
        for (const ComplexLobeRecord& lobe : lobes) {
            std::visit([&](const auto& data) {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, ClearcoatLobe>) {
                    const glm::vec3 coatF = materialFresnelSchlick(
                        glm::vec3(0.04f), glm::vec3(1.0f),
                        (std::max)(glm::dot(inputs.frame.normal, inputs.view), 0.0f)) *
                        data.factor;
                    result.reflectionBrdf *= glm::vec3(1.0f) - coatF;
                    result.reflectionBrdf += materialEvaluateSpecularLobe(
                        glm::vec3(0.04f * data.factor), glm::vec3(data.factor),
                        data.roughnessFactor, inputs.frame.normal, inputs.view,
                        inputs.light);
                }
                else if constexpr (std::is_same_v<T, SheenLobe>) {
                    const glm::vec3 halfVector = glm::normalize(inputs.view + inputs.light);
                    const float inverseHalf = std::clamp(1.0f - (std::max)(
                        glm::dot(inputs.frame.normal, halfVector), 0.0f), 0.0f, 1.0f);
                    const float exponent = 5.0f + (1.0f - 5.0f) *
                        sanitizePerceptualRoughness(data.roughnessFactor);
                    result.reflectionBrdf += data.color * std::pow(inverseHalf,
                        exponent) / MaterialPi;
                }
                else if constexpr (std::is_same_v<T, AnisotropyLobe>) {
                    result.reflectionBrdf += materialEvaluateAnisotropicSpecular(
                        inputs.f0, inputs.f90, inputs.perceptualRoughness,
                        data.strength, data.rotation, inputs.frame,
                        inputs.view, inputs.light) -
                        materialEvaluateSpecularLobe(inputs.f0, inputs.f90,
                        inputs.perceptualRoughness, inputs.frame.normal,
                        inputs.view, inputs.light);
                }
                else if constexpr (std::is_same_v<T, IridescenceLobe>) {
                    const float thickness = 0.5f * (data.thicknessMinimumNm +
                        data.thicknessMaximumNm);
                    const glm::vec3 filmF0 = glm::mix(inputs.f0,
                        materialIridescenceTint(data.ior, thickness,
                            (std::max)(glm::dot(inputs.frame.normal, inputs.view), 0.0f)),
                        data.factor);
                    result.reflectionBrdf += materialEvaluateSpecularLobe(filmF0,
                        inputs.f90, inputs.perceptualRoughness,
                        inputs.frame.normal, inputs.view, inputs.light) -
                        materialEvaluateSpecularLobe(inputs.f0, inputs.f90,
                        inputs.perceptualRoughness, inputs.frame.normal,
                        inputs.view, inputs.light);
                }
                else if constexpr (std::is_same_v<T, ThinTransmissionLobe>) {
                    result.transmission = data.factor;
                    result.transmissionIor = data.ior;
                }
                else if constexpr (std::is_same_v<T, VolumeTransmissionLobe>) {
                    result.volumeThickness = data.thicknessFactor;
                    result.attenuationDistance = data.attenuationDistance;
                    result.attenuationColor = data.attenuationColor;
                }
                else if constexpr (std::is_same_v<T, DispersionLobe>)
                    result.dispersion = data.dispersion;
                else if constexpr (std::is_same_v<T, DiffuseTransmissionLobe>) {
                    result.diffuseTransmission = data.factor;
                    result.diffuseTransmissionColor = data.color;
                }
            }, lobe.data);
        }
        return result;
    }

} // namespace Iridium
