#include "assets/AssetCatalogService.h"
#include "assets/SqliteAssetCatalog.h"
#include "core/EngineLog.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition \
                    " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    struct TemporaryDirectory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("iridium-catalog-service-" +
                std::to_string(std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    std::vector<AssetCatalogJobResult> waitForResult(
        AssetCatalogService& service) {
        for (int attempt = 0; attempt < 5'000; ++attempt) {
            auto results = service.takeResults();
            if (!results.empty()) return results;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }

    class BlockingImporter final :
        public AssetImporter {
    public:
        const ImporterDescriptor&
            descriptor() const noexcept override {
            static const ImporterDescriptor
                value{
                    .id =
                        "iridium.test.blocking",
                    .implementationVersion = 1,
                    .currentSettingsSchemaVersion =
                        1,
                    .assetTypes = {
                        "iridium.test",
                    },
                    .extensions = {
                        ".block",
                    },
                };
            return value;
        }

        ImportProbeResult probe(
            const std::filesystem::path&,
            std::span<const std::byte>)
                const override {
            return ImportProbeResult::Supported;
        }

        NormalizedImportSettings
            normalizeSettings(
                uint32_t,
                const nlohmann::json&,
                bool) const override {
            return {
                .schemaVersion = 1,
                .values =
                    nlohmann::json::object(),
                .canonicalBytes = {
                    std::byte{ '{' },
                    std::byte{ '}' },
                },
            };
        }

        ParsedSourceAsset parse(
            const ImportSource& source,
            const NormalizedImportSettings&)
                const override {
            while (!source.stopToken
                    .stop_requested()) {
                std::this_thread::sleep_for(
                    std::chrono::
                        milliseconds(1));
            }
            ParsedSourceAsset result;
            result.diagnostics.push_back({
                .code =
                    "BLOCKING_IMPORT_CANCELLED",
                .message =
                    "Asset import cancelled.",
            });
            return result;
        }

        CookProduct cook(
            const ParsedSourceAsset&,
            const NormalizedImportSettings&,
            const CookTarget&,
            const AssetCookContext&,
            std::stop_token)
                const override {
            return {};
        }
    };

    bool backgroundImportRefreshAndMove() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path / "model.gltf";
        std::filesystem::copy_file(
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets" /
                "gltf_model_cooker_fixture.gltf",
            source);
        const auto catalog = createSqliteAssetCatalog(":memory:");
        AssetCatalogService service(catalog.get(), {
            AssetRoot{ "project", temporary.path },
        });
        (void)service.requestImport(source);
        auto results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(results[0].assetGuid.has_value());
        const AssetGuid importedGuid = *results[0].assetGuid;
        CHECK(std::filesystem::is_regular_file(
            assetMetadataSidecarPath(source)));
        const auto records = catalog->recordsForGuid(importedGuid);
        CHECK(records.size() == 1);
        CHECK(records[0].sourcePath == "model.gltf");
        CHECK(records[0].assetType == "iridium.model");

        (void)service.requestImport(source);
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(results[0].assetGuid == importedGuid);

        const std::filesystem::path movedDirectory =
            temporary.path / "vehicles";
        std::filesystem::create_directories(movedDirectory);
        const std::filesystem::path movedSource =
            movedDirectory / "renamed.gltf";
        const std::filesystem::path movedMetadata =
            assetMetadataSidecarPath(movedSource);
        std::filesystem::rename(source, movedSource);
        std::filesystem::rename(
            assetMetadataSidecarPath(source), movedMetadata);
        (void)service.requestRefresh();
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        const auto movedRecords =
            catalog->recordsForGuid(importedGuid);
        CHECK(movedRecords.size() == 1);
        CHECK(movedRecords[0].sourcePath ==
            "vehicles/renamed.gltf");
        return true;
    }

    bool importsOutsideSourceIntoProjectPackage() {
        TemporaryDirectory root;
        TemporaryDirectory outside;
        const std::filesystem::path source =
            outside.path / "outside.gltf";
        std::filesystem::copy_file(
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets" /
                "gltf_model_cooker_fixture.gltf",
            source);
        {
            nlohmann::json document;
            {
                std::ifstream input(source);
                input >> document;
            }
            document["buffers"].push_back({
                { "byteLength", 4 },
                { "uri",
                    "geometry/payload.bin" },
            });
            std::ofstream output(
                source,
                std::ios::binary |
                    std::ios::trunc);
            output << document.dump(2);
        }
        std::filesystem::create_directories(
            outside.path / "geometry");
        {
            std::ofstream dependency(
                outside.path / "geometry" /
                    "payload.bin",
                std::ios::binary);
            dependency.write(
                "\x00\x01\x02\x03", 4);
        }
        const auto catalog = createSqliteAssetCatalog(":memory:");
        EngineLog log;
        AssetCatalogService service(
            catalog.get(), {
                AssetRoot{
                    "project", root.path },
            }, &log);
        (void)service.requestImport(source);
        const auto results = waitForResult(service);
        CHECK(results.size() == 1);
        if (!results[0].succeeded) {
            std::cerr <<
                results[0].diagnostic << '\n';
        }
        CHECK(results[0].succeeded);
        CHECK(!std::filesystem::exists(
            assetMetadataSidecarPath(source)));
        const std::filesystem::path
            projectSource =
                root.path / "outside" /
                "outside.gltf";
        CHECK(std::filesystem::is_regular_file(
            projectSource));
        CHECK(std::filesystem::is_regular_file(
            root.path / "outside" /
                "geometry" / "payload.bin"));
        CHECK(std::filesystem::is_regular_file(
            assetMetadataSidecarPath(
                projectSource)));
        CHECK(catalog->recordCount() > 0);
        CHECK(std::ranges::any_of(
            log.snapshot(),
            [](const EngineLogEntry& entry) {
                return entry.category ==
                        "Asset Import" &&
                    entry.message.find(
                        "completed") !=
                        std::string::npos;
            }));
        return true;
    }

    bool shutdownCancelsActiveImport() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path / "slow.block";
        {
            std::ofstream output(
                source, std::ios::binary);
            output << "blocking";
        }
        ImporterRegistry importers;
        importers.registerImporter(
            std::make_shared<
                BlockingImporter>());
        const auto catalog =
            createSqliteAssetCatalog(
                ":memory:");
        EngineLog log;
        AssetCatalogService service(
            catalog.get(), {
                AssetRoot{
                    "project",
                    temporary.path },
            },
            std::move(importers),
            &log);
        (void)service.requestImport(source);
        for (int attempt = 0;
            attempt < 1'000 &&
                !service.busy();
            ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::
                    milliseconds(1));
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
        const auto start =
            std::chrono::steady_clock::now();
        service.shutdown();
        const auto elapsed =
            std::chrono::steady_clock::now() -
            start;
        CHECK(elapsed <
            std::chrono::milliseconds(500));
        CHECK(std::ranges::any_of(
            log.snapshot(),
            [](const EngineLogEntry& entry) {
                return entry.category ==
                        "Asset Import" &&
                    entry.message.find(
                        "cancelled") !=
                        std::string::npos;
            }));
        return true;
    }

    bool missingExternalDependencyRollsBack() {
        TemporaryDirectory root;
        TemporaryDirectory outside;
        const std::filesystem::path source =
            outside.path /
            "incomplete.gltf";
        nlohmann::json document;
        {
            std::ifstream input(
                std::filesystem::path(
                    PROJECT_ROOT_DIR) /
                    "tests" / "assets" /
                    "gltf_model_cooker_fixture.gltf");
            input >> document;
        }
        document["buffers"].push_back({
            { "byteLength", 4 },
            { "uri", "missing.bin" },
        });
        {
            std::ofstream output(
                source,
                std::ios::binary);
            output << document.dump(2);
        }
        const auto catalog =
            createSqliteAssetCatalog(
                ":memory:");
        EngineLog log;
        AssetCatalogService service(
            catalog.get(), {
                AssetRoot{
                    "project", root.path },
            }, &log);
        (void)service.requestImport(
            source);
        const auto results =
            waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(!results[0].succeeded);
        CHECK(!results[0].cancelled);
        CHECK(!std::filesystem::exists(
            root.path / "incomplete"));
        CHECK(!std::filesystem::exists(
            assetMetadataSidecarPath(
                source)));
        CHECK(std::ranges::any_of(
            log.snapshot(),
            [](const EngineLogEntry& entry) {
                return entry.severity ==
                        EngineLogSeverity::Error &&
                    entry.message.find(
                        "missing") !=
                        std::string::npos;
            }));
        return true;
    }

    bool updatesSettingsWithoutChangingIdentity() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path / "settings.gltf";
        std::filesystem::copy_file(
            std::filesystem::path(
                PROJECT_ROOT_DIR) / "tests" /
                "assets" /
                "gltf_model_cooker_fixture.gltf",
            source);
        const auto catalog =
            createSqliteAssetCatalog(":memory:");
        AssetCatalogService service(
            catalog.get(), {
                AssetRoot{
                    "project",
                    temporary.path },
            });
        (void)service.requestImport(source);
        auto results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        const AssetGuid guid =
            *results[0].assetGuid;

        (void)service.requestUpdateSettings(
            guid, {
                { "bake_node_transforms", true },
                { "generate_missing_tangents", false },
                { "preserve_rt_geometry", true },
            });
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(results[0].kind ==
            AssetCatalogJobKind::UpdateSettings);
        CHECK(results[0].assetGuid == guid);
        const AssetMetadataReadResult metadata =
            readAssetMetadata(
                assetMetadataSidecarPath(source));
        CHECK(metadata.metadata.has_value());
        CHECK(metadata.metadata->assetGuid == guid);
        CHECK(metadata.metadata->settings.at(
            "generate_missing_tangents") == false);
        CHECK(catalog->recordsForGuid(
            guid).size() == 1);

        (void)service.requestUpdateSettings(
            guid, {
                { "bake_node_transforms", false },
                { "generate_missing_tangents", true },
                { "preserve_rt_geometry", true },
            });
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(!results[0].succeeded);
        const AssetMetadataReadResult unchanged =
            readAssetMetadata(
                assetMetadataSidecarPath(source));
        CHECK(unchanged.metadata->settings.at(
            "bake_node_transforms") == true);
        return true;
    }

    bool performsPhysicalContentWorkflow() {
        TemporaryDirectory temporary;
        const std::filesystem::path source =
            temporary.path / "model.gltf";
        std::filesystem::copy_file(
            std::filesystem::path(
                PROJECT_ROOT_DIR) / "tests" /
                "assets" /
                "gltf_model_cooker_fixture.gltf",
            source);
        const auto catalog =
            createSqliteAssetCatalog(":memory:");
        AssetCatalogService service(
            catalog.get(), {
                AssetRoot{
                    "project",
                    temporary.path },
            });

        (void)service.requestImport(source);
        auto results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        const AssetGuid guid =
            *results[0].assetGuid;

        (void)service.requestCreateFolder(
            "project", {}, "Vehicles");
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(std::filesystem::is_directory(
            temporary.path / "Vehicles"));

        (void)service.requestRenameAsset(
            guid, "sports_car");
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(std::filesystem::is_regular_file(
            temporary.path /
                "sports_car.gltf"));
        CHECK(readAssetMetadata(
            assetMetadataSidecarPath(
                temporary.path /
                    "sports_car.gltf"))
            .metadata->assetGuid == guid);

        (void)service.requestMoveAsset(
            guid, "Vehicles");
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(catalog->recordsForGuid(guid)[0]
            .sourcePath ==
                "Vehicles/sports_car.gltf");

        (void)service.requestRenameFolder(
            "project", "Vehicles", "Cars");
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(catalog->recordsForGuid(guid)[0]
            .sourcePath ==
                "Cars/sports_car.gltf");

        (void)service.requestDeleteAsset(guid);
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(catalog->recordsForGuid(guid).empty());
        CHECK(!std::filesystem::exists(
            temporary.path / "Cars" /
                "sports_car.gltf"));

        (void)service.requestDeleteFolder(
            "project", "Cars");
        results = waitForResult(service);
        CHECK(results.size() == 1);
        CHECK(results[0].succeeded);
        CHECK(!std::filesystem::exists(
            temporary.path / "Cars"));
        return true;
    }

} // namespace

int main() {
    struct Test {
        const char* name;
        bool (*function)();
    };
    const std::vector<Test> tests{
        { "background import, refresh, and move",
            backgroundImportRefreshAndMove },
        { "external package import",
            importsOutsideSourceIntoProjectPackage },
        { "shutdown cancels active import",
            shutdownCancelsActiveImport },
        { "missing dependency rollback",
            missingExternalDependencyRollsBack },
        { "settings update preserves identity",
            updatesSettingsWithoutChangingIdentity },
        { "physical content workflow",
            performsPhysicalContentWorkflow },
    };
    size_t failures = 0;
    for (const Test& test : tests) {
        try {
            const bool passed = test.function();
            std::cout << (passed ? "[PASS] " : "[FAIL] ")
                << test.name << '\n';
            if (!passed) ++failures;
        }
        catch (const std::exception& exception) {
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
