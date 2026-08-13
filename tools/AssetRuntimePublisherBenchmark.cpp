#include "assets/runtime/AssetRuntimePublisher.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;
    using Clock = std::chrono::steady_clock;

    AssetGuid benchmarkGuid(uint32_t index) {
        AssetGuid::Bytes bytes{};
        bytes[0] = 1;
        bytes[12] = static_cast<uint8_t>(
            index >> 24u);
        bytes[13] = static_cast<uint8_t>(
            index >> 16u);
        bytes[14] = static_cast<uint8_t>(
            index >> 8u);
        bytes[15] = static_cast<uint8_t>(index);
        return AssetGuid(bytes);
    }

    uint64_t percentile(
        std::vector<uint64_t> samples,
        double quantile) {
        std::ranges::sort(samples);
        const size_t index = static_cast<size_t>(
            quantile *
            static_cast<double>(samples.size() - 1));
        return samples[index];
    }

} // namespace

int main() {
    constexpr uint32_t RuntimeRecords = 100'000;
    constexpr uint32_t Samples = 10'000;
    AssetRuntimePublisher publisher;
    for (uint32_t index = 0;
        index < RuntimeRecords; ++index) {
        publisher.setPinned(
            benchmarkGuid(index), false);
    }

    std::vector<uint64_t> idle;
    idle.reserve(Samples);
    for (uint32_t sample = 0;
        sample < Samples; ++sample) {
        const auto start = Clock::now();
        const RuntimePublishTickResult result =
            publisher.tick(1u << 20u);
        const auto end = Clock::now();
        if (result.queuedAfterTick != 0) return 2;
        idle.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    end - start).count()));
    }

    std::vector<uint64_t> scheduling;
    scheduling.reserve(Samples);
    for (uint32_t sample = 0;
        sample < Samples; ++sample) {
        const AssetGuid asset =
            benchmarkGuid(sample);
        const auto start = Clock::now();
        publisher.enqueue({
            .assetGuid = asset,
            .cookKey =
                "revision-" + std::to_string(sample),
            .estimatedUploadBytes = 4096,
            .publish = [] {
                return RuntimeAssetPublishOutcome{
                    .succeeded = true,
                    .cpuResidentBytes = 1024,
                    .gpuResidentBytes = 4096,
                };
            },
        });
        const RuntimePublishTickResult result =
            publisher.tick(4096);
        const auto end = Clock::now();
        if (result.published != 1) return 3;
        scheduling.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    end - start).count()));
    }

    std::cout
        << "{\"runtime_records\":" << RuntimeRecords
        << ",\"samples\":" << Samples
        << ",\"idle_p99_ns\":"
        << percentile(idle, 0.99)
        << ",\"schedule_publish_p99_ns\":"
        << percentile(scheduling, 0.99)
        << ",\"resident\":" <<
            publisher.stats().resident
        << ",\"queued\":" <<
            publisher.stats().queued
        << "}\n";
    return 0;
}
