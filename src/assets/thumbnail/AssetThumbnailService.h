#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetImport.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/thumbnail/AssetThumbnail.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace Iridium {

    class EngineLog;

    struct AssetThumbnailAssociation {
        AssetGuid parentGuid;
        AssetGuid childGuid;

        auto operator<=>(const AssetThumbnailAssociation&)
            const = default;
    };

    struct PreparedAssetThumbnailBatch {
        AssetGuid rootAssetGuid;
        std::vector<AssetThumbnailPixels> thumbnails;
        std::string settingsJson;
        std::vector<AssetDependency> dependencies;
        std::vector<AssetThumbnailAssociation>
            associations;
        std::string deferredReason;
        std::string diagnostic;
    };

    struct AssetThumbnailServiceStats {
        uint64_t rootsQueued = 0;
        uint64_t rootsCancelled = 0;
        uint64_t thumbnailsProduced = 0;
        uint64_t thumbnailsFailed = 0;
        uint32_t queuedRoots = 0;
        uint32_t demandedAssets = 0;
        bool active = false;
    };

    enum class AssetThumbnailStatus : uint8_t {
        Unavailable,
        Pending,
        Prepared,
        Ready,
        Failed,
    };

    struct AssetThumbnailInfo {
        AssetThumbnailStatus status =
            AssetThumbnailStatus::Unavailable;
        std::string diagnostic;
    };

    struct AssetThumbnailSourceDetail {
        bool available = false;
        std::string settingsJson;
        std::vector<AssetDependency> dependencies;
        std::vector<AssetThumbnailAssociation>
            associations;
        std::string diagnostic;
    };

    class AssetThumbnailService {
    public:
        AssetThumbnailService(
            std::filesystem::path assetRoot,
            std::filesystem::path ddcRoot,
            CookTarget target,
            EngineLog* log = nullptr);
        AssetThumbnailService(
            std::filesystem::path assetRoot,
            std::shared_ptr<LocalDerivedDataCache> cache,
            CookTarget target,
            EngineLog* log = nullptr);
        ~AssetThumbnailService();

        AssetThumbnailService(
            const AssetThumbnailService&) = delete;
        AssetThumbnailService& operator=(
            const AssetThumbnailService&) = delete;

        // Replaces the visible demand set. Queued roots that are no longer
        // visible are cancelled; an active root may finish CPU work, but its
        // no-longer-demanded pixels are discarded before crossing threads.
        void setDemand(
            std::span<const AssetCatalogRecord> visibleRecords);
        // Adds a second demand source for assets that must remain inspectable
        // even when their browser tile is outside the current page.
        void setPinnedDemand(
            std::span<const AssetCatalogRecord> records);
        // Keeps at most one selected record in a separate high-resolution
        // preview lane. It never raises the browser-grid thumbnail extent.
        void setDetailDemand(
            std::span<const AssetCatalogRecord> sourceRecords,
            std::optional<AssetGuid> selectedAsset);
        void invalidate(AssetGuid rootAssetGuid);
        void markPublished(AssetGuid assetGuid);
        void markEvicted(AssetGuid assetGuid);
        void reportFailure(
            AssetGuid assetGuid,
            std::string diagnostic);
        [[nodiscard]] bool isDemanded(
            AssetGuid assetGuid) const;
        [[nodiscard]] bool isDetailDemanded(
            AssetGuid assetGuid) const;
        [[nodiscard]] AssetThumbnailInfo info(
            AssetGuid assetGuid) const;
        [[nodiscard]] std::vector<
            AssetThumbnailInfo> info(
                std::span<const AssetGuid>
                    assetGuids) const;
        [[nodiscard]] AssetThumbnailSourceDetail
            sourceDetail(
                AssetGuid assetGuid) const;
        [[nodiscard]] std::vector<
            PreparedAssetThumbnailBatch> takeResults();
        [[nodiscard]] AssetThumbnailServiceStats stats() const;
        void shutdown() noexcept;

    private:
        struct Job {
            AssetGuid rootAssetGuid;
            AssetCatalogRecord rootRecord;
            std::vector<AssetCatalogRecord> records;
            uint32_t extent =
                kAssetThumbnailExtent;
            bool detail = false;
        };

        [[nodiscard]] PreparedAssetThumbnailBatch prepare(
            const Job& job,
            std::stop_token stopToken);
        void queueMissingLocked(
            std::map<AssetGuid, Job> grouped);
        [[nodiscard]] static std::map<
            AssetGuid, Job> groupDemand(
                std::span<const
                    AssetCatalogRecord> records);
        void rebuildDemandLocked();
        void workerLoop(std::stop_token stopToken);

        std::filesystem::path assetRoot_;
        std::shared_ptr<LocalDerivedDataCache>
            cache_;
        CookTarget target_;
        ImporterRegistry importers_;
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<Job> jobs_;
        std::vector<PreparedAssetThumbnailBatch>
            results_;
        std::map<AssetGuid, Job> demandByRoot_;
        std::map<AssetGuid, Job>
            visibleDemandByRoot_;
        std::map<AssetGuid, Job>
            pinnedDemandByRoot_;
        std::optional<Job> detailDemand_;
        std::optional<AssetGuid>
            detailAsset_;
        std::set<AssetGuid> demandedAssets_;
        std::set<AssetGuid> completedAssets_;
        std::map<AssetGuid, AssetThumbnailInfo>
            info_;
        std::map<AssetGuid, AssetGuid>
            rootByAsset_;
        std::map<AssetGuid,
            AssetThumbnailSourceDetail>
            detailByRoot_;
        std::optional<AssetGuid> activeRoot_;
        AssetThumbnailServiceStats stats_;
        bool shutdown_ = false;
        std::jthread worker_;
        EngineLog* log_ = nullptr;
    };

} // namespace Iridium
