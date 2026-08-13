#pragma once

#include "assets/environment/EnvironmentProduct.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace Iridium {

    struct EnvironmentFloatImage {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<glm::vec4> pixels;
    };

    struct EnvironmentFloatCube {
        uint32_t baseSize = 0;
        // Each mip is face-major in +X,-X,+Y,-Y,+Z,-Z order.
        std::vector<std::vector<glm::vec4>> mips;
    };

    struct EnvironmentConvolutionSettings {
        // A 1024-face cube retains approximately 4K equirectangular sky detail
        // while keeping each resident radiance chain near 64 MiB (RGBA16F).
        uint32_t radianceSize = 1024;
        uint32_t irradianceSize = 32;
        // High-end production default. Rough materials naturally consume the
        // lower mips; this resolution primarily preserves mirrors, clearcoat,
        // polished metals, and the glass path introduced by M6.
        uint32_t prefilteredSize = 1024;
        uint32_t brdfLutSize = 256;
        uint32_t prefilteredSamples = 1024;
        uint32_t brdfSamples = 1024;
        std::string sourcePrimaries = "linear_rec709_d65";
        float sourceRadianceScale = 1.0f;
    };

    struct ConvolvedEnvironment {
        EnvironmentFloatCube radiance;
        EnvironmentFloatCube irradiance;
        EnvironmentFloatCube prefilteredSpecular;
        uint32_t brdfLutSize = 0;
        std::vector<glm::vec2> brdfLut;
    };

    [[nodiscard]] glm::vec3 environmentCubeDirection(uint32_t face,
        float u, float v);
    [[nodiscard]] glm::vec4 sampleEnvironmentEquirect(
        const EnvironmentFloatImage& image, glm::vec3 direction);
    [[nodiscard]] ConvolvedEnvironment convolveEnvironmentReference(
        const EnvironmentFloatImage& source,
        const EnvironmentConvolutionSettings& settings,
        std::stop_token stopToken = {});
    // Builds the same cosine-kernel SH9 diffuse product as the HDR importer
    // from a face-major RGBA16F cube captured in scene-linear AP1/D60.
    [[nodiscard]] std::vector<std::byte>
        makeCapturedCubeDiffuseIrradiance(
            std::span<const std::byte> radianceRgba16,
            uint32_t radianceSize,
            uint32_t irradianceSize = 32);
    [[nodiscard]] CookProduct makeConvolvedEnvironmentProduct(
        const AssetGuid& sourceTextureGuid, const ConvolvedEnvironment& environment,
        const EnvironmentConvolutionSettings& settings,
        std::string toolVersion);

} // namespace Iridium
