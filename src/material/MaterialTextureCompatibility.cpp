#include "material/MaterialTextureCompatibility.h"

#include "renderer/color/SceneColor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace Iridium {

    namespace {

        FilterMode filter(int32_t value, const char* field) {
            if (value == 9728) return FilterMode::Nearest;
            if (value == 9729) return FilterMode::Linear;
            throw std::invalid_argument(std::string("unsupported glTF ") + field + " filter");
        }

        SamplerAddressMode address(int32_t value) {
            switch (value) {
            case 10497: return SamplerAddressMode::Repeat;
            case 33648: return SamplerAddressMode::MirroredRepeat;
            case 33071: return SamplerAddressMode::ClampToEdge;
            default: throw std::invalid_argument("unsupported glTF sampler wrap mode");
            }
        }

        uint8_t byte(std::byte value) noexcept {
            return std::to_integer<uint8_t>(value);
        }

        std::byte encoded(double value) noexcept {
            const long rounded = std::lround(std::clamp(value, 0.0, 1.0) * 255.0);
            return static_cast<std::byte>(static_cast<uint8_t>(rounded));
        }

    } // namespace

    uint32_t completeMipLevelCount(uint32_t width, uint32_t height) noexcept {
        if (width == 0 || height == 0) return 0;
        uint32_t levels = 1;
        while (width > 1 || height > 1) {
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
            ++levels;
        }
        return levels;
    }

    MaterialTextureCompatibilityPlan planMaterialTextureCompatibility(
        const SourceSampler& source, uint32_t width, uint32_t height) {
        if (width == 0 || height == 0)
            throw std::invalid_argument("material texture dimensions must be nonzero");
        MaterialTextureCompatibilityPlan result{};
        result.sampler.magFilter = source.magFilter.value
            ? filter(*source.magFilter.value, "magnification") : FilterMode::Linear;
        result.sampler.addressU = address(source.wrapS.value);
        result.sampler.addressV = address(source.wrapT.value);
        result.sampler.addressW = SamplerAddressMode::Repeat;

        const int32_t min = source.minFilter.value.value_or(9729);
        bool usesMipmaps = false;
        switch (min) {
        case 9728: result.sampler.minFilter = FilterMode::Nearest; break;
        case 9729: result.sampler.minFilter = FilterMode::Linear; break;
        case 9984:
            result.sampler.minFilter = FilterMode::Nearest;
            result.sampler.mipmapFilter = MipmapFilterMode::Nearest;
            usesMipmaps = true;
            break;
        case 9985:
            result.sampler.minFilter = FilterMode::Linear;
            result.sampler.mipmapFilter = MipmapFilterMode::Nearest;
            usesMipmaps = true;
            break;
        case 9986:
            result.sampler.minFilter = FilterMode::Nearest;
            result.sampler.mipmapFilter = MipmapFilterMode::Linear;
            usesMipmaps = true;
            break;
        case 9987:
            result.sampler.minFilter = FilterMode::Linear;
            result.sampler.mipmapFilter = MipmapFilterMode::Linear;
            usesMipmaps = true;
            break;
        default: throw std::invalid_argument("unsupported glTF minification filter");
        }
        result.mipLevels = usesMipmaps ? completeMipLevelCount(width, height) : 1;
        result.sampler.maxLod = result.mipLevels - 1;
        return result;
    }

    MaterialMipChain buildRgba8MipChain(std::span<const std::byte> baseLevel,
        uint32_t width, uint32_t height, uint32_t mipLevels,
        MaterialMipSemantic semantic) {
        if (width == 0 || height == 0 || mipLevels == 0 ||
            mipLevels > completeMipLevelCount(width, height))
            throw std::invalid_argument("invalid RGBA8 mip-chain dimensions");
        const size_t baseBytes = static_cast<size_t>(width) * height * 4;
        if (baseLevel.size() != baseBytes)
            throw std::invalid_argument("RGBA8 base level byte count does not match dimensions");

        MaterialMipChain result{};
        result.bytes.assign(baseLevel.begin(), baseLevel.end());
        result.levels.push_back({ width, height, 0, baseBytes });
        uint32_t sourceWidth = width;
        uint32_t sourceHeight = height;
        size_t sourceOffset = 0;
        for (uint32_t level = 1; level < mipLevels; ++level) {
            const uint32_t destinationWidth = sourceWidth > 1 ? sourceWidth / 2 : 1;
            const uint32_t destinationHeight = sourceHeight > 1 ? sourceHeight / 2 : 1;
            const size_t destinationOffset = result.bytes.size();
            const size_t destinationBytes = static_cast<size_t>(destinationWidth) *
                destinationHeight * 4;
            result.bytes.resize(destinationOffset + destinationBytes);

            for (uint32_t y = 0; y < destinationHeight; ++y) {
                for (uint32_t x = 0; x < destinationWidth; ++x) {
                    std::array<double, 4> sum{};
                    uint32_t count = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        const uint32_t sy = y * 2 + dy;
                        if (sy >= sourceHeight) continue;
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = x * 2 + dx;
                            if (sx >= sourceWidth) continue;
                            const size_t source = sourceOffset +
                                (static_cast<size_t>(sy) * sourceWidth + sx) * 4;
                            for (size_t channel = 0; channel < 4; ++channel)
                                sum[channel] += byte(result.bytes[source + channel]) / 255.0;
                            ++count;
                        }
                    }
                    for (double& channel : sum) channel /= count;
                    const size_t destination = destinationOffset +
                        (static_cast<size_t>(y) * destinationWidth + x) * 4;
                    if (semantic == MaterialMipSemantic::SrgbColor) {
                        for (size_t channel = 0; channel < 3; ++channel) {
                            double linear = 0.0;
                            for (uint32_t dy = 0; dy < 2; ++dy) {
                                const uint32_t sy = y * 2 + dy;
                                if (sy >= sourceHeight) continue;
                                for (uint32_t dx = 0; dx < 2; ++dx) {
                                    const uint32_t sx = x * 2 + dx;
                                    if (sx >= sourceWidth) continue;
                                    const size_t source = sourceOffset +
                                        (static_cast<size_t>(sy) * sourceWidth + sx) * 4;
                                    linear += Color::decodeSrgb(
                                        byte(result.bytes[source + channel]) / 255.0);
                                }
                            }
                            result.bytes[destination + channel] = encoded(
                                Color::encodeSrgb(linear / count));
                        }
                        result.bytes[destination + 3] = encoded(sum[3]);
                    }
                    else if (semantic == MaterialMipSemantic::TangentNormal) {
                        double nx = sum[0] * 2.0 - 1.0;
                        double ny = sum[1] * 2.0 - 1.0;
                        double nz = sum[2] * 2.0 - 1.0;
                        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (length > 1e-8) { nx /= length; ny /= length; nz /= length; }
                        else { nx = 0.0; ny = 0.0; nz = 1.0; }
                        result.bytes[destination + 0] = encoded(nx * 0.5 + 0.5);
                        result.bytes[destination + 1] = encoded(ny * 0.5 + 0.5);
                        result.bytes[destination + 2] = encoded(nz * 0.5 + 0.5);
                        result.bytes[destination + 3] = encoded(sum[3]);
                    }
                    else {
                        for (size_t channel = 0; channel < 4; ++channel)
                            result.bytes[destination + channel] = encoded(sum[channel]);
                    }
                }
            }
            result.levels.push_back({ destinationWidth, destinationHeight,
                destinationOffset, destinationBytes });
            sourceWidth = destinationWidth;
            sourceHeight = destinationHeight;
            sourceOffset = destinationOffset;
        }
        return result;
    }

} // namespace Iridium
