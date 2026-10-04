// M7R R5c.7: the parallel transparent sorts must reproduce the serial compact
// sorts byte for byte, take the serial path whenever two keys are equivalent,
// report the serial interval count, and allocate nothing in steady frames.
// Each case sorts one copy of a queue with the serial helpers and another
// with TransparentDrawSorter on a real task system, then compares all 240
// bytes of every packet.

#include "extraction/ParallelDrawSort.h"

#include "core/tasks/TaskSystem.h"
#include "profiling/CpuAllocationProfile.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace {
    using namespace Iridium;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")" \
                      << std::endl; \
            std::_Exit(1); \
        } \
    } while (false)

    DrawPacket basePacket(uint32_t serial) {
        DrawPacket packet{};
        packet.worldTransform = glm::mat4(1.0f);
        packet.worldTransform[3] = glm::vec4(static_cast<float>(serial), 0.0f, 0.0f, 1.0f);
        packet.instanceCount = serial;
        packet.firstInstanceTransform = serial * 3u;
        return packet;
    }

    enum class Kind {
        // Continuous depths and distinct identities: no equivalent keys.
        SceneLike,
        // Few depth values, repeated identities, -0/+0: frequent ties.
        Ties,
        // Scene-like, with one duplicated packet (a single full tie).
        OneDuplicate,
    };

    std::vector<DrawPacket> transparentQueue(std::mt19937_64& random, size_t count,
        Kind kind, bool mixedModes) {
        std::vector<DrawPacket> queue;
        queue.reserve(count);
        std::uniform_real_distribution<float> depth(0.1f, 500.0f);
        for (size_t index = 0; index < count; ++index) {
            DrawPacket packet = basePacket(static_cast<uint32_t>(index));
            packet.transparency.priority = static_cast<int32_t>(random() % 3u) - 1;
            uint32_t flags = 0;
            if (random() % 7 != 0) flags |= TransparentWorkIntervalValid;
            if (random() % 9 == 0) flags |= TransparentWorkCameraIntersecting;
            packet.transparentWorkFlags = flags;
            if (kind == Kind::Ties) {
                static constexpr float values[] = { 0.0f, -0.0f, 1.0f, 2.5f };
                packet.transparentNearDepth = values[random() % 4];
                packet.transparentFarDepth = values[random() % 4];
                packet.distanceToCamera = values[random() % 4];
            }
            else {
                packet.transparentNearDepth = depth(random);
                packet.transparentFarDepth = packet.transparentNearDepth + depth(random);
                packet.distanceToCamera = depth(random);
            }
            packet.pipeline = PipelineHandle{ static_cast<uint32_t>(random() % 3) };
            packet.material = MaterialHandle{ static_cast<uint32_t>(random() % 3) };
            packet.geometry = GeometryHandle{ static_cast<uint32_t>(random() % 3) };
            packet.transparencyExecutionMode = mixedModes && random() % 3 == 0
                ? TransparencyExecutionMode::LegacyTwoBucket
                : TransparencyExecutionMode::Classified;
            SceneEntityUuid::Bytes owner{};
            const uint64_t serial = kind == Kind::Ties ? random() % 8u : index;
            for (uint32_t byte = 0; byte < 8; ++byte)
                owner[byte] = static_cast<uint8_t>(serial >> (8u * byte));
            AssetGuid::Bytes primitive{};
            primitive[3] = static_cast<uint8_t>(random() % 2);
            packet.owner = SceneEntityUuid(owner);
            packet.sourcePrimitiveGuid = AssetGuid(primitive);
            packet.primitiveGuid = AssetGuid(primitive);
            packet.materialGuid = AssetGuid(primitive);
            queue.push_back(packet);
        }
        if (kind == Kind::OneDuplicate && count > 10) {
            DrawPacket duplicate = queue[count / 3];
            queue[(2 * count) / 3] = duplicate;
        }
        return queue;
    }

    bool samePackets(const std::vector<DrawPacket>& expected,
        const std::vector<DrawPacket>& actual) {
        return expected.size() == actual.size() && (expected.empty() ||
            std::memcmp(expected.data(), actual.data(),
                expected.size() * sizeof(DrawPacket)) == 0);
    }

    uint64_t serialIntervals(const std::vector<DrawPacket>& sorted) {
        std::vector<TransparentIntervalEndpoint> endpoints(sorted.size());
        std::vector<float> nears(sorted.size());
        std::vector<uint32_t> fenwick(sorted.size() + 1u);
        return sweepAmbiguousTransparentIntervals(sorted, endpoints, nears, fenwick);
    }

    void matchesSerialSorts(Tasks::TaskSystem* tasks) {
        std::mt19937_64 random(97);
        TransparentDrawSorter sorter(tasks);
        CompactDrawSortScratch serialScratch;
        CompactDrawSortScratch scratch;
        size_t parallelWork = 0, parallelCompatibility = 0, fallbacks = 0;
        constexpr size_t sizes[] = { 0, 1, 2, 100, 2047, 2048, 2049, 3904,
            5000, 15616, 40000 };
        for (const size_t size : sizes) {
            for (const Kind kind : { Kind::SceneLike, Kind::Ties, Kind::OneDuplicate }) {
                for (const bool mixed : { false, true }) {
                    const auto work = transparentQueue(random, size, kind, false);
                    const auto compatibility = transparentQueue(random, size, kind, mixed);
                    auto expectedWork = work;
                    auto expectedCompatibility = compatibility;
                    sortTransparentWorkDrawPackets(expectedWork, serialScratch);
                    sortTransparentCompatibilityDrawPackets(expectedCompatibility,
                        serialScratch);
                    auto actualWork = work;
                    auto actualCompatibility = compatibility;
                    sorter.sort(actualCompatibility, actualWork, scratch);
                    CHECK(samePackets(expectedWork, actualWork));
                    CHECK(samePackets(expectedCompatibility, actualCompatibility));
                    CHECK(sorter.ambiguousIntervals(actualWork) ==
                        serialIntervals(expectedWork));
                    const auto& stats = sorter.stats();
                    if (tasks && size >= TransparentDrawSorter::ParallelSortMinimumPackets) {
                        CHECK(stats.sortedSurfaceParallel);
                        CHECK(stats.compatibilityParallel);
                    }
                    else {
                        CHECK(!stats.sortedSurfaceParallel);
                        CHECK(!stats.compatibilityParallel);
                    }
                    // Ties (and only ties) take the serial order.
                    if (stats.sortedSurfaceParallel && kind == Kind::SceneLike)
                        CHECK(!stats.sortedSurfaceTieFallback);
                    if (stats.sortedSurfaceParallel && kind != Kind::SceneLike)
                        CHECK(stats.sortedSurfaceTieFallback);
                    parallelWork += stats.sortedSurfaceParallel &&
                        !stats.sortedSurfaceTieFallback;
                    parallelCompatibility += stats.compatibilityParallel &&
                        !stats.compatibilityTieFallback;
                    fallbacks += stats.sortedSurfaceTieFallback +
                        stats.compatibilityTieFallback;
                }
            }
        }
        if (tasks) CHECK(parallelWork > 0 && parallelCompatibility > 0 && fallbacks > 0);
        std::cout << "  " << (tasks ? "task system" : "serial") << ": parallel work "
            << parallelWork << ", parallel compatibility " << parallelCompatibility
            << ", tie fallbacks " << fallbacks << '\n';
    }

    // Repeated frames of the same queue sizes allocate nothing (main thread
    // and frame-critical workers, as the engine counts them).
    void steadyFramesDoNotAllocate(Tasks::TaskSystem& tasks) {
        std::mt19937_64 random(5);
        TransparentDrawSorter sorter(&tasks);
        CompactDrawSortScratch scratch;
        const auto work = transparentQueue(random, 15616, Kind::SceneLike, false);
        const auto compatibility = transparentQueue(random, 3000, Kind::SceneLike, true);
        std::vector<DrawPacket> workQueue;
        std::vector<DrawPacket> compatibilityQueue;
        for (uint32_t frame = 0; frame < 8; ++frame) {
            // The engine overwrites its queues in place every frame.
            workQueue.resize(work.size());
            std::copy(work.begin(), work.end(), workQueue.begin());
            compatibilityQueue.resize(compatibility.size());
            std::copy(compatibility.begin(), compatibility.end(),
                compatibilityQueue.begin());
            const bool measured = frame >= 4;
            if (measured) beginCpuAllocationFrame();
            sorter.sort(compatibilityQueue, workQueue, scratch);
            (void)sorter.ambiguousIntervals(workQueue);
            if (measured) {
                const CpuAllocationFrameSample sample = endCpuAllocationFrame();
                CHECK(sample.allocationCount == 0);
            }
        }
        CHECK(sorter.stats().sortedSurfaceParallel);
        CHECK(!sorter.stats().sortedSurfaceTieFallback);
    }

} // namespace

int main() {
    std::cout << "serial path" << std::endl;
    matchesSerialSorts(nullptr);
    Tasks::TaskSystem tasks(Tasks::TaskSystemConfig{ .workerThreadCount = 7,
        .reservedFrameWorkers = 2, .pinnedIoThread = false });
    std::cout << "parallel path" << std::endl;
    matchesSerialSorts(&tasks);
    std::cout << "steady frames" << std::endl;
    steadyFramesDoNotAllocate(tasks);
    tasks.shutdown();
    std::cout << "ParallelDrawSortTests passed" << std::endl;
    return 0;
}
