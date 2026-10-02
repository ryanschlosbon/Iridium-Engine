#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetImport.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/environment/EnvironmentProduct.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace Iridium {

    class EngineLog;

    struct PreparedCatalogEnvironment {
        AssetGuid assetGuid;
        bool succeeded = false;
        std::shared_ptr<CookedArtifact> artifact;
        std::shared_ptr<CookedEnvironmentProductData> product;
        uint64_t gpuResidentBytes = 0;
        std::string diagnostic;
    };

    class AssetEnvironmentPreparationService {
    public:
        AssetEnvironmentPreparationService(
            std::filesystem::path assetRoot,
            std::shared_ptr<LocalDerivedDataCache> cache,
            CookTarget target,
            EngineLog* log = nullptr);
        ~AssetEnvironmentPreparationService();

        AssetEnvironmentPreparationService(
            const AssetEnvironmentPreparationService&) = delete;
        AssetEnvironmentPreparationService& operator=(
            const AssetEnvironmentPreparationService&) = delete;

        [[nodiscard]] bool request(const AssetCatalogRecord& record);
        [[nodiscard]] std::vector<PreparedCatalogEnvironment> takeResults();
        [[nodiscard]] bool pending(AssetGuid assetGuid) const;
        void shutdown() noexcept;

    private:
        [[nodiscard]] PreparedCatalogEnvironment prepare(
            const AssetCatalogRecord& record, std::stop_token stopToken);
        void workerLoop(std::stop_token stopToken);

        std::filesystem::path assetRoot_;
        std::shared_ptr<LocalDerivedDataCache> cache_;
        CookTarget target_;
        ImporterRegistry importers_;
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<AssetCatalogRecord> requests_;
        std::vector<PreparedCatalogEnvironment> results_;
        std::set<AssetGuid> pending_;
        bool shutdown_ = false;
        std::jthread worker_;
        EngineLog* log_ = nullptr;
    };

} // namespace Iridium
