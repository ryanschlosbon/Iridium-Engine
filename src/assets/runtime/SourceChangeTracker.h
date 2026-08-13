#pragma once

#include "assets/cooker/DependencyGraph.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Iridium {

    struct SourceChangeDiagnostic {
        AssetGuid assetGuid;
        std::filesystem::path sourcePath;
        std::string code;
        std::string message;
    };

    struct SourceContentChange {
        AssetGuid assetGuid;
        std::filesystem::path sourcePath;
        std::string previousHash;
        std::string contentHash;
    };

    struct SourceChangeBatch {
        std::vector<SourceContentChange> changedSources;
        std::vector<AssetGuid> changedAssets;
        std::vector<AssetGuid> invalidatedAssets;
        std::vector<AssetGuid> rebuildOrder;
        std::vector<DependencyCycle> blockingCycles;
        std::vector<SourceChangeDiagnostic> diagnostics;
        uint32_t sameContentEvents = 0;

        [[nodiscard]] bool blocked() const noexcept {
            return !blockingCycles.empty();
        }
    };

    struct SourceChangeTrackerStats {
        uint64_t notifications = 0;
        uint64_t coalesced = 0;
        uint64_t contentChanges = 0;
        uint64_t sameContent = 0;
        uint64_t hashFailures = 0;
        uint64_t batches = 0;
        uint32_t pending = 0;
    };

    class SourceChangeTracker {
    public:
        using ContentHasher =
            std::function<std::string(
                const std::filesystem::path&)>;

        explicit SourceChangeTracker(
            uint64_t debounceNanoseconds);

        void seedContentHash(
            AssetGuid assetGuid,
            std::filesystem::path sourcePath,
            std::string hash);
        void notify(
            AssetGuid assetGuid,
            std::filesystem::path sourcePath,
            uint64_t eventNanoseconds);
        void removeAsset(AssetGuid assetGuid);

        [[nodiscard]] SourceChangeBatch poll(
            uint64_t nowNanoseconds,
            const AssetDependencyGraph& dependencies,
            const ContentHasher& hasher);

        [[nodiscard]] const SourceChangeTrackerStats&
            stats() const noexcept;

    private:
        struct SourceKey {
            AssetGuid assetGuid;
            std::filesystem::path sourcePath;

            auto operator<=>(const SourceKey&) const =
                default;
        };

        struct PendingChange {
            uint64_t lastEventNanoseconds = 0;
        };

        uint64_t debounceNanoseconds_ = 0;
        std::map<SourceKey, std::string> contentHashes_;
        std::map<SourceKey, PendingChange> pending_;
        SourceChangeTrackerStats stats_;
    };

} // namespace Iridium
