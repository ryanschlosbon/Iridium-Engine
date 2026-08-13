#include "profiling/CpuAllocationProfile.h"
#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/runtime/CookedScene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    constexpr size_t kLightCount = 1'000;
    constexpr uint32_t kWarmups = 2;
    constexpr uint32_t kSamples = 15;

    [[nodiscard]] std::string uuid(size_t index) {
        std::array<uint8_t, 10> random{};
        uint64_t value = static_cast<uint64_t>(index + 1);
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            random[byte] = static_cast<uint8_t>(value >> (byte * 8u));
        }
        return Iridium::SceneEntityUuid::fromUuidV7Fields(
            1'775'000'100'000ull + index, random).toString();
    }

    [[nodiscard]] std::string sourceScene(uint32_t lightVersion) {
        Json scene{
            { "format", "iridium.scene" },
            { "schemaVersion", 1 },
            { "name", lightVersion == 1 ? "M5.1 Light v1" : "M5.1 Light v2" },
            { "entities", Json::array() },
        };
        for (size_t index = 0; index < kLightCount; ++index) {
            const int32_t type = static_cast<int32_t>(index % 3);
            const float intensity = 1'000.0f + static_cast<float>(index);
            Json data;
            if (lightVersion == 1) {
                data = {
                    { "type", type }, { "color", { 0.25f, 0.5f, 1.0f } },
                    { "intensity", intensity }, { "range", 25.0f },
                    { "radius", 0.1f }, { "innerCone", 15.0f },
                    { "outerCone", 35.0f }, { "castsShadows", true },
                };
            }
            else {
                data = {
                    { "type", type },
                    { "colorLinearRec709", { 0.25f, 0.5f, 1.0f } },
                    { "illuminanceLux", type == 0 ? intensity : 1.0f },
                    { "luminousIntensityCandela", type == 0 ? 1.0f : intensity },
                    { "rangeMeters", 25.0f },
                    { "sourceRadiusMeters", 0.1f },
                    { "innerConeDegrees", 15.0f },
                    { "outerConeDegrees", 35.0f },
                    { "castsShadows", true }, { "shadowQuality", 2 },
                    { "priority", 0 },
                };
            }
            scene["entities"].push_back({
                { "uuid", uuid(index) },
                { "components", Json::array({
                    Json{
                        { "id", "iridium.component.relationship" },
                        { "version", 1 },
                        { "data", {
                            { "parent", nullptr },
                            { "siblingOrder", static_cast<int32_t>(index) },
                        } },
                    },
                    Json{
                        { "id", "iridium.component.light" },
                        { "version", lightVersion },
                        { "data", std::move(data) },
                    },
                }) },
            });
        }
        return scene.dump();
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

    template <typename Work>
    [[nodiscard]] Json measure(std::string name, Work&& work) {
        for (uint32_t index = 0; index < kWarmups; ++index) work();
        std::vector<double> durationMs;
        std::vector<double> allocationCount;
        std::vector<double> allocationBytes;
        uint64_t checksum = 0;
        for (uint32_t index = 0; index < kSamples; ++index) {
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const uint64_t current = work();
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            if (index == 0) checksum = current;
            else if (checksum != current) {
                throw std::runtime_error(name + " checksum changed");
            }
            durationMs.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocationCount.push_back(static_cast<double>(
                allocations.allocationCount));
            allocationBytes.push_back(static_cast<double>(
                allocations.requestedBytes));
        }
        return {
            { "name", std::move(name) }, { "warmups", kWarmups },
            { "samples", kSamples }, { "durationMs", statistics(durationMs) },
            { "allocationCount", statistics(allocationCount) },
            { "allocationBytes", statistics(allocationBytes) },
            { "checksum", checksum },
        };
    }

} // namespace

int main(int argc, char** argv) {
    try {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        if (!registries) throw std::runtime_error(registries.diagnostic);
        const std::string v1 = sourceScene(1);
        const std::string v2 = sourceScene(2);
        const auto current = Iridium::readSourceSceneSchema1(
            v2, registries.runtime, registries.source);
        if (!current) throw std::runtime_error("Could not prepare v2 document");

        Json workloads = Json::array();
        workloads.push_back(measure("read-v1-and-migrate", [&] {
            const auto result = Iridium::readSourceSceneSchema1(
                v1, registries.runtime, registries.source);
            if (!result) throw std::runtime_error("v1 migration failed");
            return static_cast<uint64_t>(result.document->entities.size() +
                result.diagnostics.size());
        }));
        workloads.push_back(measure("read-v2-current", [&] {
            const auto result = Iridium::readSourceSceneSchema1(
                v2, registries.runtime, registries.source);
            if (!result) throw std::runtime_error("v2 read failed");
            return static_cast<uint64_t>(result.document->entities.size() +
                result.diagnostics.size());
        }));

        size_t cookedBytes = 0;
        workloads.push_back(measure("stage-and-cook-v2", [&] {
            auto staged = Iridium::stageSourceScene(
                *current.document, registries.runtime, registries.source);
            if (!staged) throw std::runtime_error("v2 staging failed");
            const auto compiled = Iridium::compileCookedScene(*staged.staging,
                registries.runtime, registries.source, {
                    .sceneAssetGuid = *Iridium::AssetGuid::parse(
                        "01890f4c-0510-7000-8000-000000000100"),
                    .sourceContentHash = std::string(64, 'a'),
                    .canonicalContentHash = std::string(64, 'b'),
                    .target = { .platform = "windows-x64", .profile = "release",
                        .qualityPolicy = "high" },
                });
            if (!compiled) throw std::runtime_error("v2 cook failed");
            const auto blob = Iridium::serializeCookedArtifact(*compiled.artifact);
            cookedBytes = blob.bytes.size();
            return static_cast<uint64_t>(blob.bytes.size());
        }));

        const auto migrated = Iridium::readSourceSceneSchema1(
            v1, registries.runtime, registries.source);
        const auto canonical = Iridium::writeSourceSceneCanonical(
            *migrated.document, registries.runtime, registries.source);
        Json report{
            { "schema", "iridium.m5.1.light-component-v2-benchmark.v1" },
            { "configuration", IRIDIUM_BENCHMARK_CONFIGURATION },
            { "compiler", IRIDIUM_BENCHMARK_COMPILER },
            { "lightCount", kLightCount },
            { "v1SourceBytes", v1.size() }, { "v2SourceBytes", v2.size() },
            { "migratedCanonicalBytes", canonical.bytes->size() },
            { "cookedArtifactBytes", cookedBytes },
            { "runtimeManifestHash",
                Iridium::runtimeComponentManifestHash(registries.runtime) },
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
