#include "renderer/rhi/GeometryArena.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n'; \
        return false; } } while (false)

    AssetGuid guid(const char* text) { return *AssetGuid::parse(text); }

    GeometryArenaPrimitiveInput primitive(uint8_t suffix, uint64_t firstVertex,
        uint64_t vertexCount, uint64_t firstIndex, uint64_t indexCount) {
        auto source = AssetGuid::Bytes{};
        auto derived = AssetGuid::Bytes{};
        source[15] = suffix;
        derived[15] = static_cast<uint8_t>(suffix + 32u);
        return {
            .identity = { AssetGuid(source), AssetGuid(derived) },
            .firstVertex = firstVertex,
            .vertexCount = vertexCount,
            .firstIndex = firstIndex,
            .indexCount = indexCount,
        };
    }

    bool eligibleIndicesBecomeLocalUInt16() {
        const std::vector<uint32_t> indices{ 100, 102, 101, 100, 103, 102 };
        const auto input = primitive(1, 100, 4, 0, indices.size());
        const auto result = buildGeometryArena(GeometryArenaAbiVersion, 104,
            indices, std::span(&input, 1));
        CHECK(result.valid());
        CHECK(result.data->uint16Indices ==
            std::vector<uint16_t>({ 0, 2, 1, 0, 3, 2 }));
        CHECK(result.data->uint32Indices.empty());
        CHECK(result.data->primitives[0].vertexOffset == 100);
        CHECK(result.data->primitives[0].firstIndex == 0);
        CHECK(result.data->primitives[0].indexStream ==
            GeometryArenaIndexStream::UInt16);
        CHECK(result.data->stats.sourceIndexBytes == 24);
        CHECK(result.data->stats.arenaIndexBytes == 12);
        CHECK(result.data->stats.savedIndexBytes == 12);
        return true;
    }

    bool wideLocalIndexRemainsUInt32() {
        const std::vector<uint32_t> indices{ 4, 65540, 70004 };
        const auto input = primitive(2, 4, 70001, 0, indices.size());
        const auto result = buildGeometryArena(GeometryArenaAbiVersion, 70005,
            indices, std::span(&input, 1));
        CHECK(result.valid());
        CHECK(result.data->uint16Indices.empty());
        CHECK(result.data->uint32Indices ==
            std::vector<uint32_t>({ 0, 65536, 70000 }));
        CHECK(result.data->primitives[0].indexStream ==
            GeometryArenaIndexStream::UInt32);
        CHECK(result.data->stats.savedIndexBytes == 0);
        return true;
    }

    bool mixedStreamsAreIdentityDeterministic() {
        const std::vector<uint32_t> indices{
            70010, 135546, 70011,
            10, 12, 11,
        };
        std::vector inputs{
            primitive(9, 70010, 70000, 0, 3),
            primitive(3, 10, 3, 3, 3),
        };
        const auto first = buildGeometryArena(GeometryArenaAbiVersion, 140010,
            indices, inputs);
        CHECK(first.valid());
        CHECK(first.data->primitives.size() == 2);
        CHECK(first.data->primitives[0].identity == inputs[1].identity);
        CHECK(first.data->primitives[1].identity == inputs[0].identity);
        CHECK(first.data->uint16Indices == std::vector<uint16_t>({ 0, 2, 1 }));
        CHECK(first.data->uint32Indices ==
            std::vector<uint32_t>({ 0, 65536, 1 }));
        CHECK(first.data->stats.sourceIndexBytes == 24);
        CHECK(first.data->stats.arenaIndexBytes == 18);
        CHECK(first.data->stats.savedIndexBytes == 6);

        std::ranges::reverse(inputs);
        const auto second = buildGeometryArena(GeometryArenaAbiVersion, 140010,
            indices, inputs);
        CHECK(second.valid());
        CHECK(second.data->primitives == first.data->primitives);
        CHECK(second.data->uint16Indices == first.data->uint16Indices);
        CHECK(second.data->uint32Indices == first.data->uint32Indices);
        CHECK(second.data->stats == first.data->stats);
        return true;
    }

    bool invalidPrimitiveRejectsWholeBuild() {
        const std::vector<uint32_t> indices{ 0, 1, 2, 10, 11, 99 };
        std::vector inputs{
            primitive(1, 0, 3, 0, 3),
            primitive(2, 10, 3, 3, 3),
        };
        const auto result = buildGeometryArena(GeometryArenaAbiVersion, 100,
            indices, inputs);
        CHECK(!result.valid());
        CHECK(!result.data.has_value());
        CHECK(result.error ==
            GeometryArenaBuildError::IndexOutsidePrimitiveVertices);
        CHECK(result.errorPrimitive == inputs[1].identity);
        return true;
    }

    bool versionAndIdentityFailuresAreExplicit() {
        const std::vector<uint32_t> indices{ 0 };
        auto input = primitive(1, 0, 1, 0, 1);
        CHECK(buildGeometryArena(GeometryArenaAbiVersion + 1, 1,
            indices, std::span(&input, 1)).error ==
            GeometryArenaBuildError::InvalidAbiVersion);
        input.identity = {};
        CHECK(buildGeometryArena(GeometryArenaAbiVersion, 1,
            indices, std::span(&input, 1)).error ==
            GeometryArenaBuildError::NilPrimitiveIdentity);
        return true;
    }
}

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "eligible indices become local UInt16",
            eligibleIndicesBecomeLocalUInt16 },
        { "wide local index remains UInt32", wideLocalIndexRemainsUInt32 },
        { "mixed streams are identity deterministic",
            mixedStreamsAreIdentityDeterministic },
        { "invalid primitive rejects whole build",
            invalidPrimitiveRejectsWholeBuild },
        { "version and identity failures are explicit",
            versionAndIdentityFailuresAreExplicit },
    };
    size_t passed = 0;
    for (const Test& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        std::cout << "[PASS] " << test.name << '\n';
        ++passed;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return 0;
}
