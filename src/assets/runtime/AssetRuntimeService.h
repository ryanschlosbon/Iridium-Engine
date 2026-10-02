#pragma once

#include "assets/runtime/AssetReimportScheduler.h"
#include "assets/runtime/AssetSourceMonitor.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    struct AssetReimportCause {
        AssetGuid assetGuid;
        std::vector<SourceContentChange>
            changedSources;
    };

    struct TrackedRuntimeAsset {
        AssetGuid assetGuid;
        std::vector<TrackedSourceFile> sources;
        std::vector<AssetDependency> dependencies;
        std::function<PreparedRuntimeAsset(
            const AssetReimportCause&,
            std::stop_token)> prepare;
        bool pinned = false;
    };

    struct AssetRuntimeServiceConfig {
        uint64_t debounceNanoseconds =
            150'000'000;
        std::chrono::milliseconds scanInterval{
            250
        };
        uint64_t uploadBudgetBytes =
            64ull * 1024ull * 1024ull;
        std::optional<uint64_t> gpuResidencyBudgetBytes;
        bool startSourceWorkers = true;
    };

    struct AssetRuntimeServiceTick {
        uint32_t changeBatches = 0;
        uint32_t blockedBatches = 0;
        uint32_t rebuildsRequested = 0;
        uint32_t missingPreparers = 0;
        uint32_t sourceDiagnostics = 0;
        AssetReimportDrainResult reimport;
        RuntimePublishTickResult publication;
        std::optional<RuntimeResidencyResult> residency;
    };

    struct AssetRuntimeServiceStats {
        AssetSourceMonitorStats source;
        AssetReimportSchedulerStats reimport;
        AssetRuntimePublisherStats publisher;
    };

    class AssetRuntimeService {
    public:
        explicit AssetRuntimeService(
            AssetRuntimeServiceConfig config = {},
            AssetSourceMonitor::ContentHasher
                hasher = {});
        ~AssetRuntimeService();

        AssetRuntimeService(
            const AssetRuntimeService&) = delete;
        AssetRuntimeService& operator=(
            const AssetRuntimeService&) = delete;

        void track(TrackedRuntimeAsset asset);
        void untrack(AssetGuid assetGuid);
        void adoptPublished(
            AssetGuid assetGuid,
            std::string cookKey,
            uint64_t cpuResidentBytes,
            uint64_t gpuResidentBytes);
        [[nodiscard]] bool enqueuePrepared(
            AssetGuid assetGuid,
            PreparedRuntimeAsset prepared);
        void reportFailure(
            AssetGuid assetGuid,
            std::string diagnostic);
        [[nodiscard]] bool requestReimport(AssetGuid assetGuid);

        // The frame-facing method only moves small event/completion records and
        // invokes already-prepared publication callbacks within the byte budget.
        [[nodiscard]] AssetRuntimeServiceTick tick();

        void touch(AssetGuid assetGuid, uint64_t serial);
        void setPinned(
            AssetGuid assetGuid, bool pinned);
        [[nodiscard]] std::optional<RuntimeAssetSnapshot>
            snapshot(AssetGuid assetGuid) const;
        [[nodiscard]] std::vector<RuntimeAssetSnapshot>
            snapshots() const;
        [[nodiscard]] AssetRuntimeServiceStats stats() const;

        // Deterministic test/tool entry point when source workers are disabled.
        void processSourcesOnce(uint64_t nowNanoseconds);
        void shutdown() noexcept;

    private:
        [[nodiscard]] static std::string batchRequestKey(
            const SourceChangeBatch& batch,
            AssetGuid assetGuid);

        AssetRuntimeServiceConfig config_;
        AssetSourceMonitor sourceMonitor_;
        AssetReimportScheduler reimport_;
        AssetRuntimePublisher publisher_;
        std::map<AssetGuid,
            std::function<PreparedRuntimeAsset(
                const AssetReimportCause&,
                std::stop_token)>> preparers_;
        uint64_t manualRequestSerial_ = 0;
        bool shutdown_ = false;
    };

} // namespace Iridium
