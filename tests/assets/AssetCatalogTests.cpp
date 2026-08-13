#include "assets/AssetDiscovery.h"
#include "assets/AssetGuid.h"
#include "assets/AssetMetadata.h"
#include "assets/SqliteAssetCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    AssetGuid testGuid(uint64_t timestamp, uint8_t seed = 0) {
        std::array<uint8_t, 10> random{};
        for (size_t index = 0; index < random.size(); ++index) {
            random[index] = static_cast<uint8_t>(seed + index);
        }
        return AssetGuid::fromUuidV7Fields(timestamp, random);
    }

    struct TemporaryDirectory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("iridium-asset-catalog-" + testGuid(
                static_cast<uint64_t>(std::chrono::steady_clock::now()
                    .time_since_epoch().count())).toString());

        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    void writeSource(const std::filesystem::path& path, std::string_view bytes = "{}") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << bytes;
    }

    AssetMetadata sampleMetadata() {
        AssetMetadata metadata;
        metadata.assetGuid = testGuid(0x0123456789abull, 1);
        metadata.assetType = "iridium.model";
        metadata.importerId = "iridium.gltf";
        metadata.importerVersion = 7;
        metadata.settingsSchemaVersion = 2;
        metadata.settings = {
            { "zSetting", true },
            { "nested", { { "z", 2 }, { "a", 1 } } },
            { "unknownFutureSetting", "preserve-me" },
        };
        metadata.subassets = {
            {
                .guid = testGuid(0x0123456789acull, 2),
                .assetType = "iridium.mesh",
                .sourceKey = "nodes/0/meshes/0/primitives/1",
                .structuralFingerprint = "sha256:mesh-b",
            },
            {
                .guid = testGuid(0x0123456789adull, 3),
                .assetType = "iridium.mesh",
                .sourceKey = "nodes/0/meshes/0/primitives/0",
                .structuralFingerprint = "sha256:mesh-a",
            },
        };
        metadata.tags = { "vehicle", "benchmark", "vehicle" };
        return metadata;
    }

    bool testGuidContract() {
        std::array<uint8_t, 10> random{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        const AssetGuid guid = AssetGuid::fromUuidV7Fields(
            0x0123456789abull, random);
        CHECK(guid.toString() == "01234567-89ab-7001-8203-040506070809");
        CHECK(guid.version() == 7);
        CHECK(guid.hasRfc4122Variant());
        CHECK(!guid.isNil());
        CHECK(AssetGuid::parse(guid.toString()) == guid);
        CHECK(AssetGuid::parse("01234567-89AB-7001-8203-040506070809") == guid);
        CHECK(!AssetGuid::parse("01234567-89ab-7001-8203-04050607080"));
        CHECK(!AssetGuid::parse("01234567-89ab-7001-c203-040506070809")
            ->hasRfc4122Variant());

        struct MeshTag {};
        const AssetRef<MeshTag> reference{ guid };
        CHECK(static_cast<bool>(reference));
        CHECK(AssetGuidHash{}(guid) == AssetGuidHash{}(guid));
        return true;
    }

    bool testDeterministicMetadata() {
        const AssetMetadata metadata = sampleMetadata();
        const std::string first = serializeAssetMetadata(metadata);
        const std::string second = serializeAssetMetadata(metadata);
        CHECK(first == second);
        CHECK(first.find("\"a\": 1") < first.find("\"z\": 2"));
        CHECK(first.find("primitives/0") < first.find("primitives/1"));
        CHECK(first.find("\"benchmark\"") < first.find("\"vehicle\""));

        const AssetMetadataReadResult parsed = parseAssetMetadata(first);
        CHECK(!parsed.hasErrors());
        CHECK(parsed.metadata.has_value());
        CHECK(parsed.metadata->assetGuid == metadata.assetGuid);
        CHECK(parsed.metadata->settings.at("unknownFutureSetting") == "preserve-me");
        CHECK(serializeAssetMetadata(*parsed.metadata) == first);

        std::string upper = first;
        const std::string lowerGuid = metadata.assetGuid.toString();
        const size_t guidPosition = upper.find(lowerGuid);
        CHECK(guidPosition != std::string::npos);
        std::transform(upper.begin() + static_cast<std::ptrdiff_t>(guidPosition),
            upper.begin() + static_cast<std::ptrdiff_t>(guidPosition + lowerGuid.size()),
            upper.begin() + static_cast<std::ptrdiff_t>(guidPosition),
            [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        const AssetMetadataReadResult nonCanonical = parseAssetMetadata(upper);
        CHECK(nonCanonical.metadata.has_value());
        CHECK(!nonCanonical.diagnostics.empty());

        const auto malformed = parseAssetMetadata(R"({"schemaVersion":1})");
        CHECK(malformed.hasErrors());
        CHECK(!malformed.metadata);

        AssetMetadata duplicate = metadata;
        duplicate.subassets[1].guid = duplicate.subassets[0].guid;
        CHECK(parseAssetMetadata(serializeAssetMetadata(duplicate)).hasErrors());
        return true;
    }

    bool testSubassetIdentityMatching() {
        const std::vector<SubassetMetadata> previous{
            { testGuid(10, 1), "iridium.mesh", "mesh/old", "sha256:a" },
            { testGuid(11, 2), "iridium.mesh", "mesh/other-a", "sha256:duplicate" },
            { testGuid(12, 3), "iridium.mesh", "mesh/other-b", "sha256:duplicate" },
        };
        const std::vector<DiscoveredSubasset> discovered{
            { "iridium.mesh", "mesh/old", "sha256:changed" },
            { "iridium.mesh", "mesh/renamed", "sha256:a" },
            { "iridium.mesh", "mesh/ambiguous", "sha256:duplicate" },
            { "iridium.material", "material/new", "sha256:new" },
        };
        const auto matches = matchSubassets(previous, discovered);
        CHECK(matches.size() == 4);
        CHECK(matches[0].method == SubassetMatchMethod::ExactSourceKey);
        CHECK(matches[0].existingGuid == previous[0].guid);
        CHECK(matches[1].method == SubassetMatchMethod::NewSubasset);
        CHECK(matches[2].method == SubassetMatchMethod::Ambiguous);
        CHECK(matches[2].ambiguousCandidates.size() == 2);
        CHECK(matches[3].method == SubassetMatchMethod::NewSubasset);

        const std::vector<DiscoveredSubasset> renamed{
            { "iridium.mesh", "mesh/renamed", "sha256:a" },
        };
        const auto unique = matchSubassets(previous, renamed);
        CHECK(unique[0].method == SubassetMatchMethod::UniqueStructuralFingerprint);
        CHECK(unique[0].existingGuid == previous[0].guid);
        return true;
    }

    bool testDiscoveryMovesDuplicatesAndCatalog() {
        TemporaryDirectory temporary;
        AssetMetadata metadata = sampleMetadata();
        const std::filesystem::path originalSource = temporary.path / "car.gltf";
        const std::filesystem::path originalSidecar =
            assetMetadataSidecarPath(originalSource);
        writeSource(originalSource);
        std::string error;
        CHECK(writeAssetMetadataAtomic(originalSidecar, metadata, error));

        const std::array roots{ AssetRoot{ "project", temporary.path } };
        auto discovery = discoverAssetRoots(roots);
        CHECK(!discovery.hasErrors());
        CHECK(discovery.records.size() == 3);
        CHECK(discovery.records[0].guid == metadata.assetGuid);

        const std::filesystem::path movedSource = temporary.path / "models" / "renamed.gltf";
        const std::filesystem::path movedSidecar = assetMetadataSidecarPath(movedSource);
        std::filesystem::create_directories(movedSource.parent_path());
        std::filesystem::rename(originalSource, movedSource);
        std::filesystem::rename(originalSidecar, movedSidecar);
        discovery = discoverAssetRoots(roots);
        CHECK(!discovery.hasErrors());
        const auto rootRecord = std::find_if(discovery.records.begin(), discovery.records.end(),
            [&metadata](const AssetCatalogRecord& record) {
                return record.guid == metadata.assetGuid;
            });
        CHECK(rootRecord != discovery.records.end());
        CHECK(rootRecord->sourcePath == "models/renamed.gltf");
        CHECK(rootRecord->status == AssetCatalogStatus::Ready);

        const std::filesystem::path sourceWithoutSidecar =
            temporary.path / "models" / "moved-without-sidecar.gltf";
        std::filesystem::rename(movedSource, sourceWithoutSidecar);
        discovery = discoverAssetRoots(roots);
        CHECK(discovery.records.size() == 3);
        CHECK(discovery.records.front().status == AssetCatalogStatus::MissingSource);

        const std::filesystem::path duplicateSource = temporary.path / "duplicate.gltf";
        writeSource(duplicateSource);
        CHECK(writeAssetMetadataAtomic(assetMetadataSidecarPath(duplicateSource),
            metadata, error));
        discovery = discoverAssetRoots(roots);
        CHECK(discovery.hasErrors());
        CHECK(std::count_if(discovery.records.begin(), discovery.records.end(),
            [&metadata](const AssetCatalogRecord& record) {
                return record.guid == metadata.assetGuid &&
                    record.status == AssetCatalogStatus::DuplicateGuid;
            }) == 2);

        const auto catalog = createSqliteAssetCatalog(":memory:");
        catalog->rebuild(discovery.records);
        CHECK(catalog->recordCount() == discovery.records.size());
        CHECK(catalog->recordsForGuid(metadata.assetGuid).size() == 2);

        AssetCatalogQuery query;
        query.text = "primitive";
        query.assetType = "iridium.mesh";
        const auto page = catalog->query(query);
        CHECK(page.totalMatches == 4);
        CHECK(page.records.size() == 4);

        const auto firstSnapshot = catalog->query({ .limit = 1000 }).records;
        catalog->rebuild(discovery.records);
        CHECK(catalog->query({ .limit = 1000 }).records == firstSnapshot);
        return true;
    }

    bool testTrackedFixtureSidecar() {
        const auto parsed = readAssetMetadata(std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets" / "material_provenance_fixture.gltf.iridium.meta");
        CHECK(!parsed.hasErrors());
        CHECK(parsed.metadata.has_value());
        CHECK(parsed.metadata->assetType == "iridium.model");
        CHECK(serializeAssetMetadata(*parsed.metadata) ==
            serializeAssetMetadata(*parsed.metadata));
        return true;
    }

    bool testConcurrentRebuildIsReaderAtomic() {
        const auto makeRecords =
            [](size_t count,
                uint64_t timestampBase) {
            std::vector<AssetCatalogRecord>
                result;
            result.reserve(count);
            for (size_t index = 0;
                index < count; ++index) {
                result.push_back({
                    .guid = testGuid(
                        timestampBase + index,
                        static_cast<uint8_t>(
                            index)),
                    .assetType =
                        "iridium.model",
                    .assetRoot = "project",
                    .sourcePath =
                        "models/model-" +
                        std::to_string(index) +
                        ".gltf",
                    .metadataPath =
                        "models/model-" +
                        std::to_string(index) +
                        ".gltf.iridium.meta",
                    .displayName =
                        "Model " +
                        std::to_string(index),
                    .importerId =
                        "iridium.gltf-model",
                    .importerVersion = 1,
                });
            }
            return result;
        };
        const auto first =
            makeRecords(64, 10'000);
        const auto second =
            makeRecords(127, 20'000);
        const auto catalog =
            createSqliteAssetCatalog(
                ":memory:");
        catalog->rebuild(first);

        std::atomic<bool> stop = false;
        std::atomic<bool> failed = false;
        std::jthread reader([&] {
            while (!stop.load(
                std::memory_order_relaxed)) {
                const AssetCatalogQueryPage
                    page = catalog->query({
                        .limit = 1000,
                    });
                const size_t count =
                    page.records.size();
                if ((count != first.size() &&
                        count != second.size()) ||
                    page.totalMatches != count) {
                    failed.store(
                        true,
                        std::memory_order_relaxed);
                    return;
                }
            }
        });
        for (int iteration = 0;
            iteration < 50; ++iteration) {
            catalog->rebuild(
                iteration % 2 == 0
                    ? second : first);
        }
        stop.store(
            true,
            std::memory_order_relaxed);
        reader.join();
        CHECK(!failed.load(
            std::memory_order_relaxed));
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "GUID contract", testGuidContract },
        { "deterministic metadata", testDeterministicMetadata },
        { "subasset identity matching", testSubassetIdentityMatching },
        { "discovery moves, duplicates, and catalog", testDiscoveryMovesDuplicatesAndCatalog },
        { "tracked fixture sidecar", testTrackedFixtureSidecar },
        { "concurrent rebuild is reader atomic",
            testConcurrentRebuildIsReaderAtomic },
    };

    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed = test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
            if (!passed) ++failures;
        } catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
