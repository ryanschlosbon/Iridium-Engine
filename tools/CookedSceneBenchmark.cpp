#include "assets/cooker/CookedArtifact.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "ecs/Registry.h"
#include "profiling/CpuAllocationProfile.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/runtime/CookedComponentIO.h"
#include "scene/runtime/CookedScene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#endif

#ifndef IRIDIUM_BENCHMARK_CONFIGURATION
#define IRIDIUM_BENCHMARK_CONFIGURATION "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_COMPILER
#define IRIDIUM_BENCHMARK_COMPILER "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_SOURCE_COMMIT
#define IRIDIUM_BENCHMARK_SOURCE_COMMIT "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_SOURCE_DIRTY
#define IRIDIUM_BENCHMARK_SOURCE_DIRTY 1
#endif

namespace {

    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::ordered_json;

    struct BenchmarkComponent {
        std::string name;
        float value = 0.0f;
        void OnInspector() {}
    };

    struct MemorySample {
        uint64_t workingSet = 0;
        uint64_t peakWorkingSet = 0;
        uint64_t privateBytes = 0;
    };

    MemorySample memorySample() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                sizeof(counters))) {
            return { static_cast<uint64_t>(counters.WorkingSetSize),
                static_cast<uint64_t>(counters.PeakWorkingSetSize),
                static_cast<uint64_t>(counters.PrivateUsage) };
        }
#endif
        return {};
    }

    uint64_t positiveDelta(uint64_t after, uint64_t before) {
        return after > before ? after - before : 0;
    }

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }

    bool validate(const Registry& registry, Entity entity) {
        const auto* pool = registry.findPool<BenchmarkComponent>();
        return pool && pool->has(entity) &&
            std::isfinite(pool->get(entity).value);
    }

    bool encode(const Registry& registry, Entity entity,
        Iridium::CookedComponentWriter& writer) {
        const auto* pool = registry.findPool<BenchmarkComponent>();
        if (!pool || !pool->has(entity)) return false;
        const BenchmarkComponent& value = pool->get(entity);
        return writer.writeString(value.name) &&
            writer.writeFloat32(value.value);
    }

    bool decode(Registry& registry, Entity entity,
        Iridium::CookedComponentReader& reader) {
        std::string name;
        float value = 0.0f;
        if (!reader.readString(name) || !reader.readFloat32(value) ||
            !reader.finish()) return false;
        registry.addComponent<BenchmarkComponent>(entity,
            BenchmarkComponent{ std::move(name), value });
        return true;
    }

    bool serializeSource(const Registry& registry, Entity entity,
        const Iridium::SceneIdentityMap&, Iridium::SourceJson& data,
        std::string&) {
        const auto* pool = registry.findPool<BenchmarkComponent>();
        if (!pool || !pool->has(entity)) { data = nullptr; return true; }
        data = { { "name", pool->get(entity).name },
            { "value", pool->get(entity).value } };
        return true;
    }

    bool deserializeSource(Registry& registry, Entity entity,
        const Iridium::SourceJson& data, std::string&) {
        registry.addComponent<BenchmarkComponent>(entity,
            BenchmarkComponent{ data.at("name").get<std::string>(),
                data.at("value").get<float>() });
        return true;
    }

    bool validateSource(const Iridium::SourceJson& data, std::string& error) {
        if (data.is_object() && data.contains("name") &&
            data.contains("value")) return true;
        error = "Benchmark component is invalid";
        return false;
    }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries createRegistries() {
        Registries result;
        const auto componentId = *Iridium::ComponentTypeId::parse(
            "studio.benchmark.component");
        if (!result.runtime.add({
                .id = componentId,
                .cookedSectionId = *Iridium::CookedSectionId::parse("BNM1"),
                .currentCookedVersion = 1,
                .properties = {
                    { .id = *Iridium::PropertyId::parse("name"),
                        .valueType = Iridium::PropertyValueType::String,
                        .serializationOrder = 0, .required = true },
                    { .id = *Iridium::PropertyId::parse("value"),
                        .valueType = Iridium::PropertyValueType::Float32,
                        .serializationOrder = 1, .required = true },
                },
                .resolveReferences = resolve,
                .postLoadValidate = validate,
                .encodeCooked = encode,
                .decodeCooked = decode,
            }) || !result.runtime.freezeAndValidate()) {
            throw std::runtime_error("Could not create benchmark runtime registry");
        }
        if (!result.source.add({
                .componentId = componentId,
                .currentSourceVersion = 1,
                .sourceOrder = 0,
                .properties = {
                    { *Iridium::PropertyId::parse("name"), "name" },
                    { *Iridium::PropertyId::parse("value"), "value" },
                },
                .serializeSource = serializeSource,
                .deserializeLocal = deserializeSource,
                .validateLocal = validateSource,
            }) || !result.source.freezeAndValidate(result.runtime)) {
            throw std::runtime_error("Could not create benchmark source registry");
        }
        return result;
    }

    Iridium::SceneEntityUuid entityUuid(uint64_t index) {
        std::array<uint8_t, 10> random{};
        for (size_t byte = 0; byte < 8; ++byte) {
            random[byte] = static_cast<uint8_t>(index >> (byte * 8));
        }
        random[8] = 0x4d;
        random[9] = 0x34;
        return Iridium::SceneEntityUuid::fromUuidV7Fields(
            1'775'000'000'000ull + index, random);
    }

    Iridium::AssetGuid sceneGuid() {
        return *Iridium::AssetGuid::parse(
            "019fb7d3-0300-7000-8000-000000004400");
    }

    Iridium::SourceSceneDocument makeDocument(size_t entityCount) {
        const auto componentId = *Iridium::ComponentTypeId::parse(
            "studio.benchmark.component");
        Iridium::SourceSceneDocument result;
        result.name = "M4.4 cooked benchmark";
        result.entities.reserve(entityCount);
        for (size_t index = 0; index < entityCount; ++index) {
            Iridium::SourceSceneEntity entity;
            entity.uuid = entityUuid(index + 1);
            entity.components.push_back({
                .id = componentId,
                .version = 1,
                .data = { { "name", "Entity" },
                    { "value", static_cast<float>(index % 1024) } },
                .known = true,
            });
            result.entities.push_back(std::move(entity));
        }
        return result;
    }

    Iridium::CookedSceneCompileInput compileInput() {
        return {
            .sceneAssetGuid = sceneGuid(),
            .sourceContentHash = std::string(64, 'a'),
            .canonicalContentHash = std::string(64, 'b'),
            .target = { .platform = "windows-x64", .profile = "release",
                .qualityPolicy = "high" },
        };
    }

    struct WorkResult {
        uint64_t checksum = 0;
        uint64_t artifactBytes = 0;
        std::shared_ptr<void> keepAlive;
    };

    Json statistics(std::vector<double> values) {
        std::ranges::sort(values);
        const auto percentile = [&](double fraction) {
            const size_t index = static_cast<size_t>(std::ceil(
                fraction * static_cast<double>(values.size() - 1)));
            return values[index];
        };
        return { { "min", values.front() }, { "median", percentile(0.5) },
            { "p95", percentile(0.95) }, { "p99", percentile(0.99) },
            { "max", values.back() } };
    }

    template <typename Work>
    Json measure(std::string name, size_t entityCount, uint32_t sampleCount,
        Work&& work) {
        (void)work();
        std::vector<double> durationMs;
        std::vector<double> allocationCount;
        std::vector<double> requestedBytes;
        std::vector<double> workingSetDelta;
        std::vector<double> privateDelta;
        uint64_t checksum = 0;
        uint64_t artifactBytes = 0;
        uint64_t peakWorkingSet = 0;
        for (uint32_t sample = 0; sample < sampleCount; ++sample) {
            const MemorySample before = memorySample();
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            WorkResult current = work();
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            const MemorySample after = memorySample();
            if (sample == 0) {
                checksum = current.checksum;
                artifactBytes = current.artifactBytes;
            }
            else if (checksum != current.checksum ||
                artifactBytes != current.artifactBytes) {
                throw std::runtime_error(name + " result changed between samples");
            }
            durationMs.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocationCount.push_back(static_cast<double>(
                allocations.allocationCount));
            requestedBytes.push_back(static_cast<double>(allocations.requestedBytes));
            workingSetDelta.push_back(static_cast<double>(positiveDelta(
                after.workingSet, before.workingSet)));
            privateDelta.push_back(static_cast<double>(positiveDelta(
                after.privateBytes, before.privateBytes)));
            peakWorkingSet = (std::max)(peakWorkingSet, after.peakWorkingSet);
        }
        return {
            { "name", std::move(name) },
            { "entityCount", entityCount },
            { "sampleCount", sampleCount },
            { "checksum", checksum },
            { "artifactBytes", artifactBytes },
            { "artifactBytesPerEntity", entityCount == 0 ? 0.0 :
                static_cast<double>(artifactBytes) / entityCount },
            { "durationMs", statistics(std::move(durationMs)) },
            { "allocationCount", statistics(std::move(allocationCount)) },
            { "requestedAllocationBytes", statistics(std::move(requestedBytes)) },
            { "workingSetDeltaBytes", statistics(std::move(workingSetDelta)) },
            { "privateDeltaBytes", statistics(std::move(privateDelta)) },
            { "peakWorkingSetBytes", peakWorkingSet },
        };
    }

    template <typename Setup, typename Work>
    Json measurePrepared(std::string name, size_t entityCount,
        uint32_t sampleCount, uint64_t artifactBytes,
        Setup&& setup, Work&& work) {
        {
            auto prepared = setup();
            (void)work(prepared);
        }
        std::vector<double> durationMs;
        std::vector<double> allocationCount;
        std::vector<double> requestedBytes;
        std::vector<double> workingSetDelta;
        std::vector<double> privateDelta;
        uint64_t checksum = 0;
        uint64_t peakWorkingSet = 0;
        for (uint32_t sample = 0; sample < sampleCount; ++sample) {
            auto prepared = setup();
            const MemorySample before = memorySample();
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const uint64_t currentChecksum = work(prepared);
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            const MemorySample after = memorySample();
            if (sample == 0) checksum = currentChecksum;
            else if (checksum != currentChecksum) {
                throw std::runtime_error(name + " result changed between samples");
            }
            durationMs.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocationCount.push_back(static_cast<double>(
                allocations.allocationCount));
            requestedBytes.push_back(static_cast<double>(allocations.requestedBytes));
            workingSetDelta.push_back(static_cast<double>(positiveDelta(
                after.workingSet, before.workingSet)));
            privateDelta.push_back(static_cast<double>(positiveDelta(
                after.privateBytes, before.privateBytes)));
            peakWorkingSet = (std::max)(peakWorkingSet, after.peakWorkingSet);
        }
        return {
            { "name", std::move(name) },
            { "entityCount", entityCount },
            { "sampleCount", sampleCount },
            { "checksum", checksum },
            { "artifactBytes", artifactBytes },
            { "artifactBytesPerEntity", entityCount == 0 ? 0.0 :
                static_cast<double>(artifactBytes) / entityCount },
            { "durationMs", statistics(std::move(durationMs)) },
            { "allocationCount", statistics(std::move(allocationCount)) },
            { "requestedAllocationBytes", statistics(std::move(requestedBytes)) },
            { "workingSetDeltaBytes", statistics(std::move(workingSetDelta)) },
            { "privateDeltaBytes", statistics(std::move(privateDelta)) },
            { "peakWorkingSetBytes", peakWorkingSet },
        };
    }

    struct Options {
        std::filesystem::path output = "cooked-scene-benchmark.json";
        size_t entities = 1000;
        uint32_t samples = 5;
    };

    Options options(int argc, char** argv) {
        Options result;
        for (int index = 1; index < argc; ++index) {
            if (index + 1 >= argc) throw std::runtime_error("Missing option value");
            const std::string_view option(argv[index]);
            const std::string_view value(argv[++index]);
            if (option == "--output") result.output = value;
            else if (option == "--entities") result.entities = std::stoull(
                std::string(value));
            else if (option == "--samples") result.samples = static_cast<uint32_t>(
                std::stoul(std::string(value)));
            else throw std::runtime_error("Unknown benchmark option");
        }
        if (result.entities == 0 || result.samples == 0) {
            throw std::runtime_error("Entities and samples must be nonzero");
        }
        result.output = std::filesystem::absolute(result.output);
        return result;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Options command = options(argc, argv);
        Registries registries = createRegistries();
        auto source = Iridium::stageSourceScene(makeDocument(command.entities),
            registries.runtime, registries.source);
        if (!source) throw std::runtime_error("Could not stage benchmark source");
        auto baselineCompile = Iridium::compileCookedScene(*source.staging,
            registries.runtime, registries.source, compileInput());
        if (!baselineCompile) throw std::runtime_error(
            "Could not compile benchmark scene");
        const auto baselineBlob = std::make_shared<Iridium::CookedArtifactBlob>(
            Iridium::serializeCookedArtifact(*baselineCompile.artifact));

        const std::filesystem::path ddcRoot = command.output.parent_path() /
            (".m4-cooked-ddc-" + std::to_string(command.entities));
        std::error_code filesystemError;
        std::filesystem::remove_all(ddcRoot, filesystemError);
        Iridium::LocalDerivedDataCache cache(ddcRoot);
        const auto storeDiagnostics = cache.storeAtomic(
            baselineCompile.artifact->cookKey, *baselineBlob);
        if (Iridium::hasCookErrors(storeDiagnostics)) {
            throw std::runtime_error("Could not prepare benchmark DDC entry");
        }

        Json results = Json::array();
        results.push_back(measure("cold_source_compile", command.entities,
            command.samples, [&] {
                auto compiled = Iridium::compileCookedScene(*source.staging,
                    registries.runtime, registries.source, compileInput());
                if (!compiled) throw std::runtime_error("Scene compile failed");
                auto blob = std::make_shared<Iridium::CookedArtifactBlob>(
                    Iridium::serializeCookedArtifact(*compiled.artifact));
                return WorkResult{ blob->bytes.size(), blob->bytes.size(), blob };
            }));
        results.push_back(measure("warm_ddc_read", command.entities,
            command.samples, [&] {
                auto read = std::make_shared<Iridium::DdcReadResult>(cache.read(
                    baselineCompile.artifact->cookKey, false));
                if (read->status != Iridium::DdcLookupStatus::Hit || !read->blob) {
                    throw std::runtime_error("Warm DDC read missed");
                }
                return WorkResult{ read->blob->bytes.size(),
                    read->blob->bytes.size(), read };
            }));
        results.push_back(measure("artifact_read_validate", command.entities,
            command.samples, [&] {
                auto read = std::make_shared<Iridium::CookedArtifactReadResult>(
                    Iridium::readCookedArtifact(baselineBlob->bytes,
                        baselineBlob->artifactHash));
                if (!read->valid()) throw std::runtime_error(
                    "Artifact validation failed");
                return WorkResult{ read->artifact->sections.size(),
                    baselineBlob->bytes.size(), read };
            }));
        results.push_back(measure("cpu_ready_stage", command.entities,
            command.samples, [&] {
                auto staged = std::make_shared<Iridium::CookedSceneStageResult>(
                    Iridium::stageCookedScene(baselineBlob->bytes,
                        registries.runtime, {
                            .expectedCookKey = baselineCompile.artifact->cookKey,
                            .expectedArtifactHash = baselineBlob->artifactHash,
                        }));
                if (!*staged) throw std::runtime_error("Runtime stage failed");
                return WorkResult{ staged->staging->world->registry().aliveCount(),
                    baselineBlob->bytes.size(), staged };
            }));
        using CommitState = std::pair<Iridium::SceneWorld,
            std::unique_ptr<Iridium::StagedCookedScene>>;
        results.push_back(measurePrepared("active_scene_commit", command.entities,
            command.samples, baselineBlob->bytes.size(), [&] {
                auto staged = Iridium::stageCookedScene(baselineBlob->bytes,
                    registries.runtime);
                if (!staged) throw std::runtime_error("Commit setup failed");
                auto result = std::make_shared<CommitState>();
                result->second = std::move(staged.staging);
                return result;
            }, [&](std::shared_ptr<CommitState>& state) {
                Iridium::commitStagedCookedScene(state->first, *state->second);
                return static_cast<uint64_t>(
                    state->first.registry().aliveCount());
            }));

        Json report{
            { "schema", "IridiumCookedSceneBenchmark/v1" },
            { "configuration", IRIDIUM_BENCHMARK_CONFIGURATION },
            { "compiler", IRIDIUM_BENCHMARK_COMPILER },
            { "sourceCommit", IRIDIUM_BENCHMARK_SOURCE_COMMIT },
            { "sourceDirty", IRIDIUM_BENCHMARK_SOURCE_DIRTY != 0 },
            { "logicalProcessorCount", std::thread::hardware_concurrency() },
            { "allocationScope", "C++ global new/new[] requested bytes" },
            { "entityCount", command.entities },
            { "results", std::move(results) },
        };
        std::filesystem::create_directories(command.output.parent_path(),
            filesystemError);
        std::ofstream output(command.output, std::ios::binary | std::ios::trunc);
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("Could not write benchmark output");
        std::filesystem::remove_all(ddcRoot, filesystemError);
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
