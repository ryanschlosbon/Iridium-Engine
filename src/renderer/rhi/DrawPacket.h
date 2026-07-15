#pragma once
#include "RenderHandles.h"
#include <glm/glm.hpp>

namespace Iridium {

    // Exactly 96 Bytes (Perfectly fits into 6 CPU Cache Lines)
    struct alignas(16) DrawPacket {

        // --- CHUNK 1: The Transform (64 Bytes) ---
        glm::mat4 worldTransform;     // Offsets 0 - 64

        // --- CHUNK 2: The Tickets (16 Bytes) ---
        GeometryHandle geometry;      // Offsets 64 - 68
        MaterialHandle material;      // Offsets 68 - 72
        uint32_t indexCount;          // Offsets 72 - 76
        uint32_t firstIndex;          // Offsets 76 - 80

        // --- CHUNK 3: State & Padding (16 Bytes) ---
        float distanceToCamera;       // Offsets 80 - 84
        uint32_t isSelected = 0;          // Offsets 84 - 88 (Use uint32 instead of bool!)
        uint32_t _padding[2];         // Offsets 88 - 96 (Explicit padding to hit 16-byte boundary)
    };

} // namespace Iridium