#include "assets/AssetCatalogService.h"
#include "assets/AssetMetadata.h"
#include "assets/SqliteAssetCatalog.h"
#include "editor/EditorAssetSettingsTransactionService.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

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

    AssetGuid guid(std::string_view text) {
        const auto parsed = AssetGuid::parse(text);
        if (!parsed) throw std::runtime_error("invalid test GUID");
        return *parsed;
    }

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            const auto stamp = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            path = std::filesystem::temp_directory_path() /
                ("iridium-asset-settings-transactions-" +
                    std::to_string(stamp));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    AssetCatalogJobResult waitForResult(AssetCatalogService& service) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            auto results = service.takeResults();
            if (!results.empty()) return std::move(results.front());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("asset catalog job timed out");
    }

    bool historyAdvancesOnlyAfterSuccessfulJobs() {
        const AssetGuid root = guid(
            "0198d300-0000-7000-8000-000000000001");
        uint64_t nextSerial = 0;
        nlohmann::json submitted;
        EditorAssetSettingsTransactionService transactions(
            [&](AssetGuid submittedRoot, nlohmann::json settings) {
                if (submittedRoot != root) {
                    throw std::runtime_error(
                        "transaction submitted the wrong root GUID");
                }
                submitted = std::move(settings);
                return ++nextSerial;
            });
        const nlohmann::json before{
            { "transparency_policies", nlohmann::json::object() },
        };
        const nlohmann::json after{
            { "transparency_policies", {
                { "primitive-guid", {
                    { "class", "thin_glass" },
                    { "quality", "ordinary2" },
                } },
            } },
        };

        auto result = transactions.requestApply(
            root, "Edit transparency policy", before, after);
        CHECK(result.outcome ==
            EditorAssetSettingsTransactionOutcome::Queued);
        CHECK(transactions.pending());
        CHECK(transactions.historyEntryCount() == 0);
        CHECK(submitted == after);
        result = transactions.complete(result.jobSerial, false,
            "fixture cook rejected");
        CHECK(result.outcome ==
            EditorAssetSettingsTransactionOutcome::JobFailed);
        CHECK(!transactions.pending());
        CHECK(transactions.historyEntryCount() == 0);

        result = transactions.requestApply(
            root, "Edit transparency policy", before, after);
        CHECK(result);
        result = transactions.complete(result.jobSerial, true);
        CHECK(result.outcome ==
            EditorAssetSettingsTransactionOutcome::Completed);
        CHECK(transactions.historyEntryCount() == 1);
        CHECK(transactions.appliedEntryCount() == 1);
        CHECK(transactions.canUndo(root));
        CHECK(transactions.undoLabel(root) == "Edit transparency policy");

        result = transactions.requestUndo(root, after);
        CHECK(result);
        CHECK(submitted == before);
        const uint64_t failedUndoSerial = result.jobSerial;
        result = transactions.complete(failedUndoSerial, false,
            "undo cook failed");
        CHECK(result.outcome ==
            EditorAssetSettingsTransactionOutcome::JobFailed);
        CHECK(transactions.appliedEntryCount() == 1);
        CHECK(transactions.canUndo(root));

        result = transactions.requestUndo(root, after);
        CHECK(result);
        result = transactions.complete(result.jobSerial, true);
        CHECK(result);
        CHECK(transactions.appliedEntryCount() == 0);
        CHECK(transactions.canRedo(root));

        result = transactions.requestRedo(root,
            nlohmann::json{ { "external", true } });
        CHECK(result.outcome ==
            EditorAssetSettingsTransactionOutcome::Diverged);
        CHECK(transactions.historyEntryCount() == 0);
        CHECK(!transactions.canRedo(root));
        return true;
    }

    bool policyApplyUndoRedoPersistsAndRecooks() {
        TemporaryDirectory temporary;
        const std::filesystem::path source = temporary.path /
            "transaction-policy.gltf";
        std::filesystem::copy_file(
            std::filesystem::path(PROJECT_ROOT_DIR) / "tests" / "assets" /
                "gltf_model_cooker_fixture.gltf",
            source);
        auto catalog = createSqliteAssetCatalog(":memory:");
        AssetCatalogService catalogService(catalog.get(), {
            AssetRoot{ "project", temporary.path },
        });

        (void)catalogService.requestImport(source);
        const AssetCatalogJobResult imported = waitForResult(catalogService);
        CHECK(imported.succeeded);
        CHECK(imported.assetGuid.has_value());
        const AssetGuid root = *imported.assetGuid;
        const auto records = catalog->recordsForSourceRoot(root);
        const auto primitive = std::ranges::find_if(records,
            [](const AssetCatalogRecord& record) {
                return record.assetType == "iridium.model-primitive";
            });
        CHECK(primitive != records.end());

        const std::filesystem::path metadataPath =
            assetMetadataSidecarPath(source);
        const AssetMetadataReadResult initial =
            readAssetMetadata(metadataPath);
        CHECK(initial.metadata.has_value());
        const nlohmann::json before = initial.metadata->settings;
        nlohmann::json after = before;
        after["transparency_execution_mode"] = "classified";
        after["transparency_policies"][primitive->guid.toString()] = {
            { "class", "thin_glass" },
            { "priority", 3 },
            { "quality", "ordinary2" },
            { "schema_version", 1 },
            { "thin_sheet_thickness_m", 0.0 },
        };

        EditorAssetSettingsTransactionService transactions(
            [&](AssetGuid assetGuid, nlohmann::json settings) {
                return catalogService.requestUpdateSettings(
                    assetGuid, std::move(settings));
            });
        auto transaction = transactions.requestApply(root,
            "Edit transparency policy", before, after);
        CHECK(transaction);
        AssetCatalogJobResult cooked = waitForResult(catalogService);
        CHECK(cooked.succeeded);
        CHECK(cooked.assetGuid == std::optional(root));
        CHECK(transactions.complete(cooked.serial, true));
        AssetMetadataReadResult persisted = readAssetMetadata(metadataPath);
        CHECK(persisted.metadata.has_value());
        CHECK(persisted.metadata->assetGuid == root);
        CHECK(persisted.metadata->settings == after);
        CHECK(catalog->recordsForSourceRoot(root).size() == records.size());

        transaction = transactions.requestUndo(
            root, persisted.metadata->settings);
        CHECK(transaction);
        cooked = waitForResult(catalogService);
        CHECK(cooked.succeeded);
        CHECK(transactions.complete(cooked.serial, true));
        persisted = readAssetMetadata(metadataPath);
        CHECK(persisted.metadata->settings == before);
        CHECK(persisted.metadata->assetGuid == root);

        transaction = transactions.requestRedo(
            root, persisted.metadata->settings);
        CHECK(transaction);
        cooked = waitForResult(catalogService);
        CHECK(cooked.succeeded);
        CHECK(transactions.complete(cooked.serial, true));
        persisted = readAssetMetadata(metadataPath);
        CHECK(persisted.metadata->settings == after);
        CHECK(persisted.metadata->assetGuid == root);
        CHECK(transactions.canUndo(root));
        CHECK(!transactions.canRedo(root));
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[]{
        { "history waits for successful jobs",
            historyAdvancesOnlyAfterSuccessfulJobs },
        { "policy apply undo redo persists and recooks",
            policyApplyUndoRedoPersistsAndRecooks },
    };
    size_t failures = 0;
    for (const auto& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else ++failures;
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
