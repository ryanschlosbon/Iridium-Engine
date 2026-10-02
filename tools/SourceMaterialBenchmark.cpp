#include "material/SourceMaterial.h"
#include "profiling/CpuAllocationProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::json;

    struct Sample {
        double milliseconds = 0.0;
        Iridium::CpuAllocationFrameSample allocations;
    };

    std::vector<Sample> measure(const std::filesystem::path& path, size_t expectedMaterials) {
        std::vector<Sample> samples;
        for (size_t iteration = 0; iteration < 12; ++iteration) {
            Iridium::beginCpuAllocationFrame();
            const auto start = Clock::now();
            const Iridium::SourceMaterialDocument document = Iridium::importGltfSourceMaterials(path);
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            if (document.hasErrors() || document.materials().size() != expectedMaterials) {
                throw std::runtime_error("benchmark source did not import as expected");
            }
            if (iteration >= 2) samples.push_back({
                std::chrono::duration<double, std::milli>(end - start).count(), allocations });
        }
        return samples;
    }

    Json summarize(std::vector<Sample> samples) {
        std::sort(samples.begin(), samples.end(), [](const Sample& lhs, const Sample& rhs) {
            return lhs.milliseconds < rhs.milliseconds;
        });
        const size_t medianIndex = samples.size() / 2;
        const size_t p95Index = std::min(samples.size() - 1,
            static_cast<size_t>(samples.size() * 0.95));
        const uint64_t allocations = std::accumulate(samples.begin(), samples.end(), uint64_t{ 0 },
            [](uint64_t sum, const Sample& sample) { return sum + sample.allocations.allocationCount; }) /
            samples.size();
        const uint64_t bytes = std::accumulate(samples.begin(), samples.end(), uint64_t{ 0 },
            [](uint64_t sum, const Sample& sample) { return sum + sample.allocations.requestedBytes; }) /
            samples.size();
        return {
            { "iterations", samples.size() },
            { "median_ms", samples[medianIndex].milliseconds },
            { "p95_ms", samples[p95Index].milliseconds },
            { "mean_global_new_calls", allocations },
            { "mean_global_new_requested_bytes", bytes },
        };
    }

} // namespace

int main() {
    try {
        Json root = { { "asset", { { "version", "2.0" } } },
            { "materials", Json::array() } };
        for (size_t index = 0; index < 10'000; ++index) {
            root["materials"].push_back({
                { "name", "factor-" + std::to_string(index) },
                { "pbrMetallicRoughness", {
                    { "baseColorFactor", { 0.8, 0.4, 0.2, 1.0 } },
                    { "metallicFactor", 0.25 }, { "roughnessFactor", 0.6 } } }
            });
        }
        const std::filesystem::path factorPath = std::filesystem::temp_directory_path() /
            "iridium_m2_1_factor_10000.gltf";
        { std::ofstream output(factorPath, std::ios::binary); output << root.dump(); }

        Json report;
        report["factor_only_10000"] = summarize(measure(factorPath, 10'000));
        std::error_code error;
        std::filesystem::remove(factorPath, error);

        const std::filesystem::path carPath = std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "models" / "alfa_romeo" / "alfa_romeo.gltf";
        if (std::filesystem::exists(carPath)) report["sample_car_87"] = summarize(measure(carPath, 87));
        else report["sample_car_87"] = { { "skipped", "licensed source absent" } };
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
