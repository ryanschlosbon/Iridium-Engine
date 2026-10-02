#include "material/MaterialCompiler.h"
#include "profiling/CpuAllocationProfile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <initializer_list>
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

    SourceMaterialExtension extension(std::string name,
        std::initializer_list<SourceExtensionProperty> properties = {}) {
        SourceMaterialExtension result{};
        result.name = std::move(name);
        result.supportedByM2 = true;
        result.canonicalValues = "{}";
        result.properties.assign(properties.begin(), properties.end());
        return result;
    }

    size_t compiledBytes(const CompiledMaterial& material) {
        size_t bytes = sizeof(material) + material.sourceName.capacity() + material.contentHash.capacity() +
            material.textureOperations.capacity() * sizeof(CompiledTextureOperation) +
            material.complexLobes.capacity() * sizeof(ComplexLobeRecord);
        for (const CompiledTextureOperation& texture : material.textureOperations)
            bytes += texture.imageIdentity.capacity() + texture.channels.capacity();
        for (const ComplexLobeRecord& lobe : material.complexLobes)
            bytes += lobe.sourceExtension.capacity();
        return bytes;
    }

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

    Json measure(const std::vector<SourceMaterial>& sources) {
        std::vector<Sample> samples;
        Json retained;
        for (size_t iteration = 0; iteration < 12; ++iteration) {
            beginCpuAllocationFrame();
            const auto start = Clock::now();
            std::vector<MaterialCompileResult> results;
            results.reserve(sources.size());
            for (const SourceMaterial& source : sources)
                results.push_back(compileSourceMaterial(source));
            const auto end = Clock::now();
            const CpuAllocationFrameSample allocations = endCpuAllocationFrame();
            if (!std::all_of(results.begin(), results.end(),
                [](const MaterialCompileResult& result) { return result.succeeded(); }))
                throw std::runtime_error("benchmark material compilation failed");
            if (iteration == 2) {
                for (const MaterialCompileResult& result : results) {
                    const std::string key = materialClosureClassName(result.material->closureClass);
                    if (!retained[key].is_object()) retained[key] = Json::object();
                    retained[key]["count"] = retained[key].value("count", 0u) + 1u;
                    retained[key]["estimated_retained_bytes"] =
                        retained[key].value("estimated_retained_bytes", uint64_t{ 0 }) +
                        compiledBytes(*result.material);
                }
            }
            if (iteration >= 2) samples.push_back({
                std::chrono::duration<double, std::milli>(end - start).count(), allocations });
        }
        Json report = summarize(std::move(samples));
        report["compiled_classes"] = std::move(retained);
        report["source_record_inline_bytes"] = sizeof(SourceMaterial);
        report["compiled_record_inline_bytes"] = sizeof(CompiledMaterial);
        return report;
    }

} // namespace

int main() {
    try {
        std::vector<SourceMaterial> sources;
        sources.reserve(10'000);
        for (size_t index = 0; index < 10'000; ++index) {
            SourceMaterial material{};
            material.localIndex = static_cast<uint32_t>(index);
            material.metallicRoughness.baseColorFactor.value = { 0.8f, 0.4f, 0.2f, 1.0f };
            material.metallicRoughness.metallicFactor.value = 0.25f;
            material.metallicRoughness.roughnessFactor.value = 0.6f;
            if (index >= 8'000 && index < 9'500)
                material.extensions.push_back(extension("KHR_materials_clearcoat", {
                    { "clearcoatFactor", "1.0", SourceValueOrigin::Authored },
                    { "clearcoatRoughnessFactor", "0.2", SourceValueOrigin::Authored } }));
            else if (index >= 9'500)
                material.extensions.push_back(extension("KHR_materials_unlit"));
            sources.push_back(std::move(material));
        }
        Json report;
        report["mixed_10000"] = measure(sources);

        const std::filesystem::path carPath = std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "models" / "alfa_romeo" / "alfa_romeo.gltf";
        if (std::filesystem::exists(carPath)) {
            const SourceMaterialDocument document = importGltfSourceMaterials(carPath);
            std::vector<SourceMaterial> car(document.materials().begin(), document.materials().end());
            report["sample_car_87"] = measure(car);
        }
        else report["sample_car_87"] = { { "skipped", "licensed source absent" } };
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
