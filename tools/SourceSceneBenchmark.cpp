#include "profiling/CpuAllocationProfile.h"
#include "scene/authoring/CoreComponentCodecs.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneEnvelopeMigrator.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/authoring/SourceSceneCapture.h"
#include "scene/authoring/AtomicSourceSceneFile.h"
#include "scene/runtime/CoreComponentRegistry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#endif

#ifndef IRIDIUM_BENCHMARK_CONFIGURATION
#define IRIDIUM_BENCHMARK_CONFIGURATION "unknown"
#endif
#ifndef IRIDIUM_BENCHMARK_COMPILER
#define IRIDIUM_BENCHMARK_COMPILER "unknown"
#endif

namespace {

    using Clock = std::chrono::steady_clock;
    using json = nlohmann::ordered_json;
    constexpr uint32_t kWarmups = 1;
    constexpr uint32_t kSamples = 5;
    constexpr std::array<size_t, 3> kEntityCounts{ 1'000, 10'000, 100'000 };
    constexpr std::array<uint8_t, 16> kSceneAssetGuid{
        0x01, 0x9f, 0xb7, 0xd3, 0x01, 0x00, 0x70, 0x00,
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xbb,
    };

    struct Memory { uint64_t working = 0; uint64_t privateBytes = 0; };
    Memory memory() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
            return { static_cast<uint64_t>(counters.WorkingSetSize),
                static_cast<uint64_t>(counters.PrivateUsage) };
        }
#endif
        return {};
    }

    uint64_t delta(uint64_t after, uint64_t before) {
        return after > before ? after - before : 0;
    }

    struct WorkOutcome {
        uint64_t checksum = 0;
        Memory liveMemory;
    };

    struct CommitPreparation {
        std::unique_ptr<Iridium::StagedSourceScene> staging;
        std::unique_ptr<Iridium::SceneWorld> active;
    };

    json stats(std::vector<double> values) {
        std::ranges::sort(values);
        const auto at = [&](double percentile) {
            return values[static_cast<size_t>(std::ceil(
                percentile * static_cast<double>(values.size() - 1)))];
        };
        return { { "min", values.front() }, { "median", at(0.5) },
            { "p95", at(0.95) }, { "max", values.back() } };
    }

    template<typename Work>
    json measure(std::string_view name, size_t count, Work&& work) {
        for (uint32_t index = 0; index < kWarmups; ++index) (void)work();
        std::vector<double> durations;
        std::vector<double> allocations;
        std::vector<double> requestedBytes;
        std::vector<double> workingDelta;
        std::vector<double> privateDelta;
        uint64_t checksum = 0;
        for (uint32_t index = 0; index < kSamples; ++index) {
            const Memory before = memory();
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const WorkOutcome outcome = work();
            const auto end = Clock::now();
            const auto allocation = Iridium::endCpuAllocationFrame();
            if (index == 0) checksum = outcome.checksum;
            else if (checksum != outcome.checksum) throw std::runtime_error(
                std::string(name) + " checksum changed between samples");
            durations.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocations.push_back(static_cast<double>(allocation.allocationCount));
            requestedBytes.push_back(static_cast<double>(allocation.requestedBytes));
            workingDelta.push_back(static_cast<double>(delta(
                outcome.liveMemory.working, before.working)));
            privateDelta.push_back(static_cast<double>(delta(
                outcome.liveMemory.privateBytes, before.privateBytes)));
        }
        return {
            { "name", name }, { "entityCount", count },
            { "warmupSamples", kWarmups }, { "measuredSamples", kSamples },
            { "durationMs", stats(std::move(durations)) },
            { "allocationCount", stats(std::move(allocations)) },
            { "requestedAllocationBytes", stats(std::move(requestedBytes)) },
            { "liveWorkingSetDeltaBytes", stats(std::move(workingDelta)) },
            { "livePrivateDeltaBytes", stats(std::move(privateDelta)) },
            { "checksum", checksum },
        };
    }

    template<typename Prepare, typename Work>
    json measurePrepared(std::string_view name, size_t count,
        Prepare&& prepare, Work&& work) {
        for (uint32_t index = 0; index < kWarmups; ++index) {
            auto prepared = prepare();
            (void)work(prepared);
        }
        std::vector<double> durations;
        std::vector<double> allocations;
        std::vector<double> requestedBytes;
        std::vector<double> workingDelta;
        std::vector<double> privateDelta;
        uint64_t checksum = 0;
        for (uint32_t index = 0; index < kSamples; ++index) {
            auto prepared = prepare();
            const Memory before = memory();
            Iridium::beginCpuAllocationFrame();
            const auto begin = Clock::now();
            const WorkOutcome outcome = work(prepared);
            const auto end = Clock::now();
            const auto allocation = Iridium::endCpuAllocationFrame();
            if (index == 0) checksum = outcome.checksum;
            else if (checksum != outcome.checksum) throw std::runtime_error(
                std::string(name) + " checksum changed between samples");
            durations.push_back(std::chrono::duration<double, std::milli>(
                end - begin).count());
            allocations.push_back(static_cast<double>(allocation.allocationCount));
            requestedBytes.push_back(static_cast<double>(allocation.requestedBytes));
            workingDelta.push_back(static_cast<double>(delta(
                outcome.liveMemory.working, before.working)));
            privateDelta.push_back(static_cast<double>(delta(
                outcome.liveMemory.privateBytes, before.privateBytes)));
        }
        return {
            { "name", name }, { "entityCount", count },
            { "warmupSamples", kWarmups }, { "measuredSamples", kSamples },
            { "durationMs", stats(std::move(durations)) },
            { "allocationCount", stats(std::move(allocations)) },
            { "requestedAllocationBytes", stats(std::move(requestedBytes)) },
            { "liveWorkingSetDeltaBytes", stats(std::move(workingDelta)) },
            { "livePrivateDeltaBytes", stats(std::move(privateDelta)) },
            { "checksum", checksum },
        };
    }

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }
    bool validateRuntime(const Registry&, Entity) { return true; }
    bool encode(const Registry&, Entity, Iridium::CookedComponentWriter&) { return true; }
    bool decode(Registry&, Entity, Iridium::CookedComponentReader&) { return true; }
    bool serialize(const Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SourceJson&, std::string&) { return true; }
    bool deserialize(Registry&, Entity, const Iridium::SourceJson&, std::string&) { return true; }
    bool validateSource(const Iridium::SourceJson&, std::string&) { return true; }

    struct Registries {
        Iridium::RuntimeComponentRegistry runtime;
        Iridium::ComponentSerializerRegistry source;
    };

    Registries registries() {
        const Iridium::RuntimeComponentCallbacks runtimeCallbacks{
            resolve, validateRuntime, encode, decode };
        auto runtime = Iridium::createRuntimeSceneComponentRegistry({
            runtimeCallbacks, runtimeCallbacks, runtimeCallbacks,
            runtimeCallbacks, runtimeCallbacks });
        const Iridium::SourceComponentCallbacks sourceCallbacks{
            serialize, deserialize, validateSource };
        auto source = Iridium::createSourceComponentSerializerRegistry(
            runtime.registry, { sourceCallbacks, sourceCallbacks, sourceCallbacks,
                sourceCallbacks, sourceCallbacks });
        if (!runtime || !source) throw std::runtime_error("registry composition failed");
        return { std::move(runtime.registry), std::move(source.registry) };
    }

    std::string uuid(size_t index) {
        std::array<uint8_t, 10> random{};
        uint64_t value = static_cast<uint64_t>(index + 1);
        for (size_t byte = 0; byte < 8; ++byte) {
            random[byte] = static_cast<uint8_t>(value >> (byte * 8u));
        }
        return Iridium::SceneEntityUuid::fromUuidV7Fields(
            1'775'000'000'000ull + index, random).toString();
    }

    std::string schema1(size_t count) {
        std::string bytes;
        bytes.reserve(count * 300);
        bytes += "{\"format\":\"iridium.scene\",\"schemaVersion\":1,\"name\":\"Scale\",\"entities\":[";
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) bytes.push_back(',');
            bytes += "{\"uuid\":\"" + uuid(index) +
                "\",\"components\":[{\"id\":\"iridium.component.name\",\"version\":1,\"data\":{\"value\":\"Entity " +
                std::to_string(index) +
                "\"}},{\"id\":\"iridium.component.relationship\",\"version\":1,\"data\":{\"parent\":null,\"siblingOrder\":" +
                std::to_string(index) + "}}]}";
        }
        bytes += "]}";
        return bytes;
    }

    std::string schema0(size_t count) {
        std::string bytes;
        bytes.reserve(count * 100);
        bytes += "{\"Scene\":\"Scale v0\",\"Entities\":[";
        for (size_t index = 0; index < count; ++index) {
            if (index != 0) bytes.push_back(',');
            bytes += "{\"EntityID\":" + std::to_string(index) +
                ",\"Name\":\"Entity " + std::to_string(index) +
                "\",\"RelationshipComponent\":{\"parent\":null,\"siblingOrder\":" +
                std::to_string(index) + "}}";
        }
        bytes += "]}";
        return bytes;
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("source-scene-benchmark.json");
        std::vector<size_t> entityCounts(kEntityCounts.begin(), kEntityCounts.end());
        if (argc > 2) {
            const size_t requestedCount = std::stoull(argv[2]);
            if (requestedCount == 0) {
                throw std::runtime_error("entity count must be greater than zero");
            }
            entityCounts.assign(1, requestedCount);
        }
        Registries registry = registries();
        json results = json::array();
        for (size_t count : entityCounts) {
            const std::string current = schema1(count);
            const std::string legacy = schema0(count);
            auto parsed = Iridium::readSourceSceneSchema1(
                current, registry.runtime, registry.source);
            if (!parsed) throw std::runtime_error("benchmark setup parse failed");

            results.push_back(measure("parse_schema1", count, [&] {
                auto result = Iridium::readSourceSceneSchema1(
                    current, registry.runtime, registry.source);
                if (!result) throw std::runtime_error("parse failed");
                return WorkOutcome{
                    static_cast<uint64_t>(result.document->entities.size()), memory() };
            }));
            results.push_back(measure("serialize_schema1", count, [&] {
                auto result = Iridium::writeSourceSceneCanonical(
                    *parsed.document, registry.runtime, registry.source);
                if (!result) throw std::runtime_error("serialize failed");
                return WorkOutcome{
                    static_cast<uint64_t>(result.bytes->size()), memory() };
            }));
            results.push_back(measure("migrate_schema0_to_1", count, [&] {
                auto result = Iridium::migrateSourceSceneV0(legacy, kSceneAssetGuid);
                if (!result) throw std::runtime_error("migration failed");
                return WorkOutcome{
                    static_cast<uint64_t>(result.value->at("entities").size()), memory() };
            }));
            results.push_back(measure("stage_schema1", count, [&] {
                auto result = Iridium::stageSourceScene(*parsed.document,
                    registry.runtime, registry.source);
                if (!result) throw std::runtime_error("stage failed");
                return WorkOutcome{
                    static_cast<uint64_t>(result.staging->world->registry()
                        .aliveCount()), memory() };
            }));

            auto stagedWorld = Iridium::stageSourceScene(*parsed.document,
                registry.runtime, registry.source);
            if (!stagedWorld) throw std::runtime_error("capture setup stage failed");
            results.push_back(measure("capture_live_schema1", count, [&] {
                auto result = Iridium::captureSourceScene(
                    *stagedWorld.staging->world, *parsed.document,
                    registry.runtime, registry.source);
                if (!result) throw std::runtime_error("capture failed");
                return WorkOutcome{
                    static_cast<uint64_t>(result.document->entities.size()), memory() };
            }));

            results.push_back(measurePrepared("commit_staged_world", count,
                [&] {
                    auto result = Iridium::stageSourceScene(*parsed.document,
                        registry.runtime, registry.source);
                    if (!result) throw std::runtime_error("commit setup stage failed");
                    return CommitPreparation{
                        .staging = std::move(result.staging),
                        .active = std::make_unique<Iridium::SceneWorld>(),
                    };
                },
                [&](CommitPreparation& prepared) {
                    Iridium::commitStagedSourceScene(
                        *prepared.active, *prepared.staging);
                    return WorkOutcome{
                        static_cast<uint64_t>(prepared.active->registry().aliveCount()),
                        memory() };
                }));

            const auto canonical = Iridium::writeSourceSceneCanonical(
                *parsed.document, registry.runtime, registry.source);
            if (!canonical) throw std::runtime_error("atomic save setup failed");
            const std::filesystem::path atomicPath = output.parent_path() /
                (".iridium-m4.3-atomic-" + std::to_string(count) +
                    ".iridium.scene.json");
            const Iridium::SourceSceneFileVerifier verifier = [&](
                std::string_view bytes, std::string& diagnostic) {
                const auto verified = Iridium::readSourceSceneSchema1(bytes,
                    registry.runtime, registry.source);
                if (verified) return true;
                diagnostic = "benchmark semantic verification failed";
                return false;
            };
            results.push_back(measure("atomic_save_verified", count, [&] {
                const auto saved = Iridium::saveSourceSceneAtomically(
                    atomicPath, *canonical.bytes, verifier);
                if (!saved) throw std::runtime_error(
                    "atomic save failed: " + saved.diagnostic);
                return WorkOutcome{
                    static_cast<uint64_t>(canonical.bytes->size()), memory() };
            }));
            std::error_code cleanupError;
            std::filesystem::remove(atomicPath, cleanupError);
            std::filesystem::path backup = atomicPath;
            backup += ".bak";
            std::filesystem::remove(backup, cleanupError);
            std::cout << "measured " << count << " entities\n";
        }
        json report{
            { "schema", "IridiumSourceSceneBenchmark/v1" },
            { "configuration", IRIDIUM_BENCHMARK_CONFIGURATION },
            { "compiler", IRIDIUM_BENCHMARK_COMPILER },
            { "logicalProcessorCount", std::thread::hardware_concurrency() },
            { "allocationScope", "C++ global new/new[] requested bytes" },
            { "entityCounts", entityCounts },
            { "results", std::move(results) },
        };
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n';
        if (!stream) throw std::runtime_error("could not write benchmark report");
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
