#include "assets/SqliteAssetCatalog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {

    using namespace Iridium;
    using Clock = std::chrono::steady_clock;

    AssetGuid benchmarkGuid(uint64_t index) {
        std::array<uint8_t, 10> random{};
        for (size_t byte = 0; byte < 8; ++byte) {
            random[9 - byte] = static_cast<uint8_t>(index >> (byte * 8));
        }
        return AssetGuid::fromUuidV7Fields(1'800'000'000'000ull + index, random);
    }

    double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double percentile95(std::vector<double> samples) {
        std::sort(samples.begin(), samples.end());
        return samples[static_cast<size_t>((samples.size() - 1) * 0.95)];
    }

    struct ProcessMemory {
        double workingSetMiB = 0.0;
        double peakWorkingSetMiB = 0.0;
    };

    ProcessMemory processMemory() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) == 0) {
            return {};
        }
        constexpr double bytesPerMiB = 1024.0 * 1024.0;
        return {
            static_cast<double>(counters.WorkingSetSize) / bytesPerMiB,
            static_cast<double>(counters.PeakWorkingSetSize) / bytesPerMiB,
        };
#else
        return {};
#endif
    }

    struct TemporaryDatabase {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("iridium-asset-catalog-scale-" +
                std::to_string(Clock::now().time_since_epoch().count()) + ".sqlite");

        ~TemporaryDatabase() {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(path.string() + "-wal", error);
            std::filesystem::remove(path.string() + "-shm", error);
        }
    };

} // namespace

int main() {
    constexpr uint64_t kRecordCount = 100'000;
    const ProcessMemory initialMemory = processMemory();
    std::vector<AssetCatalogRecord> records;
    records.reserve(kRecordCount);
    for (uint64_t index = 0; index < kRecordCount; ++index) {
        records.push_back({
            .guid = benchmarkGuid(index),
            .assetType = index % 4 == 0 ? "iridium.texture" : "iridium.model",
            .assetRoot = "benchmark",
            .sourcePath = "generated/folder-" + std::to_string(index % 100) +
                "/asset-" + std::to_string(index) + ".gltf",
            .metadataPath = "generated/asset-" + std::to_string(index) +
                ".gltf.iridium.meta",
            .displayName = "Asset " + std::to_string(index),
            .importerId = "iridium.benchmark",
            .importerVersion = 1,
            .tags = { index % 10 == 0 ? "featured" : "ordinary", "scale-fixture" },
        });
    }

    TemporaryDatabase database;
    const auto catalog = createSqliteAssetCatalog(database.path);
    const auto rebuildBegin = Clock::now();
    catalog->rebuild(records);
    const double rebuildMs = milliseconds(Clock::now() - rebuildBegin);

    AssetCatalogQuery warm{
        .text = "999",
        .assetType = "iridium.model",
        .limit = 100,
    };
    (void)catalog->query(warm);

    std::vector<double> warmSamples;
    std::vector<double> incrementalSamples;
    for (uint32_t iteration = 0; iteration < 200; ++iteration) {
        const auto begin = Clock::now();
        (void)catalog->query(warm);
        warmSamples.push_back(milliseconds(Clock::now() - begin));

        AssetCatalogQuery incremental = warm;
        incremental.text = "999" + std::to_string(iteration % 10);
        incremental.calculateTotalMatches = false;
        const auto incrementalBegin = Clock::now();
        (void)catalog->query(incremental);
        incrementalSamples.push_back(milliseconds(Clock::now() - incrementalBegin));
    }

    const double warmP95 = percentile95(warmSamples);
    const double incrementalP95 = percentile95(incrementalSamples);
    const ProcessMemory finalMemory = processMemory();
    std::cout
        << "{\n"
        << "  \"records\": " << kRecordCount << ",\n"
        << "  \"coldRebuildMs\": " << rebuildMs << ",\n"
        << "  \"workingSetBeforeMiB\": " << initialMemory.workingSetMiB << ",\n"
        << "  \"workingSetAfterMiB\": " << finalMemory.workingSetMiB << ",\n"
        << "  \"peakWorkingSetMiB\": " << finalMemory.peakWorkingSetMiB << ",\n"
        << "  \"warmQueryP95Ms\": " << warmP95 << ",\n"
        << "  \"incrementalQueryP95Ms\": " << incrementalP95 << ",\n"
        << "  \"warmGateMs\": 16.0,\n"
        << "  \"incrementalGateMs\": 4.0,\n"
        << "  \"passed\": " << (warmP95 <= 16.0 && incrementalP95 <= 4.0
            ? "true" : "false") << "\n"
        << "}\n";
    return warmP95 <= 16.0 && incrementalP95 <= 4.0 ? 0 : 1;
}
