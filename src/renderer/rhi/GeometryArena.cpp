#include "renderer/rhi/GeometryArena.h"

#include <algorithm>
#include <limits>

namespace Iridium {

    namespace {

        GeometryArenaBuildResult fail(
            GeometryArenaBuildError error,
            GeometryArenaPrimitiveIdentity primitive = {}) {
            return { .error = error, .errorPrimitive = primitive };
        }

        bool rangeFits(uint64_t first, uint64_t count, uint64_t size) noexcept {
            return first <= size && count <= size - first;
        }

    } // namespace

    GeometryArenaBuildResult buildGeometryArena(
        uint32_t abiVersion,
        uint64_t vertexCount,
        std::span<const uint32_t> sourceIndices,
        std::span<const GeometryArenaPrimitiveInput> primitives) {
        if (abiVersion != GeometryArenaAbiVersion) {
            return fail(GeometryArenaBuildError::InvalidAbiVersion);
        }

        std::vector<const GeometryArenaPrimitiveInput*> ordered;
        ordered.reserve(primitives.size());
        for (const GeometryArenaPrimitiveInput& primitive : primitives) {
            if (primitive.identity.sourcePrimitiveGuid.isNil() ||
                primitive.identity.primitiveGuid.isNil()) {
                return fail(GeometryArenaBuildError::NilPrimitiveIdentity,
                    primitive.identity);
            }
            ordered.push_back(&primitive);
        }
        std::ranges::sort(ordered, {},
            &GeometryArenaPrimitiveInput::identity);
        for (size_t index = 1; index < ordered.size(); ++index) {
            if (ordered[index - 1]->identity == ordered[index]->identity) {
                return fail(GeometryArenaBuildError::DuplicatePrimitiveIdentity,
                    ordered[index]->identity);
            }
        }

        GeometryArenaData arena;
        arena.vertexCount = vertexCount;
        arena.primitives.reserve(ordered.size());

        for (const GeometryArenaPrimitiveInput* primitive : ordered) {
            if (!rangeFits(primitive->firstVertex, primitive->vertexCount,
                    vertexCount)) {
                return fail(GeometryArenaBuildError::VertexRangeOutOfBounds,
                    primitive->identity);
            }
            if (!rangeFits(primitive->firstIndex, primitive->indexCount,
                    sourceIndices.size())) {
                return fail(GeometryArenaBuildError::IndexRangeOutOfBounds,
                    primitive->identity);
            }
            if (primitive->firstVertex >
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
                primitive->indexCount >
                    static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                return fail(GeometryArenaBuildError::DrawRangeOverflow,
                    primitive->identity);
            }

            uint32_t maximumLocalIndex = 0;
            const auto source = sourceIndices.subspan(
                static_cast<size_t>(primitive->firstIndex),
                static_cast<size_t>(primitive->indexCount));
            for (uint32_t globalIndex : source) {
                const uint64_t global = globalIndex;
                if (global < primitive->firstVertex ||
                    global - primitive->firstVertex >= primitive->vertexCount) {
                    return fail(
                        GeometryArenaBuildError::IndexOutsidePrimitiveVertices,
                        primitive->identity);
                }
                maximumLocalIndex = std::max(maximumLocalIndex,
                    static_cast<uint32_t>(global - primitive->firstVertex));
            }

            const bool useUInt16 = maximumLocalIndex <=
                std::numeric_limits<uint16_t>::max();
            const size_t destinationSize = useUInt16
                ? arena.uint16Indices.size() : arena.uint32Indices.size();
            if (destinationSize > std::numeric_limits<uint32_t>::max() ||
                primitive->indexCount >
                    std::numeric_limits<uint32_t>::max() - destinationSize) {
                return fail(GeometryArenaBuildError::DrawRangeOverflow,
                    primitive->identity);
            }

            GeometryArenaPrimitiveRange range{
                .identity = primitive->identity,
                .indexStream = useUInt16 ? GeometryArenaIndexStream::UInt16
                    : GeometryArenaIndexStream::UInt32,
                .firstIndex = static_cast<uint32_t>(destinationSize),
                .indexCount = static_cast<uint32_t>(primitive->indexCount),
                .vertexOffset = static_cast<int32_t>(primitive->firstVertex),
            };
            if (useUInt16) {
                for (uint32_t globalIndex : source) {
                    arena.uint16Indices.push_back(static_cast<uint16_t>(
                        static_cast<uint64_t>(globalIndex) -
                        primitive->firstVertex));
                }
                ++arena.stats.uint16PrimitiveCount;
                arena.stats.uint16IndexCount += primitive->indexCount;
            }
            else {
                for (uint32_t globalIndex : source) {
                    arena.uint32Indices.push_back(static_cast<uint32_t>(
                        static_cast<uint64_t>(globalIndex) -
                        primitive->firstVertex));
                }
                ++arena.stats.uint32PrimitiveCount;
                arena.stats.uint32IndexCount += primitive->indexCount;
            }
            arena.primitives.push_back(range);
        }

        arena.stats.sourceIndexBytes =
            (arena.stats.uint16IndexCount + arena.stats.uint32IndexCount) *
            sizeof(uint32_t);
        arena.stats.arenaIndexBytes = arena.stats.uint16IndexCount *
            sizeof(uint16_t) + arena.stats.uint32IndexCount * sizeof(uint32_t);
        arena.stats.savedIndexBytes = arena.stats.sourceIndexBytes -
            arena.stats.arenaIndexBytes;
        return { .data = std::move(arena) };
    }

} // namespace Iridium
