#pragma once

#include "assets/runtime/SourceChangeTracker.h"
#include "assets/runtime/SourceFileWatcher.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
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

        // startWorkers processes sources automatically on the task system
        // (M7R R5b.2), which `tasks` must then provide.
        AssetSourceMonitor(
            uint64_t debounceNanoseconds,
            std::chrono::milliseconds scanInterval,
            ContentHasher hasher = {},
            bool startWorkers = true,
            Tasks::TaskSystem* tasks = nullptr);
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

        // Deterministic tool/test entry point. Production uses the periodic
        // watcher scan (pinned I/O thread) and monitor pass (Background task),
        // so stat/hash work never enters a frame (M7R R5b.2).
        void processOnce(uint64_t nowNanoseconds);
        void shutdown() noexcept;

    private:
        // Hashes the debounced sources outside mutex_ (M7R R5b.2).
        void processPendingEvents(
            uint64_t nowNanoseconds);

        SourceFileWatcher watcher_;
        SourceChangeTracker tracker_;
        AssetDependencyGraph dependencies_;
        ContentHasher hasher_;
        mutable std::mutex mutex_;
        std::deque<SourceChangeBatch> batches_;
        uint64_t emittedBatches_ = 0;
        bool shutdown_ = false;
        bool automatic_ = false;
        // The 10 ms monitor pass, started by the frame tick. Declared last:
        // stopped in shutdown() before the state above goes.
        std::unique_ptr<Tasks::Periodic> periodic_;
    };

} // namespace Iridium
