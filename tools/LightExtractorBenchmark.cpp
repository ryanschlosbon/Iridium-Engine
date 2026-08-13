#include "ecs/systems/TransformSystem.h"
#include "profiling/CpuAllocationProfile.h"
#include "renderer/lighting/LightExtractor.h"
#include "scene/components/LightComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef IRIDIUM_BENCHMARK_CONFIGURATION
#define IRIDIUM_BENCHMARK_CONFIGURATION "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_COMPILER
#define IRIDIUM_BENCHMARK_COMPILER "unknown"
#endif

namespace {

    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::ordered_json;
    constexpr uint32_t kWarmups = 3;
    constexpr uint32_t kSamples = 21;

    [[nodiscard]] Iridium::SceneEntityUuid uuid(uint32_t index) {
        std::array<uint8_t, 10> random{};
        const uint64_t value = static_cast<uint64_t>(index) + 1;
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            random[byte] = static_cast<uint8_t>(value >> (byte * 8u));
        }
        return Iridium::SceneEntityUuid::fromUuidV7Fields(
            1'775'000'200'000ull + index, random);
    }

    [[nodiscard]] Json statistics(std::vector<double> values) {
        std::ranges::sort(values);
        const auto percentile = [&](double fraction) {
            return values[static_cast<size_t>(std::ceil(
                fraction * static_cast<double>(values.size() - 1)))];
        };
        return {
            { "min", values.front() }, { "median", percentile(0.50) },
            { "p95", percentile(0.95) }, { "p99", percentile(0.99) },
            { "max", values.back() },
        };
    }

    class Workload final {
    public:
        Workload(uint32_t lightCount, uint32_t changePermille)
            : lightCount_(lightCount),
              changedCount_(changePermille == 0 ? 0u :
                  (std::max)(1u, static_cast<uint32_t>(
                      (static_cast<uint64_t>(lightCount) * changePermille) /
                      1'000u))),
              extractor_({ .initialCapacity = (std::min)(
                    Iridium::kInitialGpuLightCapacity, lightCount),
                  .maximumCapacity = Iridium::kMaximumGpuLightCapacity }) {
            changedEntities_.reserve(changedCount_);
            for (uint32_t index = 0; index < lightCount; ++index) {
                const Entity entity = world_.createEntity(uuid(index));
                auto& transform = world_.registry().addComponent<TransformComponent>(
                    entity);
                transform.position = {
                    static_cast<float>(index % 256u),
                    static_cast<float>((index / 256u) % 256u),
                    static_cast<float>(index / 65'536u),
                };
                world_.registry().addComponent<RelationshipComponent>(entity)
                    .siblingOrder = static_cast<int32_t>(index);
                auto& light = world_.registry().addComponent<LightComponent>(entity);
                light.type = static_cast<LightType>(index % 3u);
                light.colorLinearRec709 = { 1.0f, 0.5f, 0.25f };
                light.illuminanceLux = 100'000.0f;
                light.luminousIntensityCandela = 1'250.0f;
                light.rangeMeters = 25.0f;
            }
            TransformSystem transforms;
            (void)transforms.update(world_.registry());
            if (changedCount_ != 0) {
                const auto* lights = world_.registry().getPool<LightComponent>();
                for (uint32_t change = 0; change < changedCount_; ++change) {
                    const size_t index = static_cast<size_t>(
                        (static_cast<uint64_t>(change) * lightCount_) /
                        changedCount_);
                    changedEntities_.push_back(lights->entities[index]);
                }
            }
            (void)extractor_.extract(world_);
        }

        [[nodiscard]] uint64_t run() {
            toggle_ = !toggle_;
            for (Entity entity : changedEntities_) {
                auto& light = world_.registry().getComponent<LightComponent>(entity);
                light.illuminanceLux = toggle_ ? 110'000.0f : 100'000.0f;
                light.luminousIntensityCandela = toggle_ ? 1'500.0f : 1'250.0f;
            }
            const Iridium::LightingFramePacket packet = extractor_.extract(world_);
            if (packet.stats.activeLightCount != lightCount_ ||
                packet.stats.changedRecordCount != changedCount_) {
                throw std::runtime_error("Light extraction benchmark count mismatch: active=" +
                    std::to_string(packet.stats.activeLightCount) + "/" +
                    std::to_string(lightCount_) + ", changed=" +
                    std::to_string(packet.stats.changedRecordCount) + "/" +
                    std::to_string(changedCount_));
            }
            lastChangedBytes_ = packet.stats.changedRecordBytes;
            lastChangedRanges_ = packet.stats.changedRangeCount;
            capacity_ = packet.stats.capacity;
            return (static_cast<uint64_t>(packet.stats.activeLightCount) << 32u) |
                packet.stats.changedRecordCount;
        }

        [[nodiscard]] uint32_t changedCount() const noexcept {
            return changedCount_;
        }
        [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] uint64_t changedBytes() const noexcept {
            return lastChangedBytes_;
        }
        [[nodiscard]] uint32_t changedRanges() const noexcept {
            return lastChangedRanges_;
        }

    private:
        uint32_t lightCount_ = 0;
        uint32_t changedCount_ = 0;
        Iridium::SceneWorld world_;
        Iridium::LightExtractor extractor_;
        std::vector<Entity> changedEntities_;
        bool toggle_ = false;
        uint32_t capacity_ = 0;
        uint64_t lastChangedBytes_ = 0;
        uint32_t lastChangedRanges_ = 0;
    };

    [[nodiscard]] Json measure(uint32_t lightCount, uint32_t changePermille) {
        Workload workload(lightCount, changePermille);
        for (uint32_t index = 0; index < kWarmups; ++index) (void)workload.run();
        std::vector<double> durationMs;
        std::vector<double> allocationCount;
        std::vector<double> allocationBytes;
        uint64_t checksum = 0;
        for (uint32_t index = 0; index < kSamples; ++index) {
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const uint64_t value = workload.run();
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            if (index == 0) checksum = value;
            else if (checksum != value) {
                throw std::runtime_error("Light extraction checksum changed");
            }
            durationMs.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocationCount.push_back(static_cast<double>(
                allocations.allocationCount));
            allocationBytes.push_back(static_cast<double>(
                allocations.requestedBytes));
        }
        const uint64_t cpuRecordBytes = static_cast<uint64_t>(
            workload.capacity()) * (sizeof(Iridium::PackedGpuLight) +
                sizeof(uint64_t));
        const uint64_t gpuRecordBytes = static_cast<uint64_t>(
            workload.capacity()) * sizeof(Iridium::PackedGpuLight) * 2u;
        return {
            { "lightCount", lightCount },
            { "changePermille", changePermille },
            { "changedRecordCount", workload.changedCount() },
            { "changedRecordBytes", workload.changedBytes() },
            { "changedRangeCount", workload.changedRanges() },
            { "capacity", workload.capacity() },
            { "cpuRecordAndRevisionBytes", cpuRecordBytes },
            { "gpuRecordBytesTwoFrames", gpuRecordBytes },
            { "warmups", kWarmups }, { "samples", kSamples },
            { "durationMs", statistics(durationMs) },
            { "allocationCount", statistics(allocationCount) },
            { "allocationBytes", statistics(allocationBytes) },
            { "checksum", checksum },
        };
    }

} // namespace

int main(int argc, char** argv) {
    try {
        Json workloads = Json::array();
        for (const uint32_t count : { 1u, 256u, 4'096u, 65'536u }) {
            for (const uint32_t change : { 0u, 10u, 100u, 1'000u }) {
                workloads.push_back(measure(count, change));
            }
        }
        Json report{
            { "schema", "iridium.m5.2.light-extraction-benchmark.v1" },
            { "configuration", IRIDIUM_BENCHMARK_CONFIGURATION },
            { "compiler", IRIDIUM_BENCHMARK_COMPILER },
            { "gpuFrameContexts", 2 },
            { "packedGpuLightBytes", sizeof(Iridium::PackedGpuLight) },
            { "workloads", std::move(workloads) },
        };
        const std::string bytes = report.dump(2) + '\n';
        if (argc > 1) {
            const std::filesystem::path path(argv[1]);
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) throw std::runtime_error("Could not write report");
        }
        std::cout << bytes;
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
