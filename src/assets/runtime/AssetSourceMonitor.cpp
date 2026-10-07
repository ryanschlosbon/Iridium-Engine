#include "assets/runtime/AssetSourceMonitor.h"

#include "core/tasks/TaskSystem.h"
#include "utils/Sha256.h"

#include <stdexcept>
#include <utility>

namespace Iridium {

    namespace {

        uint64_t monotonicNanoseconds() {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch())
                    .count());
        }

        bool hasBatchOutput(
            const SourceChangeBatch& batch) {
            return !batch.changedSources.empty() ||
                !batch.diagnostics.empty() ||
                batch.sameContentEvents != 0;
        }

    } // namespace

    AssetSourceMonitor::AssetSourceMonitor(
        uint64_t debounceNanoseconds,
        std::chrono::milliseconds scanInterval,
        ContentHasher hasher,
        bool startWorkers,
        Tasks::TaskSystem* tasks)
        : watcher_(scanInterval, startWorkers, tasks),
          tracker_(debounceNanoseconds),
          hasher_(std::move(hasher)),
          automatic_(startWorkers) {
        if (!hasher_) {
            hasher_ =
                [](const std::filesystem::path& path) {
                    return sha256File(path);
                };
        }
        if (automatic_) {
            // M7R R5b.2 (ADR-0015): a Background Periodic replaces the 10 ms
            // sleep-poll thread. The watcher constructor already required the
            // task system.
            constexpr auto serviceInterval =
                std::chrono::milliseconds(10);
            periodic_ = std::make_unique<Tasks::Periodic>(*tasks,
                serviceInterval, Tasks::Periodic::Target::Background,
                [this] { processPendingEvents(monotonicNanoseconds()); },
                "asset.source.monitor");
        }
    }

    AssetSourceMonitor::~AssetSourceMonitor() {
        shutdown();
    }

    void AssetSourceMonitor::trackAsset(
        AssetGuid assetGuid,
        std::span<const TrackedSourceFile> sources,
        std::vector<AssetDependency> dependencies) {
        if (assetGuid.isNil() || sources.empty()) {
            throw std::invalid_argument(
                "Source monitoring requires a stable GUID and at least one source.");
        }
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) {
                throw std::logic_error(
                    "Cannot track an asset after source monitor shutdown.");
            }
            dependencies_.setDependencies(
                assetGuid,
                std::move(dependencies));
            for (const TrackedSourceFile& source :
                sources) {
                tracker_.seedContentHash(
                    assetGuid, source.path,
                    source.contentHash);
            }
        }
        watcher_.unwatchAsset(assetGuid);
        for (const TrackedSourceFile& source :
            sources) {
            watcher_.watch(
                assetGuid, source.path);
        }
    }

    void AssetSourceMonitor::untrackAsset(
        AssetGuid assetGuid) {
        watcher_.unwatchAsset(assetGuid);
        std::lock_guard lock(mutex_);
        tracker_.removeAsset(assetGuid);
        dependencies_.removeAsset(assetGuid);
    }

    std::vector<SourceChangeBatch>
        AssetSourceMonitor::drainBatches() {
        std::lock_guard lock(mutex_);
        std::vector<SourceChangeBatch> result;
        result.reserve(batches_.size());
        while (!batches_.empty()) {
            result.push_back(
                std::move(batches_.front()));
            batches_.pop_front();
        }
        return result;
    }

    AssetSourceMonitorStats
        AssetSourceMonitor::stats() const {
        AssetSourceMonitorStats result;
        result.watcher = watcher_.stats();
        std::lock_guard lock(mutex_);
        result.tracker = tracker_.stats();
        result.emittedBatches =
            emittedBatches_;
        result.pendingBatches =
            static_cast<uint32_t>(
                batches_.size());
        return result;
    }

    void AssetSourceMonitor::processOnce(
        uint64_t nowNanoseconds) {
        if (automatic_) {
            throw std::logic_error(
                "Manual source processing is unavailable while background workers are active.");
        }
        watcher_.scanNow();
        processPendingEvents(
            nowNanoseconds);
    }

    void AssetSourceMonitor::shutdown() noexcept {
        watcher_.shutdown();
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
        }
        if (periodic_) {
            // Unregisters from the frame tick and waits for a pass in flight.
            periodic_->stop();
        }
    }

    void AssetSourceMonitor::processPendingEvents(
        uint64_t nowNanoseconds) {
        const std::vector<SourceFileChangeEvent>
            events = watcher_.drainEvents();
        std::vector<SourceChangeTracker::DueSourceChange> due;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            for (const SourceFileChangeEvent& event :
                events) {
                tracker_.notify(
                    event.assetGuid,
                    event.sourcePath,
                    event.eventNanoseconds);
            }
            due = tracker_.takeDue(nowNanoseconds);
        }
        // SHA-256 of the changed sources runs without the monitor's mutex, so
        // tracking, stats and draining never wait for file reads.
        SourceChangeTracker::hashDueSources(due, hasher_);
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        SourceChangeBatch batch =
            tracker_.completePoll(due, dependencies_);
        if (hasBatchOutput(batch)) {
            batches_.push_back(
                std::move(batch));
            ++emittedBatches_;
        }
    }

} // namespace Iridium
