#include "assets/texture/TextureMipGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace Iridium {

    namespace {
        float srgbToLinear(float value) noexcept {
            value = std::clamp(value, 0.0f, 1.0f);
            return value <= 0.04045f
                ? value / 12.92f
                : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        float linearToSrgb(float value) noexcept {
            value = std::max(value, 0.0f);
            return value <= 0.0031308f
                ? value * 12.92f
                : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }

        std::array<float, 4> texel(
            const FloatRgbaImage& image, uint32_t x, uint32_t y) {
            x = std::min(x, image.width - 1);
            y = std::min(y, image.height - 1);
            const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
            return { image.rgba[offset], image.rgba[offset + 1],
                image.rgba[offset + 2], image.rgba[offset + 3] };
        }

        void preserveCoverage(FloatRgbaImage& image,
            float threshold, double targetCoverage) {
            float low = 0.0f;
            float high = 16.0f;
            for (uint32_t iteration = 0; iteration < 20; ++iteration) {
                const float scale = (low + high) * 0.5f;
                uint64_t covered = 0;
                for (size_t index = 3; index < image.rgba.size(); index += 4) {
                    if (std::clamp(image.rgba[index] * scale, 0.0f, 1.0f) >= threshold) {
                        ++covered;
                    }
                }
                const double coverage = static_cast<double>(covered) /
                    (image.width * image.height);
                if (coverage < targetCoverage) low = scale;
                else high = scale;
            }
            // `high` is the smallest tested scale that still met the target.
            // Using the midpoint can land one ULP below the alpha threshold.
            const float scale = high;
            for (size_t index = 3; index < image.rgba.size(); index += 4) {
                image.rgba[index] = std::clamp(image.rgba[index] * scale, 0.0f, 1.0f);
            }
        }
    }

    double alphaCoverage(const FloatRgbaImage& image, float threshold) noexcept {
        if (image.width == 0 || image.height == 0 ||
            image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4) {
            return 0.0;
        }
        uint64_t covered = 0;
        for (size_t index = 3; index < image.rgba.size(); index += 4) {
            if (image.rgba[index] >= threshold) ++covered;
        }
        return static_cast<double>(covered) / (image.width * image.height);
    }

    std::vector<FloatRgbaImage> generateTextureMipChain(
        const FloatRgbaImage& source, const TextureImportSettings& settings) {
        if (source.width == 0 || source.height == 0 ||
            source.rgba.size() != static_cast<size_t>(source.width) * source.height * 4) {
            throw std::invalid_argument("Texture source pixels do not match dimensions");
        }
        FloatRgbaImage prepared = source;
        for (size_t index = 0; index < prepared.rgba.size(); index += 4) {
            if (settings.alphaMode == TextureAlphaMode::Opaque) {
                prepared.rgba[index + 3] = 1.0f;
            }
            if (settings.semantic != TextureSemantic::Normal) {
                continue;
            }
            float nx = prepared.rgba[index] * 2.0f - 1.0f;
            float ny = prepared.rgba[index + 1] * 2.0f - 1.0f;
            if (settings.flipGreen) {
                ny = -ny;
            }
            float nz = settings.reconstructNormalZ
                ? std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny))
                : prepared.rgba[index + 2] * 2.0f - 1.0f;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 1e-8f) {
                nx /= length;
                ny /= length;
                nz /= length;
            } else {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            }
            prepared.rgba[index] = nx * 0.5f + 0.5f;
            prepared.rgba[index + 1] = ny * 0.5f + 0.5f;
            prepared.rgba[index + 2] = nz * 0.5f + 0.5f;
        }

        std::vector<FloatRgbaImage> result{ std::move(prepared) };
        if (settings.mipPolicy != TextureMipPolicy::FullChain) return result;

        const double sourceCoverage = settings.alphaMode == TextureAlphaMode::Coverage
            ? alphaCoverage(result.front(), settings.alphaCoverageThreshold) : 0.0;
        while (result.back().width > 1 || result.back().height > 1) {
            const FloatRgbaImage& input = result.back();
            FloatRgbaImage output;
            output.width = std::max(1u, input.width / 2);
            output.height = std::max(1u, input.height / 2);
            output.rgba.resize(static_cast<size_t>(output.width) * output.height * 4);

            for (uint32_t y = 0; y < output.height; ++y) {
                for (uint32_t x = 0; x < output.width; ++x) {
                    std::array<std::array<float, 4>, 4> samples = {
                        texel(input, x * 2, y * 2),
                        texel(input, x * 2 + 1, y * 2),
                        texel(input, x * 2, y * 2 + 1),
                        texel(input, x * 2 + 1, y * 2 + 1),
                    };
                    std::array<float, 4> value{};
                    if (settings.semantic == TextureSemantic::Normal) {
                        for (auto sample : samples) {
                            const float nx = sample[0] * 2.0f - 1.0f;
                            const float ny = sample[1] * 2.0f - 1.0f;
                            const float nz = sample[2] * 2.0f - 1.0f;
                            value[0] += nx;
                            value[1] += ny;
                            value[2] += nz;
                            value[3] += sample[3] * 0.25f;
                        }
                        const float length = std::sqrt(value[0] * value[0] +
                            value[1] * value[1] + value[2] * value[2]);
                        if (length > 1e-8f) {
                            value[0] /= length;
                            value[1] /= length;
                            value[2] /= length;
                        } else {
                            value = { 0.0f, 0.0f, 1.0f, value[3] };
                        }
                        value[0] = value[0] * 0.5f + 0.5f;
                        value[1] = value[1] * 0.5f + 0.5f;
                        value[2] = value[2] * 0.5f + 0.5f;
                    } else {
                        for (auto sample : samples) {
                            for (uint32_t channel = 0; channel < 3; ++channel) {
                                value[channel] += settings.viewColorSpace ==
                                    TextureViewColorSpace::sRGB
                                    ? srgbToLinear(sample[channel]) * 0.25f
                                    : sample[channel] * 0.25f;
                            }
                            value[3] += sample[3] * 0.25f;
                        }
                        if (settings.viewColorSpace == TextureViewColorSpace::sRGB) {
                            for (uint32_t channel = 0; channel < 3; ++channel) {
                                value[channel] = linearToSrgb(value[channel]);
                            }
                        }
                    }
                    const size_t offset =
                        (static_cast<size_t>(y) * output.width + x) * 4;
                    std::copy(value.begin(), value.end(), output.rgba.begin() + offset);
                }
            }
            if (settings.alphaMode == TextureAlphaMode::Coverage) {
                preserveCoverage(output, settings.alphaCoverageThreshold, sourceCoverage);
            }
            result.push_back(std::move(output));
        }
        return result;
    }

} // namespace Iridium
