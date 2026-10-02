#include "renderer/lighting/ClusteredLighting.h"

#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;

    struct Fixture {
        std::vector<Iridium::PackedGpuLight> records;
        std::vector<uint64_t> revisions;
        std::vector<uint32_t> activeSlots;
        std::vector<Iridium::LightSelectionMetadata> selection;

        [[nodiscard]] Iridium::LightingFramePacket packet() const {
            return {
                .records = records,
                .recordRevisions = revisions,
                .activeSlots = activeSlots,
                .selectionMetadata = selection,
                .requiredCapacity = static_cast<uint32_t>(records.size()),
                .stats = { .activeLightCount =
                    static_cast<uint32_t>(activeSlots.size()) },
            };
        }
    };

    Fixture makeFixture(uint32_t count) {
        Fixture result;
        result.records.resize(count);
        result.revisions.assign(count, 1);
        result.activeSlots.resize(count);
        result.selection.resize(count);
        std::iota(result.activeSlots.begin(), result.activeSlots.end(), 0u);
        for (uint32_t index = 0; index < count; ++index) {
            auto& light = result.records[index];
            const bool directional = index < 4;
            const float x = (static_cast<float>(index % 32u) - 15.5f) * 3.0f;
            const float y = (static_cast<float>((index / 32u) % 16u) - 7.5f) * 3.0f;
            const float depth = 6.0f + static_cast<float>(index % 64u) * 3.0f;
            light.positionRange = { x, y, -depth, directional ? 0.0f : 6.0f };
            light.directionOuterCos = { 0.0f, -1.0f, 0.0f, 0.8f };
            light.colorIntensity = { 1.0f, 0.7f, 0.35f,
                directional ? 50'000.0f : 1'000.0f };
            const uint32_t metadata = static_cast<uint32_t>(directional
                ? Iridium::PackedGpuLightType::Directional
                : Iridium::PackedGpuLightType::Point);
            light.shapeMetadata = { 0.05f, 0.0f,
                std::bit_cast<float>(metadata),
                std::bit_cast<float>(Iridium::kInvalidShadowDataSlot) };
            Iridium::SceneEntityUuid::Bytes bytes{};
            bytes[6] = 0x70;
            bytes[8] = 0x80;
            for (uint32_t byte = 0; byte < 4; ++byte) {
                bytes[15u - byte] = static_cast<uint8_t>(index >> (byte * 8u));
            }
            result.selection[index].owner = Iridium::SceneEntityUuid(bytes);
            result.selection[index].priority = static_cast<int32_t>(index % 5u);
            result.selection[index].castsShadows = (index % 7u) == 0;
        }
        return result;
    }

    uint64_t productBytes(const Iridium::ClusterGridConfig& config,
        const Iridium::ClusterGridDimensions& dimensions) {
        const uint64_t clusters = dimensions.clusterCount();
        return clusters * (sizeof(Iridium::ClusterLightHeader) +
                sizeof(uint32_t) * 2u) +
            static_cast<uint64_t>(config.maximumLightReferences) *
                sizeof(uint32_t) +
            Iridium::clusterScanScratchElementCount(clusters) * sizeof(uint32_t) +
            static_cast<uint64_t>(config.maximumDirectionalLights +
                config.maximumFallbackLights) * sizeof(uint32_t) + 64u + 32u;
    }

    uint64_t percentile(std::vector<uint64_t> samples, double fraction) {
        std::sort(samples.begin(), samples.end());
        const size_t index = static_cast<size_t>(fraction *
            static_cast<double>(samples.size() - 1));
        return samples[index];
    }

} // namespace

int main() {
    Iridium::ClusterFrameParameters frame{};
    frame.renderWidth = 3840;
    frame.renderHeight = 2160;
    frame.nearPlane = 0.1f;
    frame.farPlane = 1'000.0f;
    frame.projection = glm::perspectiveRH_ZO(glm::radians(60.0f),
        3840.0f / 2160.0f, frame.nearPlane, frame.farPlane);
    frame.projection[1][1] *= -1.0f;

    std::cout << "{\n  \"schema\": \"iridium.m5.cluster-cpu-benchmark.v1\",\n"
        "  \"extent\": [3840, 2160],\n  \"results\": [\n";
    bool first = true;
    volatile uint64_t checksum = 0;
    for (uint32_t tile : { 16u, 32u }) {
        for (uint32_t slices : { 24u, 32u }) {
            Iridium::ClusterGridConfig config{};
            config.tileWidth = config.tileHeight = tile;
            config.depthSlices = slices;
            const Iridium::ClusterGridDimensions dimensions =
                Iridium::clusterGridDimensions(config, frame);
            for (uint32_t lightCount : { 64u, 512u, 4096u }) {
                const Fixture fixture = makeFixture(lightCount);
                const Iridium::LightingFramePacket packet = fixture.packet();
                Iridium::ClusteredLightAssigner assigner(config);
                (void)assigner.build(packet, frame);
                const uint32_t sampleCount = lightCount <= 64 ? 7u :
                    (lightCount <= 512 ? 5u : 3u);
                std::vector<uint64_t> samples;
                samples.reserve(sampleCount);
                Iridium::ClusteredLightingProduct last;
                for (uint32_t sample = 0; sample < sampleCount; ++sample) {
                    const auto start = Clock::now();
                    last = assigner.build(packet, frame);
                    samples.push_back(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - start).count()));
                }
                checksum = checksum + last.stats.publishedLightReferences +
                    last.stats.fallbackLightCount;
                if (!first) std::cout << ",\n";
                first = false;
                std::cout << "    {\"tile\": " << tile
                    << ", \"slices\": " << slices
                    << ", \"lights\": " << lightCount
                    << ", \"clusters\": " << dimensions.clusterCount()
                    << ", \"bytes_per_frame\": "
                    << productBytes(config, dimensions)
                    << ", \"median_ns\": " << percentile(samples, 0.5)
                    << ", \"p95_ns\": " << percentile(samples, 0.95)
                    << ", \"references\": "
                    << last.stats.publishedLightReferences
                    << ", \"consumer_proxy_iterations\": "
                    << static_cast<uint64_t>(
                        last.stats.publishedLightReferences) * tile * tile
                    << ", \"max_occupancy\": "
                    << last.stats.maximumClusterOccupancy
                    << ", \"overflow\": "
                    << static_cast<uint32_t>(last.stats.overflow) << "}";
            }
        }
    }
    std::cout << "\n  ],\n  \"checksum\": " << checksum << "\n}\n";
    return 0;
}
