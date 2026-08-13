#pragma once

#include "assets/runtime/AssetRuntimePublisher.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Iridium {

    struct PreparedRuntimeAsset {
        std::string cookKey;
        uint64_t estimatedUploadBytes = 0;
        bool allowSingleOversizedUpload = false;
        std::function<RuntimeAssetPublishOutcome()> publish;
    };

    struct AssetReimportRequest {
        AssetGuid assetGuid;
        std::string requestKey;
        std::function<PreparedRuntimeAsset(std::stop_token)> prepare;
    };

    enum class AssetReimportCompletionStatus : uint8_t {
        Ready,
        Failed,
    };

    struct AssetReimportCompletion {
        AssetGuid assetGuid;
        std::string requestKey;
        AssetReimportCompletionStatus status =
            AssetReimportCompletionStatus::Failed;
        std::optional<PreparedRuntimeAsset> prepared;
        std::string diagnostic;
    };

    struct AssetReimportSchedulerStats {
        uint64_t enqueued = 0;
        uint64_t coalesced = 0;
        uint64_t cancellationRequests = 0;
        uint64_t prepared = 0;
        uint64_t failed = 0;
        uint64_t superseded = 0;
        uint32_t queued = 0;
        uint32_t active = 0;
        uint32_t completed = 0;
    };

    struct AssetReimportDrainResult {
        uint32_t ready = 0;
        uint32_t failed = 0;
        uint32_t unchanged = 0;
    };

    class AssetReimportScheduler {
    public:
        AssetReimportScheduler();
        ~AssetReimportScheduler();

        AssetReimportScheduler(
            const AssetReimportScheduler&) = delete;
        AssetReimportScheduler& operator=(
            const AssetReimportScheduler&) = delete;

        // Requests execute in dependency-first enqueue order. A newer request
        // for the same GUID replaces queued work and requests cancellation of
        // active work. Non-cooperative stale completions are still discarded.
        bool enqueue(AssetReimportRequest request);
        void cancel(AssetGuid assetGuid);

        [[nodiscard]] std::vector<AssetReimportCompletion>
            takeCompletions();
        [[nodiscard]] bool waitForCompletion(
            std::chrono::milliseconds timeout);
        [[nodiscard]] AssetReimportDrainResult drainTo(
            AssetRuntimePublisher& publisher);
        [[nodiscard]] AssetReimportSchedulerStats stats() const;

        void shutdown() noexcept;

    private:
        struct WorkItem {
            AssetReimportRequest request;
            uint64_t serial = 0;
        };

        void workerLoop(std::stop_token stopToken);

        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<WorkItem> queued_;
        std::vector<AssetReimportCompletion> completed_;
        std::map<AssetGuid, uint64_t> latestSerial_;
        std::optional<AssetGuid> activeAsset_;
        std::string activeRequestKey_;
        std::optional<std::stop_source> activeStop_;
        uint64_t serialCounter_ = 0;
        AssetReimportSchedulerStats stats_;
        bool shutdown_ = false;
        std::jthread worker_;
    };

} // namespace Iridium
