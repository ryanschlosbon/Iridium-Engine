#pragma once
#include <cstdint>

namespace Iridium {

    // The Generic Handle Template
    template <typename Tag>
    struct RenderHandle {
        uint32_t id = 0; // 0 represents an invalid/null handle

        bool isValid() const { return id != 0; }

        // Bitwise extraction: Lower 20 bits for the index (up to 1,048,576 concurrent items)
        uint32_t getIndex() const { return id & 0xFFFFF; }

        // Bitwise extraction: Upper 12 bits for the generation (allows 4,096 reallocations per slot)
        uint32_t getGeneration() const { return id >> 20; }

        // Operator overloads for sorting and map lookups
        bool operator==(const RenderHandle& other) const { return id == other.id; }
        bool operator!=(const RenderHandle& other) const { return id != other.id; }
        bool operator<(const RenderHandle& other) const { return id < other.id; }
    };

    // The unique Tags for type-safety
    struct GeometryTag {};
    struct MaterialTag {};
    struct TextureTag {};

    // The explicit Handle types you will use throughout the engine
    using GeometryHandle = RenderHandle<GeometryTag>;
    using MaterialHandle = RenderHandle<MaterialTag>;
    using TextureHandle = RenderHandle<TextureTag>;

} // namespace Iridium