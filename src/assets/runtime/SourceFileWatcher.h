#pragma once

#include "assets/AssetGuid.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

namespace Iridium {

    struct SourceFileChangeEvent {
        AssetGuid assetGuid;
        std::filesystem::path sourcePath;
        uint64_t eventNanoseconds = 0;
    };

    struct SourceFileWatcherStats {
        uint64_t scans = 0;
        uint64_t changes = 0;
        uint64_t missingTransitions = 0;
        uint64_t statFailures = 0;
        uint32_t watchedFiles = 0;
        uint32_t pendingEvents = 0;
    };

    class SourceFileWatcher {
    public:
        explicit SourceFileWatcher(
            std::chrono::milliseconds scanInterval =
                std::chrono::milliseconds(250),
            bool startWorker = true);
        ~SourceFileWatcher();

        SourceFileWatcher(const SourceFileWatcher&) = delete;
        SourceFileWatcher& operator=(
            const SourceFileWatcher&) = delete;

        // One source/dependency path may invalidate multiple owning assets.
        // Registration captures the current state and never emits an initial
        // synthetic change event.
        bool watch(
            AssetGuid owner,
            const std::filesystem::path& sourcePath);
        void unwatchAsset(AssetGuid owner);
        void clear();

        // Used by deterministic tests and tools. The application uses the
        // background worker so filesystem queries never run on the frame tick.
        void scanNow();
        [[nodiscard]] std::vector<SourceFileChangeEvent>
            drainEvents();
        [[nodiscard]] SourceFileWatcherStats stats() const;

        void shutdown() noexcept;

    private:
        struct FileStamp {
            bool exists = false;
            uint64_t size = 0;
            std::filesystem::file_time_type lastWrite{};

            auto operator<=>(const FileStamp&) const = default;
        };

        struct WatchedFile {
            FileStamp stamp;
            std::set<AssetGuid> owners;
        };

        [[nodiscard]] static std::filesystem::path
            normalizePath(
                const std::filesystem::path& path);
        [[nodiscard]] FileStamp readStamp(
            const std::filesystem::path& path);
        void workerLoop(std::stop_token stopToken);

        std::chrono::milliseconds scanInterval_;
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::map<std::filesystem::path, WatchedFile>
            watched_;
        std::vector<SourceFileChangeEvent> events_;
        SourceFileWatcherStats stats_;
        bool shutdown_ = false;
        std::jthread worker_;
    };

} // namespace Iridium
