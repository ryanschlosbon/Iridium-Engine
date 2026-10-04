#pragma once

#include "core/types/AssetGuid.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace Iridium {

    namespace Tasks {
        class Periodic;
        class TaskSystem;
    }

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
        // startWorker scans automatically every scanInterval: a Periodic on the
        // task system's pinned I/O thread (M7R R5b.2), which `tasks` must
        // provide. Without it, scanNow() is the only scan.
        explicit SourceFileWatcher(
            std::chrono::milliseconds scanInterval =
                std::chrono::milliseconds(250),
            bool startWorker = true,
            Tasks::TaskSystem* tasks = nullptr);
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
        // periodic scan so filesystem queries never run on the frame tick. The
        // stat calls run outside the watcher's mutex.
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
            // Distinguishes a re-watched path from the entry a scan read.
            uint64_t generation = 0;
        };

        [[nodiscard]] static std::filesystem::path
            normalizePath(
                const std::filesystem::path& path);
        [[nodiscard]] static FileStamp readStamp(
            const std::filesystem::path& path,
            uint64_t& statFailures);

        std::chrono::milliseconds scanInterval_;
        mutable std::mutex mutex_;
        std::map<std::filesystem::path, WatchedFile>
            watched_;
        std::vector<SourceFileChangeEvent> events_;
        SourceFileWatcherStats stats_;
        uint64_t nextGeneration_ = 0;
        bool shutdown_ = false;
        // Declared last: stopped in shutdown() before the state above goes.
        std::unique_ptr<Tasks::Periodic> periodic_;
    };

} // namespace Iridium
