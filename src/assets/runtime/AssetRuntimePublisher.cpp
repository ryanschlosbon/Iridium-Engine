#include "assets/runtime/AssetRuntimePublisher.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium {

    AssetRuntimePublisher::~AssetRuntimePublisher() {
        shutdown();
    }

    bool AssetRuntimePublisher::enqueue(
        RuntimeAssetPublishRequest request) {
        if (request.assetGuid.isNil() ||
            request.cookKey.empty() ||
            !request.publish) {
            throw std::invalid_argument(
                "Runtime publication requires a stable asset GUID, cook key, and callback.");
        }
        Entry& entry = entries_[request.assetGuid];
        if ((entry.published &&
                entry.published->cookKey == request.cookKey) ||
            (entry.queued &&
                entry.queued->cookKey == request.cookKey)) {
            ++stats_.unchanged;
            return false;
        }
        if (entry.queued) {
            ++stats_.coalesced;
        } else {
            queuedAssets_.insert(request.assetGuid);
            ++stats_.queued;
        }
        entry.queued = std::move(request);
        entry.state = RuntimeAssetState::Queued;
        entry.diagnostic.clear();
        ++stats_.enqueued;
        return true;
    }

    void AssetRuntimePublisher::adoptPublished(
        AssetGuid assetGuid,
        std::string cookKey,
        uint64_t cpuResidentBytes,
        uint64_t gpuResidentBytes) {
        if (assetGuid.isNil() ||
            cookKey.empty()) {
            throw std::invalid_argument(
                "Runtime adoption requires a stable GUID and cook key.");
        }
        Entry& entry = entries_[assetGuid];
        if (entry.published) {
            if (entry.published->cookKey ==
                cookKey) {
                return;
            }
            throw std::logic_error(
                "Cannot adopt over an existing runtime revision.");
        }
        cancelQueued(assetGuid);
        if (entry.revisionCounter ==
            UINT64_MAX) {
            throw std::overflow_error(
                "Runtime asset revision counter is exhausted.");
        }
        const uint64_t revision =
            ++entry.revisionCounter;
        entry.published = PublishedRevision{
            .revision = revision,
            .cookKey = std::move(cookKey),
            .cpuResidentBytes =
                cpuResidentBytes,
            .gpuResidentBytes =
                gpuResidentBytes,
        };
        entry.state = RuntimeAssetState::Ready;
        entry.diagnostic.clear();
        ++stats_.resident;
        stats_.cpuResidentBytes +=
            cpuResidentBytes;
        stats_.gpuResidentBytes +=
            gpuResidentBytes;
    }

    void AssetRuntimePublisher::reportFailure(
        AssetGuid assetGuid, std::string diagnostic) {
        if (assetGuid.isNil() || diagnostic.empty()) {
            throw std::invalid_argument(
                "Runtime failure reporting requires a stable GUID and diagnostic.");
        }
        Entry& entry = entries_[assetGuid];
        if (entry.queued) {
            entry.queued.reset();
            queuedAssets_.erase(assetGuid);
            --stats_.queued;
        }
        entry.diagnostic = std::move(diagnostic);
        entry.state = entry.published
            ? RuntimeAssetState::ReadyWithError
            : RuntimeAssetState::Failed;
        ++stats_.failed;
    }

    void AssetRuntimePublisher::cancelQueued(
        AssetGuid assetGuid) {
        const auto found =
            entries_.find(assetGuid);
        if (found == entries_.end() ||
            !found->second.queued) {
            return;
        }
        found->second.queued.reset();
        queuedAssets_.erase(assetGuid);
        --stats_.queued;
        found->second.state =
            found->second.published
                ? RuntimeAssetState::Ready
                : RuntimeAssetState::Missing;
    }

    RuntimePublishTickResult AssetRuntimePublisher::tick(
        uint64_t uploadBudgetBytes) {
        RuntimePublishTickResult result{
            .uploadBudgetBytes = uploadBudgetBytes,
        };
        uint64_t remaining = uploadBudgetBytes;
        for (auto queued = queuedAssets_.begin();
            queued != queuedAssets_.end();) {
            Entry& entry = entries_.at(*queued);
            const uint64_t requestBytes =
                entry.queued->estimatedUploadBytes;
            const bool allowedOversized = requestBytes > remaining &&
                result.scheduledUploadBytes == 0u &&
                entry.queued->allowSingleOversizedUpload;
            if (requestBytes > remaining && !allowedOversized) {
                ++result.deferredByBudget;
                ++queued;
                continue;
            }

            RuntimeAssetPublishRequest request =
                std::move(*entry.queued);
            entry.queued.reset();
            queued = queuedAssets_.erase(queued);
            --stats_.queued;
            if (allowedOversized) {
                remaining = 0;
                ++result.oversizedPublications;
            } else {
                remaining -= requestBytes;
            }
            result.scheduledUploadBytes += requestBytes;
            stats_.scheduledUploadBytes += requestBytes;

            RuntimeAssetPublishOutcome outcome;
            try {
                outcome = request.publish();
            } catch (const std::exception& exception) {
                outcome.diagnostic = exception.what();
            } catch (...) {
                outcome.diagnostic =
                    "Runtime publication threw an unknown exception.";
            }
            if (!outcome.succeeded) {
                if (outcome.retire) {
                    PublishedRevision rejected{
                        .retire =
                            std::move(outcome.retire),
                    };
                    retire(rejected);
                }
                if (outcome.diagnostic.empty()) {
                    outcome.diagnostic =
                        "Runtime publication failed without a diagnostic.";
                }
                entry.diagnostic =
                    std::move(outcome.diagnostic);
                entry.state = entry.published
                    ? RuntimeAssetState::ReadyWithError
                    : RuntimeAssetState::Failed;
                ++result.failed;
                ++stats_.failed;
                continue;
            }

            if (entry.revisionCounter ==
                UINT64_MAX) {
                if (outcome.retire) {
                    PublishedRevision rejected{
                        .retire =
                            std::move(outcome.retire),
                    };
                    retire(rejected);
                }
                entry.diagnostic =
                    "Runtime asset revision counter is exhausted.";
                entry.state = entry.published
                    ? RuntimeAssetState::ReadyWithError
                    : RuntimeAssetState::Failed;
                ++result.failed;
                ++stats_.failed;
                continue;
            }
            const uint64_t nextRevision =
                ++entry.revisionCounter;
            if (entry.published) {
                stats_.cpuResidentBytes -=
                    entry.published->cpuResidentBytes;
                stats_.gpuResidentBytes -=
                    entry.published->gpuResidentBytes;
                retire(*entry.published);
            } else {
                ++stats_.resident;
            }
            entry.published = PublishedRevision{
                .revision = nextRevision,
                .cookKey = std::move(request.cookKey),
                .cpuResidentBytes =
                    outcome.cpuResidentBytes,
                .gpuResidentBytes =
                    outcome.gpuResidentBytes,
                .retire = std::move(outcome.retire),
            };
            entry.state = RuntimeAssetState::Ready;
            entry.diagnostic.clear();
            stats_.cpuResidentBytes +=
                outcome.cpuResidentBytes;
            stats_.gpuResidentBytes +=
                outcome.gpuResidentBytes;
            ++result.published;
            ++stats_.published;
        }
        result.queuedAfterTick = stats_.queued;
        return result;
    }

    void AssetRuntimePublisher::setPinned(
        AssetGuid assetGuid, bool pinned) {
        if (assetGuid.isNil()) {
            throw std::invalid_argument(
                "Cannot pin a nil asset GUID.");
        }
        Entry& entry = entries_[assetGuid];
        if (entry.pinned == pinned) return;
        entry.pinned = pinned;
        if (pinned) ++stats_.pinned;
        else --stats_.pinned;
    }

    void AssetRuntimePublisher::touch(
        AssetGuid assetGuid, uint64_t serial) {
        const auto found = entries_.find(assetGuid);
        if (found == entries_.end()) return;
        found->second.lastUsedSerial =
            std::max(found->second.lastUsedSerial,
                serial);
    }

    RuntimeResidencyResult
        AssetRuntimePublisher::evictToGpuBudget(
            uint64_t budgetBytes) {
        RuntimeResidencyResult result{
            .budgetBytes = budgetBytes,
            .residentBytesBefore =
                stats_.gpuResidentBytes,
            .residentBytesAfter =
                stats_.gpuResidentBytes,
        };
        struct Candidate {
            AssetGuid assetGuid;
            uint64_t lastUsedSerial = 0;
        };
        std::vector<Candidate> candidates;
        for (const auto& [assetGuid, entry] : entries_) {
            if (entry.published && !entry.pinned) {
                candidates.push_back({
                    assetGuid, entry.lastUsedSerial,
                });
            }
        }
        std::ranges::sort(candidates,
            [](const Candidate& left,
                const Candidate& right) {
                if (left.lastUsedSerial !=
                    right.lastUsedSerial) {
                    return left.lastUsedSerial <
                        right.lastUsedSerial;
                }
                return left.assetGuid <
                    right.assetGuid;
            });
        for (const Candidate& candidate : candidates) {
            if (result.residentBytesAfter <=
                budgetBytes) {
                break;
            }
            Entry& entry = entries_.at(
                candidate.assetGuid);
            result.residentBytesAfter -=
                entry.published->gpuResidentBytes;
            stats_.cpuResidentBytes -=
                entry.published->cpuResidentBytes;
            stats_.gpuResidentBytes -=
                entry.published->gpuResidentBytes;
            --stats_.resident;
            retire(*entry.published);
            entry.published.reset();
            entry.state = RuntimeAssetState::Evicted;
            ++result.evicted;
            ++stats_.evicted;
        }
        result.budgetSatisfied =
            result.residentBytesAfter <= budgetBytes;
        result.residentBytesAfter =
            stats_.gpuResidentBytes;
        return result;
    }

    std::optional<RuntimeAssetSnapshot>
        AssetRuntimePublisher::snapshot(
            AssetGuid assetGuid) const {
        const auto found = entries_.find(assetGuid);
        if (found == entries_.end()) {
            return std::nullopt;
        }
        return makeSnapshot(
            assetGuid, found->second);
    }

    std::vector<RuntimeAssetSnapshot>
        AssetRuntimePublisher::snapshots() const {
        std::vector<RuntimeAssetSnapshot> result;
        result.reserve(entries_.size());
        for (const auto& [assetGuid, entry] :
            entries_) {
            result.push_back(makeSnapshot(
                assetGuid, entry));
        }
        return result;
    }

    const AssetRuntimePublisherStats&
        AssetRuntimePublisher::stats() const noexcept {
        return stats_;
    }

    void AssetRuntimePublisher::shutdown() noexcept {
        for (auto& [assetGuid, entry] : entries_) {
            (void)assetGuid;
            if (entry.published) {
                retire(*entry.published);
                entry.published.reset();
            }
        }
        entries_.clear();
        queuedAssets_.clear();
        stats_.cpuResidentBytes = 0;
        stats_.gpuResidentBytes = 0;
        stats_.queued = 0;
        stats_.resident = 0;
        stats_.pinned = 0;
    }

    void AssetRuntimePublisher::retire(
        PublishedRevision& revision) noexcept {
        if (revision.retire) {
            try {
                revision.retire();
            } catch (...) {
                // Retirement is a cleanup boundary and cannot unwind through
                // a newer successfully published revision or shutdown.
            }
        }
        ++stats_.retired;
    }

    RuntimeAssetSnapshot
        AssetRuntimePublisher::makeSnapshot(
            AssetGuid assetGuid,
            const Entry& entry) const {
        RuntimeAssetSnapshot result{
            .assetGuid = assetGuid,
            .state = entry.state,
            .revision = entry.revisionCounter,
            .diagnostic = entry.diagnostic,
            .lastUsedSerial = entry.lastUsedSerial,
            .pinned = entry.pinned,
            .hasPublishedRevision =
                entry.published.has_value(),
        };
        if (entry.published) {
            result.cookKey =
                entry.published->cookKey;
            result.cpuResidentBytes =
                entry.published->cpuResidentBytes;
            result.gpuResidentBytes =
                entry.published->gpuResidentBytes;
        }
        if (entry.queued) {
            result.pendingCookKey =
                entry.queued->cookKey;
        }
        return result;
    }

} // namespace Iridium
