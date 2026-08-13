#pragma once

#include "assets/texture/TextureProduct.h"

#include <cstdint>
#include <vector>

namespace Iridium {

    struct FloatRgbaImage {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> rgba;

        bool operator==(const FloatRgbaImage&) const = default;
    };

    [[nodiscard]] std::vector<FloatRgbaImage> generateTextureMipChain(
        const FloatRgbaImage& source, const TextureImportSettings& settings);
    [[nodiscard]] double alphaCoverage(
        const FloatRgbaImage& image, float threshold) noexcept;

} // namespace Iridium
