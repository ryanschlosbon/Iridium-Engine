#pragma once
#include "RenderHandles.h"
#include <cstdint>
#include <type_traits>
#include <glm/glm.hpp>

namespace Iridium {

    // Exactly 112 Bytes (7 CPU Cache Lines)
    struct alignas(16) DrawPacket {

        // --- CHUNK 1: The Transform (64 Bytes) ---
        glm::mat4 worldTransform;     // Offsets 0 - 64

        // --- SORT KEY & TICKETS (28 Bytes) ---
        uint64_t opaqueSortKey;       // Offsets 64 - 72
        GeometryHandle geometry;      // Offsets 72 - 76
        MaterialHandle material;      // Offsets 76 - 80
        PipelineHandle pipeline;      // Offsets 80 - 84
        uint32_t indexCount;          // Offsets 84 - 88
        uint32_t firstIndex;          // Offsets 88 - 92

        // --- STATE & PADDING (20 Bytes) ---
        float distanceToCamera;       // Offsets 92 - 96
        uint32_t isSelected = 0;      // Offsets 96 - 100 (Use uint32 instead of bool!)
        uint32_t _padding[3];         // Offsets 100 - 112 (Explicit padding to hit 16-byte boundary)
    };

    static_assert(sizeof(DrawPacket) == 112);
    static_assert(alignof(DrawPacket) == 16);
    static_assert(std::is_trivially_copyable_v<DrawPacket>);

} // namespace Iridium
