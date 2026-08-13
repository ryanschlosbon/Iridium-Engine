#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetContentOperations.h"
#include "assets/AssetDiscovery.h"
#include "assets/AssetImport.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Iridium {

    class EngineLog;

    enum class AssetCatalogJobKind : uint8_t {
        Import,
        Refresh,
        UpdateSettings,
        CreateFolder,
        RenameFolder,
        DeleteFolder,
        MoveAsset,
        RenameAsset,
        DeleteAsset,
    };

    struct AssetCatalogJobResult {
        uint64_t serial = 0;
        AssetCatalogJobKind kind = AssetCatalogJobKind::Refresh;
        bool succeeded = false;
        std::filesystem::path sourcePath;
        std::optional<AssetGuid> assetGuid;
        uint64_t recordCount = 0;
        bool cancelled = false;
        std::string diagnostic;
    };

    class AssetCatalogService {
    public:
        AssetCatalogService(
            AssetCatalog* catalog,
            std::vector<AssetRoot> roots,
            EngineLog* log = nullptr);
        AssetCatalogService(
            AssetCatalog* catalog,
            std::vector<AssetRoot> roots,
            ImporterRegistry importers,
            EngineLog* log);
        ~AssetCatalogService();

        AssetCatalogService(const AssetCatalogService&) = delete;
        AssetCatalogService& operator=(const AssetCatalogService&) = delete;

        [[nodiscard]] uint64_t requestImport(
            std::filesystem::path sourcePath,
            std::string rootId = "project",
            std::filesystem::path
                destinationDirectory = {});
        [[nodiscard]] uint64_t requestRefresh();
        [[nodiscard]] uint64_t requestUpdateSettings(
            AssetGuid assetGuid,
            nlohmann::json settings);
        [[nodiscard]] uint64_t requestCreateFolder(
            std::string rootId,
            std::filesystem::path parentDirectory,
            std::string name);
        [[nodiscard]] uint64_t requestRenameFolder(
            std::string rootId,
            std::filesystem::path directory,
            std::string name);
        [[nodiscard]] uint64_t requestDeleteFolder(
            std::string rootId,
            std::filesystem::path directory);
        [[nodiscard]] uint64_t requestMoveAsset(
            AssetGuid assetGuid,
            std::filesystem::path destinationDirectory);
        [[nodiscard]] uint64_t requestRenameAsset(
            AssetGuid assetGuid,
            std::string name);
        [[nodiscard]] uint64_t requestDeleteAsset(
            AssetGuid assetGuid);
        [[nodiscard]] std::vector<AssetCatalogJobResult> takeResults();
        [[nodiscard]] bool busy() const;
        void shutdown() noexcept;

    private:
        struct Job {
            uint64_t serial = 0;
            AssetCatalogJobKind kind = AssetCatalogJobKind::Refresh;
            std::filesystem::path sourcePath;
            std::filesystem::path destinationPath;
            std::string rootId;
            std::string name;
            AssetGuid assetGuid;
            nlohmann::json settings =
                nlohmann::json::object();
        };

        [[nodiscard]] bool sourceBelongsToRoot(
            const std::filesystem::path& source) const;
        [[nodiscard]] uint64_t enqueue(Job job);
        [[nodiscard]] AssetCatalogRecord
            readyRootRecord(AssetGuid guid) const;
        [[nodiscard]] AssetCatalogJobResult execute(
            const Job& job,
            std::stop_token stopToken);
        void workerLoop(std::stop_token stopToken);

        AssetCatalog* catalog_ = nullptr;
        std::vector<AssetRoot> roots_;
        AssetContentOperations contentOperations_;
        ImporterRegistry importers_;
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<Job> jobs_;
        std::vector<AssetCatalogJobResult> results_;
        uint64_t serial_ = 0;
        bool active_ = false;
        bool shutdown_ = false;
        std::jthread worker_;
        EngineLog* log_ = nullptr;
    };

} // namespace Iridium
