#pragma once
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace Iridium {

    // The Generic Handle Template
    template <typename Tag>
    struct RenderHandle {
        static constexpr uint32_t IndexBits = 20;
        static constexpr uint32_t GenerationBits = 12;
        static constexpr uint32_t IndexMask = (uint32_t{1} << IndexBits) - 1;
        static constexpr uint32_t MaxIndex = IndexMask;
        static constexpr uint32_t MaxGeneration = (uint32_t{1} << GenerationBits) - 1;

        uint32_t id = 0; // 0 represents an invalid/null handle

        constexpr bool isValid() const noexcept { return id != 0; }

        // Bitwise extraction: Lower 20 bits for the index (up to 1,048,576 concurrent items)
        constexpr uint32_t getIndex() const noexcept { return id & IndexMask; }

        // Bitwise extraction: Upper 12 bits for the generation (allows 4,096 reallocations per slot)
        constexpr uint32_t getGeneration() const noexcept { return id >> IndexBits; }

        [[nodiscard]] static constexpr RenderHandle fromParts(uint32_t index, uint32_t generation) {
            if (index > MaxIndex) {
                throw std::out_of_range("RenderHandle index exceeds 20-bit capacity");
            }
            if (generation == 0 || generation > MaxGeneration) {
                throw std::out_of_range("RenderHandle generation must be in the range 1..MaxGeneration");
            }

            return RenderHandle{ (generation << IndexBits) | index };
        }

        // Operator overloads for sorting and map lookups
        constexpr bool operator==(const RenderHandle& other) const noexcept { return id == other.id; }
        constexpr bool operator!=(const RenderHandle& other) const noexcept { return id != other.id; }
        constexpr bool operator<(const RenderHandle& other) const noexcept { return id < other.id; }
    };

    // The unique Tags for type-safety
    struct GeometryTag {};
    struct MaterialTag {};
    struct TextureTag {};
    struct TextureViewTag {};
    struct SamplerTag {};
    struct PipelineTag {};

    // The explicit Handle types you will use throughout the engine
    using GeometryHandle = RenderHandle<GeometryTag>;
    using MaterialHandle = RenderHandle<MaterialTag>;
    using TextureHandle = RenderHandle<TextureTag>;
    using TextureViewHandle = RenderHandle<TextureViewTag>;
    using SamplerHandle = RenderHandle<SamplerTag>;
    using PipelineHandle = RenderHandle<PipelineTag>;

    static_assert(sizeof(GeometryHandle) == 4 && std::is_trivially_copyable_v<GeometryHandle>);
    static_assert(sizeof(MaterialHandle) == 4 && std::is_trivially_copyable_v<MaterialHandle>);
    static_assert(sizeof(TextureHandle) == 4 && std::is_trivially_copyable_v<TextureHandle>);
    static_assert(sizeof(TextureViewHandle) == 4 &&
        std::is_trivially_copyable_v<TextureViewHandle>);
    static_assert(sizeof(SamplerHandle) == 4 && std::is_trivially_copyable_v<SamplerHandle>);
    static_assert(sizeof(PipelineHandle) == 4 && std::is_trivially_copyable_v<PipelineHandle>);

} // namespace Iridium
