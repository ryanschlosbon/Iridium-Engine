#include "assets/runtime/AssetRuntimeService.h"

#include "utils/Sha256.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace Iridium {

    AssetRuntimeService::AssetRuntimeService(
        AssetRuntimeServiceConfig config,
        AssetSourceMonitor::ContentHasher hasher)
        : config_(config),
          sourceMonitor_(
              config.debounceNanoseconds,
              config.scanInterval,
              std::move(hasher),
              config.startSourceWorkers) {}

    AssetRuntimeService::~AssetRuntimeService() {
        shutdown();
    }

    void AssetRuntimeService::track(
        TrackedRuntimeAsset asset) {
        if (shutdown_) {
            throw std::logic_error(
                "Cannot track an asset after runtime service shutdown.");
        }
        if (asset.assetGuid.isNil() ||
            asset.sources.empty() ||
            !asset.prepare) {
            throw std::invalid_argument(
                "Runtime tracking requires a stable GUID, sources, and prepare callback.");
        }
        sourceMonitor_.trackAsset(
            asset.assetGuid,
            asset.sources,
            std::move(asset.dependencies));
        preparers_[asset.assetGuid] =
            std::move(asset.prepare);
        publisher_.setPinned(
            asset.assetGuid, asset.pinned);
    }

    void AssetRuntimeService::untrack(
        AssetGuid assetGuid) {
        sourceMonitor_.untrackAsset(assetGuid);
        preparers_.erase(assetGuid);
        reimport_.cancel(assetGuid);
        publisher_.cancelQueued(assetGuid);
        publisher_.setPinned(
            assetGuid, false);
    }

    void AssetRuntimeService::adoptPublished(
        AssetGuid assetGuid,
        std::string cookKey,
        uint64_t cpuResidentBytes,
        uint64_t gpuResidentBytes) {
        publisher_.adoptPublished(
            assetGuid, std::move(cookKey),
            cpuResidentBytes,
            gpuResidentBytes);
    }

    bool AssetRuntimeService::enqueuePrepared(
        AssetGuid assetGuid,
        PreparedRuntimeAsset prepared) {
        if (shutdown_) {
            throw std::logic_error(
                "Cannot enqueue prepared work after runtime service shutdown.");
        }
        return publisher_.enqueue({
            .assetGuid = assetGuid,
            .cookKey = std::move(prepared.cookKey),
            .estimatedUploadBytes =
                prepared.estimatedUploadBytes,
            .allowSingleOversizedUpload =
                prepared.allowSingleOversizedUpload,
            .publish = std::move(prepared.publish),
        });
    }

    void AssetRuntimeService::reportFailure(
        AssetGuid assetGuid,
        std::string diagnostic) {
        if (shutdown_) {
            throw std::logic_error(
                "Cannot report a runtime failure after service shutdown.");
        }
        publisher_.reportFailure(
            assetGuid, std::move(diagnostic));
    }

    bool AssetRuntimeService::requestReimport(
        AssetGuid assetGuid) {
        if (shutdown_) {
            throw std::logic_error(
                "Cannot request reimport after runtime service shutdown.");
        }
        const auto preparer = preparers_.find(assetGuid);
        if (preparer == preparers_.end()) return false;
        if (manualRequestSerial_ == UINT64_MAX) {
            throw std::overflow_error(
                "Manual reimport request serial is exhausted.");
        }
        const uint64_t serial = ++manualRequestSerial_;
        return reimport_.enqueue({
            .assetGuid = assetGuid,
            .requestKey = "manual:" + std::to_string(serial),
            .prepare =
                [prepare = preparer->second,
                    cause = AssetReimportCause{
                        .assetGuid = assetGuid,
                    }](std::stop_token stopToken) {
                    return prepare(cause, stopToken);
                },
        });
    }

    AssetRuntimeServiceTick
        AssetRuntimeService::tick() {
        if (shutdown_) {
            throw std::logic_error(
                "Cannot tick a shut down asset runtime service.");
        }
        AssetRuntimeServiceTick result;
        for (const SourceChangeBatch& batch :
            sourceMonitor_.drainBatches()) {
            ++result.changeBatches;
            result.sourceDiagnostics +=
                static_cast<uint32_t>(
                    batch.diagnostics.size());
            if (batch.blocked()) {
                ++result.blockedBatches;
                for (const AssetGuid assetGuid :
                    batch.invalidatedAssets) {
                    publisher_.reportFailure(
                        assetGuid,
                        "Asset rebuild blocked by a dependency cycle.");
                }
                continue;
            }
            for (const AssetGuid assetGuid :
                batch.rebuildOrder) {
                const auto preparer =
                    preparers_.find(assetGuid);
                if (preparer ==
                    preparers_.end()) {
                    publisher_.reportFailure(
                        assetGuid,
                        "Invalidated asset has no registered reimport preparer.");
                    ++result.missingPreparers;
                    continue;
                }
                const bool enqueued =
                    reimport_.enqueue({
                        .assetGuid = assetGuid,
                        .requestKey =
                            batchRequestKey(
                                batch,
                                assetGuid),
                        .prepare =
                            [prepare =
                                preparer->second,
                                cause =
                                    AssetReimportCause{
                                        assetGuid,
                                        batch.changedSources,
                                    }](
                                std::stop_token
                                    stopToken) {
                                return prepare(
                                    cause,
                                    stopToken);
                            },
                    });
                if (enqueued) {
                    ++result.rebuildsRequested;
                }
            }
        }
        result.reimport =
            reimport_.drainTo(publisher_);
        result.publication =
            publisher_.tick(
                config_.uploadBudgetBytes);
        if (config_.gpuResidencyBudgetBytes) {
            result.residency =
                publisher_.evictToGpuBudget(
                    *config_
                        .gpuResidencyBudgetBytes);
        }
        return result;
    }

    void AssetRuntimeService::touch(
        AssetGuid assetGuid, uint64_t serial) {
        publisher_.touch(assetGuid, serial);
    }

    void AssetRuntimeService::setPinned(
        AssetGuid assetGuid, bool pinned) {
        publisher_.setPinned(
            assetGuid, pinned);
    }

    std::optional<RuntimeAssetSnapshot>
        AssetRuntimeService::snapshot(
            AssetGuid assetGuid) const {
        return publisher_.snapshot(assetGuid);
    }

    std::vector<RuntimeAssetSnapshot>
        AssetRuntimeService::snapshots() const {
        return publisher_.snapshots();
    }

    AssetRuntimeServiceStats
        AssetRuntimeService::stats() const {
        return {
            .source = sourceMonitor_.stats(),
            .reimport = reimport_.stats(),
            .publisher = publisher_.stats(),
        };
    }

    void AssetRuntimeService::processSourcesOnce(
        uint64_t nowNanoseconds) {
        sourceMonitor_.processOnce(
            nowNanoseconds);
    }

    void AssetRuntimeService::shutdown() noexcept {
        if (shutdown_) return;
        shutdown_ = true;
        sourceMonitor_.shutdown();
        reimport_.shutdown();
        publisher_.shutdown();
        preparers_.clear();
    }

    std::string
        AssetRuntimeService::batchRequestKey(
            const SourceChangeBatch& batch,
            AssetGuid assetGuid) {
        std::string identity =
            assetGuid.toString();
        for (const SourceContentChange& change :
            batch.changedSources) {
            identity += '\n';
            identity +=
                change.assetGuid.toString();
            identity += '\n';
            identity +=
                change.sourcePath.generic_string();
            identity += '\n';
            identity += change.contentHash;
        }
        return sha256(std::as_bytes(
            std::span(identity.data(),
                identity.size())));
    }

} // namespace Iridium
