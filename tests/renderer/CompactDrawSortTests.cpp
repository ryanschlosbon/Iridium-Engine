// M7R R5c.3: the compact (key, index) sorts must reproduce the packet sorts
// they replace byte for byte, including the order of ties that the non-total
// opaque comparator leaves to std::sort. Each case sorts one copy of a queue
// with the original packet comparator and another with the compact helper,
// then compares all 240 bytes of every packet.

#include "renderer/rhi/CompactDrawSort.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <iostream>
#include <random>
#include <vector>

namespace {
    using namespace Iridium;

    // The comparator at the opaque and forward-opaque call sites
    // (Application.cpp, "cpu.render.sort.opaque"/".forward_opaque").
    bool opaquePacketLess(const DrawPacket& a, const DrawPacket& b) {
        if (a.opaqueSortKey != b.opaqueSortKey) return a.opaqueSortKey < b.opaqueSortKey;
        if (a.geometry != b.geometry) return a.geometry < b.geometry;
        return a.firstIndex < b.firstIndex;
    }

    enum class Pattern { Random, Sorted, Reversed, Equal, OrganPipe, Sawtooth };

    struct Shape {
        uint32_t keyValues;   // distinct values per key field (ties when small)
        Pattern pattern;
    };

    DrawPacket basePacket(uint32_t serial) {
        DrawPacket packet{};
        // A unique payload so a wrong permutation is always visible.
        packet.worldTransform = glm::mat4(1.0f);
        packet.worldTransform[3] = glm::vec4(static_cast<float>(serial), 0.0f, 0.0f, 1.0f);
        packet.instanceCount = serial;
        packet.firstInstanceTransform = serial * 3u;
        return packet;
    }

    uint32_t patterned(std::mt19937_64& random, Pattern pattern, size_t index,
        size_t count, uint32_t values) {
        switch (pattern) {
        case Pattern::Random: return static_cast<uint32_t>(random() % values);
        case Pattern::Sorted:
            return static_cast<uint32_t>((index * values) / (count + 1));
        case Pattern::Reversed:
            return static_cast<uint32_t>(((count - index) * values) / (count + 1));
        case Pattern::Equal: return 0u;
        case Pattern::OrganPipe:
            return static_cast<uint32_t>(((index < count / 2 ? index : count - index) *
                values) / (count + 1));
        case Pattern::Sawtooth: return static_cast<uint32_t>(index % values);
        }
        return 0u;
    }

    std::vector<DrawPacket> opaqueQueue(std::mt19937_64& random, size_t count,
        Shape shape) {
        std::vector<DrawPacket> queue;
        queue.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            DrawPacket packet = basePacket(static_cast<uint32_t>(index));
            const uint32_t value = patterned(random, shape.pattern, index, count,
                shape.keyValues);
            packet.opaqueSortKey = (static_cast<uint64_t>(value % 5u) << 32u) |
                (value / 5u);
            packet.geometry = GeometryHandle{ static_cast<uint32_t>(
                random() % (1 + shape.keyValues / 4)) };
            packet.firstIndex = static_cast<uint32_t>(random() % 3) * 36u;
            queue.push_back(packet);
        }
        return queue;
    }

    float depthValue(std::mt19937_64& random, uint32_t values) {
        static constexpr float special[] = { 0.0f, -0.0f, 0.1f, 1.0f, 1.0f,
            2.5f, 1.0e30f, 0.5f };
        if (random() % 4 == 0) return special[random() % std::size(special)];
        return 0.25f * static_cast<float>(random() % (values + 1));
    }

    std::vector<DrawPacket> transparentQueue(std::mt19937_64& random, size_t count,
        Shape shape, bool mixedModes) {
        std::vector<DrawPacket> queue;
        queue.reserve(count);
        std::vector<TransparentWorkIdentity> identities;
        for (size_t index = 0; index < count; ++index) {
            DrawPacket packet = basePacket(static_cast<uint32_t>(index));
            const uint32_t value = patterned(random, shape.pattern, index, count,
                shape.keyValues);
            packet.transparency.priority = static_cast<int32_t>(value % 3u) - 1;
            uint32_t flags = 0;
            if (random() % 5 != 0) flags |= TransparentWorkIntervalValid;
            if (random() % 4 == 0) flags |= TransparentWorkCameraIntersecting;
            if (random() % 6 == 0) flags |= TransparentWorkMirrored;
            packet.transparentWorkFlags = flags;
            packet.transparentFarDepth = depthValue(random, shape.keyValues);
            packet.transparentNearDepth = depthValue(random, shape.keyValues);
            packet.distanceToCamera = depthValue(random, shape.keyValues);
            packet.pipeline = PipelineHandle{ static_cast<uint32_t>(random() % 3) };
            packet.material = MaterialHandle{ static_cast<uint32_t>(random() % 3) };
            packet.geometry = GeometryHandle{ static_cast<uint32_t>(random() % 3) };
            packet.transparencyExecutionMode = mixedModes && random() % 3 == 0
                ? TransparencyExecutionMode::LegacyTwoBucket
                : TransparencyExecutionMode::Classified;
            // Identities: mostly distinct, some repeated (full ties).
            if (!identities.empty() && random() % 8 == 0) {
                const auto& repeated = identities[random() % identities.size()];
                packet.owner = repeated.owner;
                packet.sourcePrimitiveGuid = repeated.sourcePrimitiveGuid;
                packet.primitiveGuid = repeated.primitiveGuid;
                packet.materialGuid = repeated.materialGuid;
            } else {
                SceneEntityUuid::Bytes owner{};
                owner[0] = static_cast<uint8_t>(random() % 4);
                owner[15] = static_cast<uint8_t>(random());
                AssetGuid::Bytes primitive{};
                primitive[3] = static_cast<uint8_t>(random() % 4);
                primitive[9] = static_cast<uint8_t>(random());
                packet.owner = SceneEntityUuid(owner);
                packet.sourcePrimitiveGuid = AssetGuid(primitive);
                primitive[12] = static_cast<uint8_t>(random());
                packet.primitiveGuid = AssetGuid(primitive);
                primitive[1] = static_cast<uint8_t>(random() % 2);
                packet.materialGuid = AssetGuid(primitive);
                identities.push_back(transparentWorkIdentity(packet));
            }
            queue.push_back(packet);
        }
        return queue;
    }

    bool samePackets(const std::vector<DrawPacket>& expected,
        const std::vector<DrawPacket>& actual) {
        return expected.size() == actual.size() && (expected.empty() ||
            std::memcmp(expected.data(), actual.data(),
                expected.size() * sizeof(DrawPacket)) == 0);
    }

    constexpr size_t kSizes[] = { 0, 1, 2, 3, 16, 31, 32, 33, 64, 100, 257,
        1000, 3904, 6464, 20000 };
    constexpr Shape kShapes[] = {
        { 2, Pattern::Random }, { 7, Pattern::Random }, { 1000, Pattern::Random },
        { 40, Pattern::Sorted }, { 40, Pattern::Reversed }, { 1, Pattern::Equal },
        { 40, Pattern::OrganPipe }, { 9, Pattern::Sawtooth },
    };

    bool opaquePermutationIsIdentical() {
        std::mt19937_64 random(11);
        CompactDrawSortScratch scratch;
        size_t cases = 0;
        for (const size_t size : kSizes) {
            for (const Shape shape : kShapes) {
                for (uint32_t repeat = 0; repeat < 3; ++repeat) {
                    const auto queue = opaqueQueue(random, size, shape);
                    auto expected = queue;
                    std::sort(expected.begin(), expected.end(), opaquePacketLess);
                    auto actual = queue;
                    sortOpaqueDrawPackets(actual, scratch);
                    if (!samePackets(expected, actual)) {
                        std::cerr << "opaque permutation differs: size " << size
                            << " values " << shape.keyValues << '\n';
                        return false;
                    }
                    ++cases;
                }
            }
        }
        std::cout << "  opaque: " << cases << " queues identical\n";
        return true;
    }

    bool transparentWorkPermutationIsIdentical() {
        std::mt19937_64 random(23);
        CompactDrawSortScratch scratch;
        size_t cases = 0;
        for (const size_t size : kSizes) {
            for (const Shape shape : kShapes) {
                for (uint32_t repeat = 0; repeat < 3; ++repeat) {
                    const auto queue = transparentQueue(random, size, shape, false);
                    auto expected = queue;
                    std::sort(expected.begin(), expected.end(), transparentWorkLess);
                    auto actual = queue;
                    sortTransparentWorkDrawPackets(actual, scratch);
                    if (!samePackets(expected, actual)) {
                        std::cerr << "transparent work permutation differs: size "
                            << size << " values " << shape.keyValues << '\n';
                        return false;
                    }
                    ++cases;
                }
            }
        }
        std::cout << "  transparent work: " << cases << " queues identical\n";
        return true;
    }

    bool transparentCompatibilityPermutationIsIdentical() {
        std::mt19937_64 random(37);
        CompactDrawSortScratch scratch;
        size_t cases = 0;
        for (const size_t size : kSizes) {
            for (const Shape shape : kShapes) {
                for (uint32_t repeat = 0; repeat < 3; ++repeat) {
                    const auto queue = transparentQueue(random, size, shape, true);
                    auto expected = queue;
                    std::sort(expected.begin(), expected.end(),
                        transparentCompatibilityLess);
                    auto actual = queue;
                    sortTransparentCompatibilityDrawPackets(actual, scratch);
                    if (!samePackets(expected, actual)) {
                        std::cerr << "transparent compatibility permutation differs: size "
                            << size << " values " << shape.keyValues << '\n';
                        return false;
                    }
                    ++cases;
                }
            }
        }
        std::cout << "  transparent compatibility: " << cases << " queues identical\n";
        return true;
    }

    bool scratchIsReusedWithoutGrowth() {
        std::mt19937_64 random(5);
        CompactDrawSortScratch scratch;
        auto queue = opaqueQueue(random, 500, { 7, Pattern::Random });
        sortOpaqueDrawPackets(queue, scratch);
        const auto* keys = scratch.opaqueKeys.data();
        auto smaller = opaqueQueue(random, 300, { 7, Pattern::Random });
        sortOpaqueDrawPackets(smaller, scratch);
        return scratch.opaqueKeys.data() == keys;
    }

    // Indicative only (not timing evidence): F7-sized queues, best of 51.
    void printTiming() {
        using Clock = std::chrono::steady_clock;
        const auto median = [](auto&& run) {
            std::vector<double> samples;
            for (int repeat = 0; repeat < 51; ++repeat) {
                const auto start = Clock::now();
                run();
                samples.push_back(std::chrono::duration<double, std::micro>(
                    Clock::now() - start).count());
            }
            // The minimum: least disturbed by other load on the machine.
            return *std::ranges::min_element(samples);
        };
        std::mt19937_64 random(3);
        const auto opaque = opaqueQueue(random, 6464, { 400, Pattern::Random });
        auto transparent = transparentQueue(random, 3904,
            { 1000, Pattern::Random }, false);
        // Scene-like intervals: continuous depths, so identity ties are rare.
        std::uniform_real_distribution<float> depth(0.1f, 500.0f);
        for (DrawPacket& packet : transparent) {
            packet.transparentWorkFlags = TransparentWorkIntervalValid;
            packet.transparentNearDepth = depth(random);
            packet.transparentFarDepth = packet.transparentNearDepth + depth(random);
        }
        CompactDrawSortScratch scratch;
        std::vector<DrawPacket> work;
        const double opaquePacket = median([&] {
            work = opaque; std::sort(work.begin(), work.end(), opaquePacketLess); });
        const double opaqueCompact = median([&] {
            work = opaque; sortOpaqueDrawPackets(work, scratch); });
        const double copyOpaque = median([&] { work = opaque; });
        const double transparentPacket = median([&] {
            work = transparent;
            std::sort(work.begin(), work.end(), transparentWorkLess); });
        const double transparentCompact = median([&] {
            work = transparent; sortTransparentWorkDrawPackets(work, scratch); });
        const double copyTransparent = median([&] { work = transparent; });
        std::cout << "opaque 6464: packet sort " << opaquePacket - copyOpaque
            << " us, compact " << opaqueCompact - copyOpaque << " us\n"
            << "transparent 3904: packet sort " << transparentPacket - copyTransparent
            << " us, compact " << transparentCompact - copyTransparent << " us\n";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--timing") {
        printTiming();
        return 0;
    }
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "opaque compact sort keeps the packet permutation",
            opaquePermutationIsIdentical },
        { "transparent work compact sort keeps the packet permutation",
            transparentWorkPermutationIsIdentical },
        { "transparent compatibility compact sort keeps the packet permutation",
            transparentCompatibilityPermutationIsIdentical },
        { "compact sort scratch is reused", scratchIsReusedWithoutGrowth },
    };
    size_t passed = 0;
    for (const Test& test : tests) {
        if (!test.run()) { std::cerr << "[FAIL] " << test.name << '\n'; return 1; }
        std::cout << "[PASS] " << test.name << '\n'; ++passed;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return 0;
}
