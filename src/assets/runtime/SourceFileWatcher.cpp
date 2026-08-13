#include "assets/runtime/SourceFileWatcher.h"

#include <stdexcept>
#include <system_error>

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
        bool startWorker)
        : scanInterval_(scanInterval) {
        if (scanInterval_.count() <= 0) {
            throw std::invalid_argument(
                "Source watcher interval must be positive.");
        }
        if (startWorker) {
            worker_ = std::jthread(
                [this](std::stop_token stopToken) {
                    workerLoop(stopToken);
                });
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
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot register a source after watcher shutdown.");
        }
        auto [found, inserted] = watched_.try_emplace(
            normalized);
        if (inserted) {
            found->second.stamp =
                readStamp(normalized);
        }
        const bool ownerInserted =
            found->second.owners.insert(owner).second;
        stats_.watchedFiles =
            static_cast<uint32_t>(watched_.size());
        condition_.notify_all();
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
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        ++stats_.scans;
        const uint64_t eventTime =
            monotonicNanoseconds();
        for (auto& [path, watched] : watched_) {
            const FileStamp current =
                readStamp(path);
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
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
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
            const std::filesystem::path& path) {
        std::error_code error;
        const bool exists =
            std::filesystem::is_regular_file(
                path, error);
        if (error) {
            ++stats_.statFailures;
            return {};
        }
        if (!exists) return {};
        const uint64_t size =
            std::filesystem::file_size(path, error);
        if (error) {
            ++stats_.statFailures;
            return {};
        }
        const auto lastWrite =
            std::filesystem::last_write_time(
                path, error);
        if (error) {
            ++stats_.statFailures;
            return {};
        }
        return {
            .exists = true,
            .size = size,
            .lastWrite = lastWrite,
        };
    }

    void SourceFileWatcher::workerLoop(
        std::stop_token stopToken) {
        std::unique_lock lock(mutex_);
        while (!shutdown_ &&
            !stopToken.stop_requested()) {
            condition_.wait_for(
                lock, stopToken, scanInterval_,
                [this] { return shutdown_; });
            if (shutdown_ ||
                stopToken.stop_requested()) {
                return;
            }
            lock.unlock();
            scanNow();
            lock.lock();
        }
    }

} // namespace Iridium
