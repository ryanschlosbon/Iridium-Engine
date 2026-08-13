#include "material/MaterialRuntime.h"
#include "profiling/CpuAllocationProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::json;
    using namespace Iridium;

    struct Sample {
        double milliseconds = 0.0;
        CpuAllocationFrameSample allocations;
    };

    Json summarize(std::vector<Sample> samples) {
        std::sort(samples.begin(), samples.end(), [](const Sample& lhs, const Sample& rhs) {
            return lhs.milliseconds < rhs.milliseconds;
        });
        const uint64_t allocationCount = std::accumulate(samples.begin(), samples.end(), uint64_t{ 0 },
            [](uint64_t sum, const Sample& sample) { return sum + sample.allocations.allocationCount; }) /
            samples.size();
        const uint64_t allocationBytes = std::accumulate(samples.begin(), samples.end(), uint64_t{ 0 },
            [](uint64_t sum, const Sample& sample) { return sum + sample.allocations.requestedBytes; }) /
            samples.size();
        return {
            { "iterations", samples.size() },
            { "median_ms", samples[samples.size() / 2].milliseconds },
            { "p95_ms", samples.back().milliseconds },
            { "mean_global_new_calls", allocationCount },
            { "mean_global_new_requested_bytes", allocationBytes },
        };
    }

    std::shared_ptr<const CompiledMaterial> makeCompiledMaterial() {
        SourceMaterial source{};
        source.name = "runtime-benchmark";
        source.metallicRoughness.baseColorFactor.value = { 0.8f, 0.4f, 0.2f, 1.0f };
        source.metallicRoughness.metallicFactor.value = 0.25f;
        source.metallicRoughness.roughnessFactor.value = 0.6f;
        MaterialCompileResult result = compileSourceMaterial(source);
        if (!result.succeeded()) throw std::runtime_error("benchmark material compilation failed");
        return result.material;
    }

} // namespace

int main() {
    try {
        constexpr size_t InstanceCount = 10'000;
        constexpr size_t ChangedCount = 100;
        const auto compiled = makeCompiledMaterial();

        std::vector<MaterialInstance> instances;
        instances.reserve(InstanceCount);
        for (size_t index = 0; index < InstanceCount; ++index) {
            instances.emplace_back(compiled);
            instances.back().setRoughness(0.1f + 0.8f *
                static_cast<float>(index % 101) / 100.0f);
        }
        std::vector<PackedGpuMaterial> packed(InstanceCount);
        std::vector<Sample> fullSamples;
        for (size_t iteration = 0; iteration < 12; ++iteration) {
            beginCpuAllocationFrame();
            const auto start = Clock::now();
            for (size_t index = 0; index < instances.size(); ++index) {
                MaterialPackResult result = packMaterialInstance(instances[index], {});
                if (!result.succeeded()) throw std::runtime_error("benchmark packing failed");
                packed[index] = *result.material;
            }
            const auto end = Clock::now();
            const CpuAllocationFrameSample allocations = endCpuAllocationFrame();
            if (iteration >= 2) fullSamples.push_back({
                std::chrono::duration<double, std::milli>(end - start).count(), allocations });
        }

        MaterialInstanceStore store(InstanceCount);
        std::vector<MaterialInstanceHandle> handles;
        handles.reserve(InstanceCount);
        for (size_t index = 0; index < InstanceCount; ++index)
            handles.push_back(store.create(compiled));
        std::vector<MaterialUpload> uploads;
        uploads.reserve(InstanceCount);
        const MaterialUploadStats initial = store.collectChanged({}, uploads);
        if (initial.changedInstances != InstanceCount)
            throw std::runtime_error("initial upload count mismatch");

        std::vector<Sample> changedSamples;
        uint64_t changedBytes = 0;
        for (size_t iteration = 0; iteration < 12; ++iteration) {
            const float roughness = (iteration & 1u) ? 0.25f : 0.75f;
            for (size_t index = 0; index < ChangedCount; ++index)
                store.get(handles[index])->setRoughness(roughness);
            beginCpuAllocationFrame();
            const auto start = Clock::now();
            const MaterialUploadStats changed = store.collectChanged({}, uploads);
            const auto end = Clock::now();
            const CpuAllocationFrameSample allocations = endCpuAllocationFrame();
            if (changed.changedInstances != ChangedCount)
                throw std::runtime_error("changed-only upload count mismatch");
            if (iteration >= 2) {
                changedBytes = changed.uploadedBytes;
                changedSamples.push_back({
                    std::chrono::duration<double, std::milli>(end - start).count(), allocations });
            }
        }

        beginCpuAllocationFrame();
        const auto unchangedStart = Clock::now();
        const MaterialUploadStats unchanged = store.collectChanged({}, uploads);
        const auto unchangedEnd = Clock::now();
        const CpuAllocationFrameSample unchangedAllocations = endCpuAllocationFrame();

        Json report;
        report["schema_version"] = PackedGpuMaterial::SchemaVersion;
        report["packed_record_bytes"] = sizeof(PackedGpuMaterial);
        report["instance_inline_bytes"] = sizeof(MaterialInstance);
        report["full_pack_10000"] = summarize(std::move(fullSamples));
        report["full_pack_10000"]["uploaded_bytes"] = initial.uploadedBytes;
        report["changed_only_100_of_10000"] = summarize(std::move(changedSamples));
        report["changed_only_100_of_10000"]["uploaded_bytes"] = changedBytes;
        report["unchanged_scan_10000"] = {
            { "milliseconds", std::chrono::duration<double, std::milli>(
                unchangedEnd - unchangedStart).count() },
            { "changed_instances", unchanged.changedInstances },
            { "uploaded_bytes", unchanged.uploadedBytes },
            { "global_new_calls", unchangedAllocations.allocationCount },
            { "global_new_requested_bytes", unchangedAllocations.requestedBytes },
        };
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
