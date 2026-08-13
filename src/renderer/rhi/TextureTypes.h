#pragma once

#include <cstdint>
#include <type_traits>

namespace Iridium {

    enum class TextureFormat : uint8_t {
        RGBA8_UNorm = 0,
        RGBA8_sRGB = 1,
        RGBA16_SFloat = 2,
        RGBA32_SFloat = 3,
        BC4_UNorm = 4,
        BC5_UNorm = 5,
        BC6H_UFloat = 6,
        BC7_UNorm = 7,
        BC7_sRGB = 8,
        // Appended to preserve the serialized values of the M3 texture formats.
        RG16_SFloat = 9,
    };
    enum class FilterMode : uint8_t { Nearest, Linear };
    enum class MipmapFilterMode : uint8_t { Nearest, Linear };
    enum class SamplerAddressMode : uint8_t { Repeat, MirroredRepeat, ClampToEdge };
    enum class SamplerCompareOp : uint8_t {
        Never, Less, LessOrEqual, Greater, GreaterOrEqual, Always
    };
    enum class TextureUsageClass : uint8_t { Sampled2D, Environment };
    enum class TextureViewColorSpace : uint8_t { Linear, sRGB };
    enum class TextureTopology : uint8_t { Texture2D, Texture2DArray, Cube };

    struct SamplerDesc {
        FilterMode minFilter = FilterMode::Linear;
        FilterMode magFilter = FilterMode::Linear;
        MipmapFilterMode mipmapFilter = MipmapFilterMode::Linear;
        SamplerAddressMode addressU = SamplerAddressMode::Repeat;
        SamplerAddressMode addressV = SamplerAddressMode::Repeat;
        SamplerAddressMode addressW = SamplerAddressMode::Repeat;
        uint32_t minLod = 0;
        uint32_t maxLod = 0;
        uint8_t maxAnisotropy = 1;
        bool compareEnable = false;
        SamplerCompareOp compareOp = SamplerCompareOp::LessOrEqual;

        constexpr bool operator==(const SamplerDesc&) const noexcept = default;
    };

    struct TextureStorageDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        TextureUsageClass usageClass = TextureUsageClass::Sampled2D;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        TextureTopology topology = TextureTopology::Texture2D;

        constexpr bool operator==(const TextureStorageDesc&) const noexcept = default;
    };

    struct TextureViewDesc {
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        TextureViewColorSpace colorSpace = TextureViewColorSpace::Linear;
        uint32_t baseMipLevel = 0;
        uint32_t mipLevelCount = 1;
        uint32_t baseArrayLayer = 0;
        uint32_t arrayLayerCount = 1;
        TextureTopology topology = TextureTopology::Texture2D;

        constexpr bool operator==(const TextureViewDesc&) const noexcept = default;
    };

    // Backend-neutral upload request. Cooked products preserve storage, view,
    // sampler, transfer, and asset identity separately before resolving them to
    // the live RHI handles represented by this request.
    struct TextureDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        TextureUsageClass usageClass = TextureUsageClass::Sampled2D;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        TextureTopology topology = TextureTopology::Texture2D;
        SamplerDesc sampler;
    };

    constexpr uint32_t bytesPerTexel(TextureFormat format) noexcept {
        switch (format) {
        case TextureFormat::RGBA8_UNorm:
        case TextureFormat::RGBA8_sRGB:
            return 4;
        case TextureFormat::RG16_SFloat:
            return 4;
        case TextureFormat::RGBA16_SFloat:
            return 8;
        case TextureFormat::RGBA32_SFloat:
            return 16;
        case TextureFormat::BC4_UNorm:
        case TextureFormat::BC5_UNorm:
        case TextureFormat::BC6H_UFloat:
        case TextureFormat::BC7_UNorm:
        case TextureFormat::BC7_sRGB:
            return 0;
        }

        return 0;
    }

    constexpr bool isBlockCompressed(TextureFormat format) noexcept {
        return format == TextureFormat::BC4_UNorm ||
            format == TextureFormat::BC5_UNorm ||
            format == TextureFormat::BC6H_UFloat ||
            format == TextureFormat::BC7_UNorm ||
            format == TextureFormat::BC7_sRGB;
    }

    constexpr uint32_t bytesPerBlock(TextureFormat format) noexcept {
        switch (format) {
        case TextureFormat::BC4_UNorm:
            return 8;
        case TextureFormat::BC5_UNorm:
        case TextureFormat::BC6H_UFloat:
        case TextureFormat::BC7_UNorm:
        case TextureFormat::BC7_sRGB:
            return 16;
        default:
            return bytesPerTexel(format);
        }
    }

    constexpr uint64_t textureMipDataSize(
        TextureFormat format, uint32_t width, uint32_t height) noexcept {
        if (isBlockCompressed(format)) {
            const uint64_t blocksWide = (static_cast<uint64_t>(width) + 3) / 4;
            const uint64_t blocksHigh = (static_cast<uint64_t>(height) + 3) / 4;
            return blocksWide * blocksHigh * bytesPerBlock(format);
        }
        return static_cast<uint64_t>(width) * height * bytesPerTexel(format);
    }

    constexpr uint64_t textureDataSize(const TextureDesc& desc) noexcept {
        uint64_t total = 0;
        uint32_t width = desc.width;
        uint32_t height = desc.height;
        for (uint32_t level = 0; level < desc.mipLevels && width != 0 && height != 0; ++level) {
            total += textureMipDataSize(desc.format, width, height);
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
        return total * desc.arrayLayers;
    }

    constexpr bool validTextureTopology(const TextureDesc& desc) noexcept {
        if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
            desc.arrayLayers == 0) {
            return false;
        }
        switch (desc.topology) {
        case TextureTopology::Texture2D:
            return desc.arrayLayers == 1;
        case TextureTopology::Texture2DArray:
            return true;
        case TextureTopology::Cube:
            return desc.width == desc.height && desc.arrayLayers == 6;
        }
        return false;
    }

    static_assert(std::is_trivially_copyable_v<SamplerDesc>);
    static_assert(std::is_trivially_copyable_v<TextureStorageDesc>);
    static_assert(std::is_trivially_copyable_v<TextureViewDesc>);
    static_assert(std::is_trivially_copyable_v<TextureDesc>);

} // namespace Iridium
