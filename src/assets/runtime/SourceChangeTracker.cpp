#include "assets/runtime/SourceChangeTracker.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>

namespace Iridium {

    namespace {

        bool validSha256(std::string_view hash) {
            return hash.size() == 64 &&
                std::ranges::all_of(hash,
                    [](unsigned char value) {
                        return std::isdigit(value) ||
                            (value >= 'a' &&
                                value <= 'f');
                    });
        }

        bool cycleIntersects(
            const DependencyCycle& cycle,
            const std::set<AssetGuid>& assets) {
            return std::ranges::any_of(cycle.chain,
                [&assets](AssetGuid asset) {
                    return assets.contains(asset);
                });
        }

        std::vector<AssetGuid> dependencyFirstOrder(
            std::span<const AssetGuid> assets,
            const AssetDependencyGraph& graph) {
            const std::set<AssetGuid> included(
                assets.begin(), assets.end());
            std::map<AssetGuid, uint32_t> incoming;
            std::map<AssetGuid, std::vector<AssetGuid>>
                dependents;
            for (const AssetGuid asset : included) {
                incoming.emplace(asset, 0);
            }
            for (const AssetGuid asset : included) {
                std::set<AssetGuid>
                    directAssetDependencies;
                for (const AssetDependency& dependency :
                    graph.directDependencies(asset)) {
                    if (!dependency.assetGuid ||
                        (dependency.type !=
                            AssetDependencyType::Asset &&
                         dependency.type !=
                            AssetDependencyType::OptionalAsset) ||
                        !included.contains(
                            *dependency.assetGuid)) {
                        continue;
                    }
                    directAssetDependencies.insert(
                        *dependency.assetGuid);
                }
                for (const AssetGuid dependency :
                    directAssetDependencies) {
                    ++incoming[asset];
                    dependents[dependency]
                        .push_back(asset);
                }
            }
            for (auto& [dependency, values] :
                dependents) {
                (void)dependency;
                std::ranges::sort(values);
                values.erase(std::unique(
                    values.begin(), values.end()),
                    values.end());
            }
            std::set<AssetGuid> ready;
            for (const auto& [asset, count] : incoming) {
                if (count == 0) ready.insert(asset);
            }
            std::vector<AssetGuid> result;
            result.reserve(included.size());
            while (!ready.empty()) {
                const AssetGuid asset = *ready.begin();
                ready.erase(ready.begin());
                result.push_back(asset);
                const auto found = dependents.find(asset);
                if (found == dependents.end()) continue;
                for (const AssetGuid dependent :
                    found->second) {
                    uint32_t& count = incoming[dependent];
                    if (--count == 0) {
                        ready.insert(dependent);
                    }
                }
            }
            return result;
        }

    } // namespace

    SourceChangeTracker::SourceChangeTracker(
        uint64_t debounceNanoseconds)
        : debounceNanoseconds_(
            debounceNanoseconds) {}

    void SourceChangeTracker::seedContentHash(
        AssetGuid assetGuid,
        std::filesystem::path sourcePath,
        std::string hash) {
        if (assetGuid.isNil() || sourcePath.empty() ||
            !validSha256(hash)) {
            throw std::invalid_argument(
                "Source hash seed requires a stable GUID, path, and lowercase SHA-256.");
        }
        contentHashes_[{
            assetGuid,
            sourcePath.lexically_normal(),
        }] =
            std::move(hash);
    }

    void SourceChangeTracker::notify(
        AssetGuid assetGuid,
        std::filesystem::path sourcePath,
        uint64_t eventNanoseconds) {
        if (assetGuid.isNil() || sourcePath.empty()) {
            throw std::invalid_argument(
                "Source change requires a stable GUID and source path.");
        }
        const SourceKey key{
            assetGuid,
            sourcePath.lexically_normal(),
        };
        const auto found = pending_.find(key);
        if (found == pending_.end()) {
            pending_.emplace(
                key, PendingChange{
                .lastEventNanoseconds =
                    eventNanoseconds,
            });
        } else {
            found->second.lastEventNanoseconds =
                std::max(
                    found->second.lastEventNanoseconds,
                    eventNanoseconds);
            ++stats_.coalesced;
        }
        ++stats_.notifications;
        stats_.pending =
            static_cast<uint32_t>(pending_.size());
    }

    void SourceChangeTracker::removeAsset(
        AssetGuid assetGuid) {
        std::erase_if(contentHashes_,
            [assetGuid](const auto& entry) {
                return entry.first.assetGuid ==
                    assetGuid;
            });
        std::erase_if(pending_,
            [assetGuid](const auto& entry) {
                return entry.first.assetGuid ==
                    assetGuid;
            });
        stats_.pending =
            static_cast<uint32_t>(pending_.size());
        if (pollInFlight_) {
            removedDuringPoll_.insert(assetGuid);
        }
    }

    SourceChangeBatch SourceChangeTracker::poll(
        uint64_t nowNanoseconds,
        const AssetDependencyGraph& dependencies,
        const ContentHasher& hasher) {
        if (!hasher) {
            throw std::invalid_argument(
                "Source change polling requires a content hasher.");
        }
        std::vector<DueSourceChange> due =
            takeDue(nowNanoseconds);
        hashDueSources(due, hasher);
        return completePoll(due, dependencies);
    }

    std::vector<SourceChangeTracker::DueSourceChange>
        SourceChangeTracker::takeDue(
            uint64_t nowNanoseconds) {
        std::vector<DueSourceChange> due;
        for (auto pending = pending_.begin();
            pending != pending_.end();) {
            const uint64_t eventTime =
                pending->second.lastEventNanoseconds;
            if (nowNanoseconds < eventTime ||
                nowNanoseconds - eventTime <
                    debounceNanoseconds_) {
                ++pending;
                continue;
            }
            due.push_back({
                .assetGuid = pending->first.assetGuid,
                .sourcePath = pending->first.sourcePath,
            });
            pending = pending_.erase(pending);
        }
        stats_.pending =
            static_cast<uint32_t>(pending_.size());
        pollInFlight_ = true;
        removedDuringPoll_.clear();
        return due;
    }

    void SourceChangeTracker::hashDueSources(
        std::span<DueSourceChange> due,
        const ContentHasher& hasher) {
        if (!hasher) {
            throw std::invalid_argument(
                "Source change polling requires a content hasher.");
        }
        for (DueSourceChange& source : due) {
            try {
                std::string hash = hasher(source.sourcePath);
                if (!validSha256(hash)) {
                    throw std::runtime_error(
                        "Content hasher returned an invalid SHA-256.");
                }
                source.contentHash = std::move(hash);
            } catch (const std::exception& exception) {
                source.hashError = exception.what();
                if (source.hashError.empty()) {
                    source.hashError = "Content hashing failed.";
                }
            }
        }
    }

    SourceChangeBatch SourceChangeTracker::completePoll(
        std::span<const DueSourceChange> due,
        const AssetDependencyGraph& dependencies) {
        SourceChangeBatch result;
        for (const DueSourceChange& source : due) {
            // An asset untracked while its sources were hashed is gone.
            if (removedDuringPoll_.contains(source.assetGuid)) {
                continue;
            }
            const AssetGuid assetGuid = source.assetGuid;
            const std::filesystem::path& sourcePath =
                source.sourcePath;
            if (!source.hashError.empty()) {
                result.diagnostics.push_back({
                    .assetGuid = assetGuid,
                    .sourcePath = sourcePath,
                    .code = "ASSET_SOURCE_HASH_FAILED",
                    .message = source.hashError,
                });
                ++stats_.hashFailures;
                continue;
            }
            const SourceKey sourceKey{
                assetGuid,
                sourcePath,
            };
            std::string hash = source.contentHash;
            const auto known =
                contentHashes_.find(sourceKey);
            if (known != contentHashes_.end() &&
                known->second == hash) {
                ++result.sameContentEvents;
                ++stats_.sameContent;
                continue;
            }
            std::string previousHash;
            if (known != contentHashes_.end()) {
                previousHash = known->second;
                known->second = hash;
            } else {
                contentHashes_.emplace(
                    sourceKey, hash);
            }
            result.changedSources.push_back({
                .assetGuid = assetGuid,
                .sourcePath = sourcePath,
                .previousHash =
                    std::move(previousHash),
                .contentHash = std::move(hash),
            });
            ++stats_.contentChanges;
        }
        pollInFlight_ = false;
        removedDuringPoll_.clear();
        stats_.pending =
            static_cast<uint32_t>(pending_.size());
        if (result.changedSources.empty()) {
            return result;
        }
        std::set<AssetGuid> changedAssets;
        for (const SourceContentChange& change :
            result.changedSources) {
            changedAssets.insert(change.assetGuid);
        }
        result.changedAssets.assign(
            changedAssets.begin(),
            changedAssets.end());
        ++stats_.batches;
        result.invalidatedAssets =
            dependencies.invalidationClosure(
                result.changedAssets);
        const std::set<AssetGuid> invalidated(
            result.invalidatedAssets.begin(),
            result.invalidatedAssets.end());
        for (const DependencyCycle& cycle :
            dependencies.cycles()) {
            if (cycleIntersects(cycle, invalidated)) {
                result.blockingCycles.push_back(cycle);
            }
        }
        if (!result.blocked()) {
            result.rebuildOrder =
                dependencyFirstOrder(
                    result.invalidatedAssets,
                    dependencies);
            if (result.rebuildOrder.size() !=
                result.invalidatedAssets.size()) {
                result.diagnostics.push_back({
                    .code =
                        "ASSET_DEPENDENCY_ORDER_FAILED",
                    .message =
                        "Invalidation closure could not be ordered dependency-first.",
                });
                result.rebuildOrder.clear();
            }
        } else {
            result.diagnostics.push_back({
                .code = "ASSET_DEPENDENCY_CYCLE",
                .message =
                    "Invalidation closure contains an asset dependency cycle.",
            });
        }
        return result;
    }

    const SourceChangeTrackerStats&
        SourceChangeTracker::stats() const noexcept {
        return stats_;
    }

} // namespace Iridium
