#pragma once

#include "assets/runtime/SourceChangeTracker.h"
#include "assets/runtime/SourceFileWatcher.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace Iridium {

    struct TrackedSourceFile {
        std::filesystem::path path;
        std::string contentHash;
    };

    struct AssetSourceMonitorStats {
        SourceFileWatcherStats watcher;
        SourceChangeTrackerStats tracker;
        uint64_t emittedBatches = 0;
        uint32_t pendingBatches = 0;
    };

    class AssetSourceMonitor {
    public:
        using ContentHasher =
            SourceChangeTracker::ContentHasher;

        AssetSourceMonitor(
            uint64_t debounceNanoseconds,
            std::chrono::milliseconds scanInterval,
            ContentHasher hasher = {},
            bool startWorkers = true);
        ~AssetSourceMonitor();

        AssetSourceMonitor(
            const AssetSourceMonitor&) = delete;
        AssetSourceMonitor& operator=(
            const AssetSourceMonitor&) = delete;

        // Source hashes come from the accepted cook receipt/product. This
        // avoids synchronous baseline reads when an asset becomes resident.
        void trackAsset(
            AssetGuid assetGuid,
            std::span<const TrackedSourceFile> sources,
            std::vector<AssetDependency> dependencies);
        void untrackAsset(AssetGuid assetGuid);

        [[nodiscard]] std::vector<SourceChangeBatch>
            drainBatches();
        [[nodiscard]] AssetSourceMonitorStats stats() const;

        // Deterministic tool/test entry point. Production uses the background
        // watcher and monitor workers so stat/hash work never enters a frame.
        void processOnce(uint64_t nowNanoseconds);
        void shutdown() noexcept;

    private:
        void processPendingEvents(
            uint64_t nowNanoseconds);
        void workerLoop(std::stop_token stopToken);

        SourceFileWatcher watcher_;
        SourceChangeTracker tracker_;
        AssetDependencyGraph dependencies_;
        ContentHasher hasher_;
        mutable std::mutex mutex_;
        std::deque<SourceChangeBatch> batches_;
        uint64_t emittedBatches_ = 0;
        bool shutdown_ = false;
        bool automatic_ = false;
        std::jthread worker_;
    };

} // namespace Iridium
