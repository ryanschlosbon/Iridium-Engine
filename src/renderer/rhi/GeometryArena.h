#pragma once

#include "assets/AssetGuid.h"
#include "renderer/rhi/RenderHandles.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t GeometryArenaAbiVersion = 1;
    inline constexpr uint32_t InvalidGeometryArenaIndex = UINT32_MAX;

    enum class GeometryArenaIndexStream : uint32_t {
        UInt16 = 0,
        UInt32 = 1,
    };

    enum class GeometryArenaBuildError : uint32_t {
        None = 0,
        InvalidAbiVersion,
        NilPrimitiveIdentity,
        DuplicatePrimitiveIdentity,
        VertexRangeOutOfBounds,
        IndexRangeOutOfBounds,
        IndexOutsidePrimitiveVertices,
        DrawRangeOverflow,
    };

    struct GeometryArenaPrimitiveIdentity {
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;

        auto operator<=>(const GeometryArenaPrimitiveIdentity&) const = default;
    };

    // Source indices use the current cooked-model convention: absolute indices
    // into a shared vertex array. The arena converts them to primitive-local
    // indices and preserves firstVertex as the indexed draw's base vertex.
    struct GeometryArenaPrimitiveInput {
        GeometryArenaPrimitiveIdentity identity;
        uint64_t firstVertex = 0;
        uint64_t vertexCount = 0;
        uint64_t firstIndex = 0;
        uint64_t indexCount = 0;
    };

    struct GeometryArenaPrimitiveRange {
        GeometryArenaPrimitiveIdentity identity;
        GeometryArenaIndexStream indexStream = GeometryArenaIndexStream::UInt32;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;

        auto operator<=>(const GeometryArenaPrimitiveRange&) const = default;
    };

    struct GeometryArenaBuildStats {
        uint64_t sourceIndexBytes = 0;
        uint64_t arenaIndexBytes = 0;
        uint64_t savedIndexBytes = 0;
        uint64_t uint16IndexCount = 0;
        uint64_t uint32IndexCount = 0;
        uint32_t uint16PrimitiveCount = 0;
        uint32_t uint32PrimitiveCount = 0;

        auto operator<=>(const GeometryArenaBuildStats&) const = default;
    };

    struct GeometryArenaData {
        uint32_t abiVersion = GeometryArenaAbiVersion;
        uint64_t vertexCount = 0;
        std::vector<uint16_t> uint16Indices;
        std::vector<uint32_t> uint32Indices;
        std::vector<GeometryArenaPrimitiveRange> primitives;
        GeometryArenaBuildStats stats;
    };

    // Handles are ordered exactly like GeometryArenaData::primitives. They are
    // one allocation unit and must be retired together with freeGeometryArena.
    struct GeometryArenaAllocation {
        std::vector<GeometryHandle> primitiveGeometry;

        [[nodiscard]] bool valid() const noexcept {
            return !primitiveGeometry.empty() &&
                std::ranges::all_of(primitiveGeometry,
                    [](GeometryHandle handle) { return handle.isValid(); });
        }
    };

    struct GeometryArenaBuildResult {
        std::optional<GeometryArenaData> data;
        GeometryArenaBuildError error = GeometryArenaBuildError::None;
        GeometryArenaPrimitiveIdentity errorPrimitive;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && error == GeometryArenaBuildError::None;
        }
    };

    // Produces byte-identical output for the same primitive identities and source
    // ranges regardless of input primitive order. Failure is atomic: no partial
    // arena is returned.
    [[nodiscard]] GeometryArenaBuildResult buildGeometryArena(
        uint32_t abiVersion,
        uint64_t vertexCount,
        std::span<const uint32_t> sourceIndices,
        std::span<const GeometryArenaPrimitiveInput> primitives);

} // namespace Iridium
