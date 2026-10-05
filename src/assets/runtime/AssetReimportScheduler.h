#pragma once

#include "assets/runtime/AssetRuntimePublisher.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Iridium {

    namespace Tasks {
        class FunctionStrand;
        class TaskSystem;
    }

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
        // Requests prepare one at a time on a Background strand of the task
        // system (M7R R5b.2); each keeps its own stop_source.
        explicit AssetReimportScheduler(Tasks::TaskSystem& tasks);
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

        // Prepares the oldest queued request (a strand item).
        void runNext();
        void schedule();

        mutable std::mutex mutex_;
        // Signals completions to waitForCompletion (no worker waits on it).
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
        // Declared last: drained in shutdown() before the state above goes.
        std::unique_ptr<Tasks::FunctionStrand> strand_;
    };

} // namespace Iridium
