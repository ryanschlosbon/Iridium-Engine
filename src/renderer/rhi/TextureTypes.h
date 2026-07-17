#pragma once

#include <cstdint>
#include <type_traits>

namespace Iridium {

    enum class TextureFormat : uint8_t { RGBA8_UNorm, RGBA8_sRGB, RGBA32_SFloat };
    enum class FilterMode : uint8_t { Nearest, Linear };
    enum class SamplerAddressMode : uint8_t { Repeat, ClampToEdge };

    struct SamplerDesc {
        FilterMode minFilter = FilterMode::Linear;
        FilterMode magFilter = FilterMode::Linear;
        SamplerAddressMode addressU = SamplerAddressMode::Repeat;
        SamplerAddressMode addressV = SamplerAddressMode::Repeat;
        SamplerAddressMode addressW = SamplerAddressMode::Repeat;
    };

    struct TextureDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        SamplerDesc sampler;
    };

    constexpr uint32_t bytesPerTexel(TextureFormat format) noexcept {
        switch (format) {
        case TextureFormat::RGBA8_UNorm:
        case TextureFormat::RGBA8_sRGB:
            return 4;
        case TextureFormat::RGBA32_SFloat:
            return 16;
        }

        return 0;
    }

    static_assert(std::is_trivially_copyable_v<SamplerDesc>);
    static_assert(std::is_trivially_copyable_v<TextureDesc>);

} // namespace Iridium
