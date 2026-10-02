#pragma once

#include "assets/AssetGuid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Iridium {

    enum class RuntimeAssetState : uint8_t {
        Missing,
        Queued,
        Ready,
        ReadyWithError,
        Failed,
        Evicted,
    };

    struct RuntimeAssetPublishOutcome {
        bool succeeded = false;
        uint64_t cpuResidentBytes = 0;
        uint64_t gpuResidentBytes = 0;
        std::function<void()> retire;
        std::string diagnostic;
    };

    struct RuntimeAssetPublishRequest {
        AssetGuid assetGuid;
        std::string cookKey;
        uint64_t estimatedUploadBytes = 0;
        // Explicit escape hatch for rare atomic products that cannot be split
        // across ticks (for example one cooked HDRI descriptor set). It may run
        // only as the first publication of a tick; callers must still impose a
        // product-specific hard size cap.
        bool allowSingleOversizedUpload = false;
        std::function<RuntimeAssetPublishOutcome()> publish;
    };

    struct RuntimeAssetSnapshot {
        AssetGuid assetGuid;
        RuntimeAssetState state = RuntimeAssetState::Missing;
        uint64_t revision = 0;
        std::string cookKey;
        std::string pendingCookKey;
        std::string diagnostic;
        uint64_t cpuResidentBytes = 0;
        uint64_t gpuResidentBytes = 0;
        uint64_t lastUsedSerial = 0;
        bool pinned = false;
        bool hasPublishedRevision = false;
    };

    struct RuntimePublishTickResult {
        uint64_t uploadBudgetBytes = 0;
        uint64_t scheduledUploadBytes = 0;
        uint32_t published = 0;
        uint32_t failed = 0;
        uint32_t deferredByBudget = 0;
        uint32_t oversizedPublications = 0;
        uint32_t queuedAfterTick = 0;
    };

    struct RuntimeResidencyResult {
        uint64_t budgetBytes = 0;
        uint64_t residentBytesBefore = 0;
        uint64_t residentBytesAfter = 0;
        uint32_t evicted = 0;
        bool budgetSatisfied = true;
    };

    struct AssetRuntimePublisherStats {
        uint64_t enqueued = 0;
        uint64_t coalesced = 0;
        uint64_t unchanged = 0;
        uint64_t published = 0;
        uint64_t failed = 0;
        uint64_t retired = 0;
        uint64_t evicted = 0;
        uint64_t scheduledUploadBytes = 0;
        uint64_t cpuResidentBytes = 0;
        uint64_t gpuResidentBytes = 0;
        uint32_t queued = 0;
        uint32_t resident = 0;
        uint32_t pinned = 0;
    };

    class AssetRuntimePublisher {
    public:
        ~AssetRuntimePublisher();

        // Keeps only the newest unpublished request per stable asset identity.
        // Returns false for an already-published or already-queued cook key.
        bool enqueue(RuntimeAssetPublishRequest request);
        void adoptPublished(
            AssetGuid assetGuid,
            std::string cookKey,
            uint64_t cpuResidentBytes,
            uint64_t gpuResidentBytes);
        // Records a preparation/cook failure without disturbing a published
        // last-known-good revision. Any older queued publication is discarded.
        void reportFailure(
            AssetGuid assetGuid, std::string diagnostic);
        void cancelQueued(AssetGuid assetGuid);
        [[nodiscard]] RuntimePublishTickResult tick(
            uint64_t uploadBudgetBytes);

        void setPinned(AssetGuid assetGuid, bool pinned);
        void touch(AssetGuid assetGuid, uint64_t serial);
        [[nodiscard]] RuntimeResidencyResult evictToGpuBudget(
            uint64_t budgetBytes);

        [[nodiscard]] std::optional<RuntimeAssetSnapshot> snapshot(
            AssetGuid assetGuid) const;
        [[nodiscard]] std::vector<RuntimeAssetSnapshot> snapshots() const;
        [[nodiscard]] const AssetRuntimePublisherStats& stats() const noexcept;

        // Retires all published revisions and drops queued requests. Retirement
        // callbacks must delegate GPU lifetime safety to the owning RHI backend.
        void shutdown() noexcept;

    private:
        struct PublishedRevision {
            uint64_t revision = 0;
            std::string cookKey;
            uint64_t cpuResidentBytes = 0;
            uint64_t gpuResidentBytes = 0;
            std::function<void()> retire;
        };

        struct Entry {
            std::optional<PublishedRevision> published;
            std::optional<RuntimeAssetPublishRequest> queued;
            RuntimeAssetState state = RuntimeAssetState::Missing;
            std::string diagnostic;
            uint64_t revisionCounter = 0;
            uint64_t lastUsedSerial = 0;
            bool pinned = false;
        };

        std::map<AssetGuid, Entry> entries_;
        std::set<AssetGuid> queuedAssets_;
        AssetRuntimePublisherStats stats_;

        void retire(PublishedRevision& revision) noexcept;
        [[nodiscard]] RuntimeAssetSnapshot makeSnapshot(
            AssetGuid assetGuid, const Entry& entry) const;
    };

} // namespace Iridium
