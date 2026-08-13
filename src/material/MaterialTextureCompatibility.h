#pragma once

#include "material/SourceMaterial.h"
#include "renderer/rhi/TextureTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    enum class MaterialMipSemantic : uint8_t {
        LinearData,
        SrgbColor,
        TangentNormal,
    };

    struct MaterialMipLevel {
        uint32_t width = 0;
        uint32_t height = 0;
        size_t byteOffset = 0;
        size_t byteSize = 0;
    };

    struct MaterialMipChain {
        std::vector<std::byte> bytes;
        std::vector<MaterialMipLevel> levels;
    };

    struct MaterialTextureCompatibilityPlan {
        SamplerDesc sampler;
        uint32_t mipLevels = 1;
    };

    [[nodiscard]] uint32_t completeMipLevelCount(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] MaterialTextureCompatibilityPlan planMaterialTextureCompatibility(
        const SourceSampler& source, uint32_t width, uint32_t height);
    [[nodiscard]] MaterialMipChain buildRgba8MipChain(
        std::span<const std::byte> baseLevel, uint32_t width, uint32_t height,
        uint32_t mipLevels, MaterialMipSemantic semantic);

} // namespace Iridium
