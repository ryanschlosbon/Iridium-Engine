#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetDiscovery.h"
#include "assets/AssetImport.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/model/ModelProduct.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <vector>

namespace Iridium {

    class EngineLog;
    namespace Tasks {
        class FunctionStrand;
        class TaskSystem;
    }

    struct PreparedCatalogModel {
        AssetGuid assetGuid;
        bool succeeded = false;
        std::shared_ptr<CookedArtifact> artifact;
        std::shared_ptr<CookedModelProductData> product;
        uint64_t cpuResidentBytes = 0;
        uint64_t gpuResidentBytes = 0;
        std::string diagnostic;
    };

    class AssetModelPreparationService {
    public:
        // Requests prepare one at a time on a Background strand of the task
        // system (M7R R5b.2).
        AssetModelPreparationService(
            Tasks::TaskSystem& tasks,
            std::filesystem::path assetRoot,
            std::filesystem::path ddcRoot,
            CookTarget target,
            EngineLog* log = nullptr);
        AssetModelPreparationService(
            Tasks::TaskSystem& tasks,
            std::filesystem::path assetRoot,
            std::shared_ptr<LocalDerivedDataCache> cache,
            CookTarget target,
            EngineLog* log = nullptr);
        // Multi-root form: each record resolves under its own assetRoot.
        AssetModelPreparationService(
            Tasks::TaskSystem& tasks,
            std::vector<AssetRoot> roots,
            std::shared_ptr<LocalDerivedDataCache> cache,
            CookTarget target,
            EngineLog* log = nullptr);
        ~AssetModelPreparationService();

        AssetModelPreparationService(
            const AssetModelPreparationService&) = delete;
        AssetModelPreparationService& operator=(
            const AssetModelPreparationService&) = delete;

        [[nodiscard]] bool request(const AssetCatalogRecord& record);
        [[nodiscard]] std::vector<PreparedCatalogModel> takeResults();
        [[nodiscard]] bool pending(AssetGuid assetGuid) const;
        void shutdown() noexcept;

    private:
        struct CookState;
        [[nodiscard]] std::shared_ptr<CookState> prepareCook(
            const AssetCatalogRecord& record,
            std::stop_token stopToken,
            PreparedCatalogModel& result);
        void finishCook(const CookState& state,
            const DdcRequestResult& cooked,
            PreparedCatalogModel& result);
        void publish(const AssetCatalogRecord& record,
            PreparedCatalogModel result,
            std::stop_token stopToken);
        // Prepares the oldest queued request (a strand item); its cook
        // completes it through a continuation.
        void runNext();

        std::vector<AssetRoot> roots_;
        std::shared_ptr<LocalDerivedDataCache>
            cache_;
        CookTarget target_;
        ImporterRegistry importers_;
        mutable std::mutex mutex_;
        std::deque<AssetCatalogRecord> requests_;
        std::vector<PreparedCatalogModel> results_;
        std::set<AssetGuid> pending_;
        bool shutdown_ = false;
        uint32_t cooksInFlight_ = 0;
        std::condition_variable cooksIdle_;
        std::stop_source stop_;
        EngineLog* log_ = nullptr;
        // Declared last: drained in shutdown() before the state above goes.
        std::unique_ptr<Tasks::FunctionStrand> strand_;
    };

} // namespace Iridium
