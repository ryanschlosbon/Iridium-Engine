#include "assets/runtime/SourceFileWatcher.h"

#include "core/tasks/TaskSystem.h"

#include <stdexcept>
#include <system_error>
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

    } // namespace

    SourceFileWatcher::SourceFileWatcher(
        std::chrono::milliseconds scanInterval,
        bool startWorker,
        Tasks::TaskSystem* tasks)
        : scanInterval_(scanInterval) {
        if (scanInterval_.count() <= 0) {
            throw std::invalid_argument(
                "Source watcher interval must be positive.");
        }
        if (startWorker) {
            if (tasks == nullptr) {
                throw std::invalid_argument(
                    "Automatic source watching needs the task system.");
            }
            // M7R R5b.2 (ADR-0015): the scan is blocking file I/O, so it runs
            // on the pinned I/O thread, started by the frame tick.
            periodic_ = std::make_unique<Tasks::Periodic>(*tasks,
                scanInterval_, Tasks::Periodic::Target::PinnedIo,
                [this] { scanNow(); }, "asset.source.watch_scan");
        }
    }

    SourceFileWatcher::~SourceFileWatcher() {
        shutdown();
    }

    bool SourceFileWatcher::watch(
        AssetGuid owner,
        const std::filesystem::path& sourcePath) {
        if (owner.isNil() || sourcePath.empty()) {
            throw std::invalid_argument(
                "Source watching requires a stable owner GUID and path.");
        }
        const std::filesystem::path normalized =
            normalizePath(sourcePath);
        // The registration stamp is read before the lock (M7R R5b.2: no file
        // system call under the watcher's mutex).
        uint64_t statFailures = 0;
        const FileStamp stamp = readStamp(normalized, statFailures);
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot register a source after watcher shutdown.");
        }
        auto [found, inserted] = watched_.try_emplace(
            normalized);
        if (inserted) {
            found->second.stamp = stamp;
            found->second.generation = ++nextGeneration_;
            stats_.statFailures += statFailures;
        }
        const bool ownerInserted =
            found->second.owners.insert(owner).second;
        stats_.watchedFiles =
            static_cast<uint32_t>(watched_.size());
        return inserted || ownerInserted;
    }

    void SourceFileWatcher::unwatchAsset(
        AssetGuid owner) {
        std::lock_guard lock(mutex_);
        for (auto watched = watched_.begin();
            watched != watched_.end();) {
            watched->second.owners.erase(owner);
            if (watched->second.owners.empty()) {
                watched = watched_.erase(watched);
            } else {
                ++watched;
            }
        }
        stats_.watchedFiles =
            static_cast<uint32_t>(watched_.size());
    }

    void SourceFileWatcher::clear() {
        std::lock_guard lock(mutex_);
        watched_.clear();
        events_.clear();
        stats_.watchedFiles = 0;
        stats_.pendingEvents = 0;
    }

    void SourceFileWatcher::scanNow() {
        // M7R R5b.2: the scan copies the watched paths, stats them without the
        // mutex, then applies the results to entries that were not re-watched
        // in between (same generation).
        struct ScanEntry {
            std::filesystem::path path;
            uint64_t generation = 0;
            FileStamp stamp;
        };
        std::vector<ScanEntry> scanned;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            ++stats_.scans;
            scanned.reserve(watched_.size());
            for (const auto& [path, watched] : watched_) {
                scanned.push_back({ path, watched.generation, {} });
            }
        }
        const uint64_t eventTime =
            monotonicNanoseconds();
        uint64_t statFailures = 0;
        for (ScanEntry& entry : scanned) {
            entry.stamp = readStamp(entry.path, statFailures);
        }
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        stats_.statFailures += statFailures;
        for (const ScanEntry& entry : scanned) {
            const auto found = watched_.find(entry.path);
            if (found == watched_.end() ||
                found->second.generation != entry.generation) {
                continue;
            }
            const std::filesystem::path& path = found->first;
            WatchedFile& watched = found->second;
            const FileStamp& current = entry.stamp;
            if (current == watched.stamp) {
                continue;
            }
            if (current.exists !=
                watched.stamp.exists) {
                ++stats_.missingTransitions;
            }
            watched.stamp = current;
            for (const AssetGuid owner :
                watched.owners) {
                events_.push_back({
                    .assetGuid = owner,
                    .sourcePath = path,
                    .eventNanoseconds = eventTime,
                });
                ++stats_.changes;
            }
        }
        stats_.pendingEvents =
            static_cast<uint32_t>(events_.size());
    }

    std::vector<SourceFileChangeEvent>
        SourceFileWatcher::drainEvents() {
        std::lock_guard lock(mutex_);
        std::vector<SourceFileChangeEvent> result;
        result.swap(events_);
        stats_.pendingEvents = 0;
        return result;
    }

    SourceFileWatcherStats
        SourceFileWatcher::stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    void SourceFileWatcher::shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
        }
        if (periodic_) {
            // Unregisters from the frame tick and waits for a scan in flight.
            periodic_->stop();
        }
    }

    std::filesystem::path
        SourceFileWatcher::normalizePath(
            const std::filesystem::path& path) {
        std::error_code error;
        std::filesystem::path absolute =
            std::filesystem::absolute(path, error);
        if (error) {
            throw std::invalid_argument(
                "Source watch path could not be made absolute: " +
                error.message());
        }
        return absolute.lexically_normal();
    }

    SourceFileWatcher::FileStamp
        SourceFileWatcher::readStamp(
            const std::filesystem::path& path,
            uint64_t& statFailures) {
        std::error_code error;
        const bool exists =
            std::filesystem::is_regular_file(
                path, error);
        if (error) {
            ++statFailures;
            return {};
        }
        if (!exists) return {};
        const uint64_t size =
            std::filesystem::file_size(path, error);
        if (error) {
            ++statFailures;
            return {};
        }
        const auto lastWrite =
            std::filesystem::last_write_time(
                path, error);
        if (error) {
            ++statFailures;
            return {};
        }
        return {
            .exists = true,
            .size = size,
            .lastWrite = lastWrite,
        };
    }

} // namespace Iridium
