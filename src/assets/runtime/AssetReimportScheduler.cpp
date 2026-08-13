#include "assets/runtime/AssetReimportScheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium {

    AssetReimportScheduler::AssetReimportScheduler()
        : worker_([this](std::stop_token stopToken) {
            workerLoop(stopToken);
        }) {}

    AssetReimportScheduler::~AssetReimportScheduler() {
        shutdown();
    }

    bool AssetReimportScheduler::enqueue(
        AssetReimportRequest request) {
        if (request.assetGuid.isNil() ||
            request.requestKey.empty() ||
            !request.prepare) {
            throw std::invalid_argument(
                "Asset reimport requires a stable GUID, request key, and prepare callback.");
        }
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot enqueue work after asset reimport shutdown.");
        }
        const auto latest = latestSerial_.find(
            request.assetGuid);
        const auto queued = std::ranges::find_if(
            queued_,
            [&request](const WorkItem& item) {
                return item.request.assetGuid ==
                    request.assetGuid;
            });
        if (queued != queued_.end() &&
            queued->request.requestKey ==
                request.requestKey) {
            ++stats_.coalesced;
            return false;
        }
        if (activeAsset_ == request.assetGuid &&
            activeRequestKey_ == request.requestKey) {
            ++stats_.coalesced;
            return false;
        }
        if (activeAsset_ == request.assetGuid &&
            latest != latestSerial_.end()) {
            if (activeStop_) {
                activeStop_->request_stop();
            }
            ++stats_.cancellationRequests;
        }
        if (serialCounter_ == UINT64_MAX) {
            throw std::overflow_error(
                "Asset reimport serial counter is exhausted.");
        }
        const uint64_t serial = ++serialCounter_;
        latestSerial_[request.assetGuid] = serial;
        if (queued != queued_.end()) {
            *queued = WorkItem{
                .request = std::move(request),
                .serial = serial,
            };
            ++stats_.coalesced;
        } else {
            queued_.push_back({
                .request = std::move(request),
                .serial = serial,
            });
            ++stats_.queued;
        }
        ++stats_.enqueued;
        condition_.notify_all();
        return true;
    }

    void AssetReimportScheduler::cancel(
        AssetGuid assetGuid) {
        std::lock_guard lock(mutex_);
        if (assetGuid.isNil()) return;
        if (serialCounter_ != UINT64_MAX) {
            latestSerial_[assetGuid] =
                ++serialCounter_;
        }
        const size_t oldSize = queued_.size();
        std::erase_if(queued_,
            [assetGuid](const WorkItem& item) {
                return item.request.assetGuid ==
                    assetGuid;
            });
        stats_.queued -= static_cast<uint32_t>(
            oldSize - queued_.size());
        std::erase_if(completed_,
            [assetGuid](
                const AssetReimportCompletion&
                    completion) {
                return completion.assetGuid ==
                    assetGuid;
            });
        stats_.completed =
            static_cast<uint32_t>(
                completed_.size());
        if (activeAsset_ == assetGuid &&
            activeStop_) {
            activeStop_->request_stop();
            ++stats_.cancellationRequests;
        }
    }

    std::vector<AssetReimportCompletion>
        AssetReimportScheduler::takeCompletions() {
        std::lock_guard lock(mutex_);
        std::vector<AssetReimportCompletion> result;
        result.swap(completed_);
        stats_.completed = 0;
        return result;
    }

    bool AssetReimportScheduler::waitForCompletion(
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout,
            [this] {
                return !completed_.empty() ||
                    (queued_.empty() && !activeAsset_);
            }) && !completed_.empty();
    }

    AssetReimportDrainResult
        AssetReimportScheduler::drainTo(
            AssetRuntimePublisher& publisher) {
        AssetReimportDrainResult result;
        for (AssetReimportCompletion& completion :
            takeCompletions()) {
            if (completion.status ==
                    AssetReimportCompletionStatus::Failed ||
                !completion.prepared) {
                publisher.reportFailure(
                    completion.assetGuid,
                    completion.diagnostic.empty()
                        ? "Asset preparation failed without a diagnostic."
                        : std::move(completion.diagnostic));
                ++result.failed;
                continue;
            }
            PreparedRuntimeAsset prepared =
                std::move(*completion.prepared);
            const bool enqueued = publisher.enqueue({
                .assetGuid = completion.assetGuid,
                .cookKey = std::move(prepared.cookKey),
                .estimatedUploadBytes =
                    prepared.estimatedUploadBytes,
                .allowSingleOversizedUpload =
                    prepared.allowSingleOversizedUpload,
                .publish = std::move(prepared.publish),
            });
            if (enqueued) ++result.ready;
            else ++result.unchanged;
        }
        return result;
    }

    AssetReimportSchedulerStats
        AssetReimportScheduler::stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    void AssetReimportScheduler::shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            queued_.clear();
            stats_.queued = 0;
            if (activeStop_) {
                activeStop_->request_stop();
            }
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::lock_guard lock(mutex_);
            completed_.clear();
            stats_.completed = 0;
        }
    }

    void AssetReimportScheduler::workerLoop(
        std::stop_token stopToken) {
        while (true) {
            WorkItem item;
            std::stop_source workStop;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stopToken,
                    [this] {
                        return shutdown_ ||
                            !queued_.empty();
                    });
                if (shutdown_ ||
                    stopToken.stop_requested()) {
                    return;
                }
                item = std::move(queued_.front());
                queued_.pop_front();
                --stats_.queued;
                activeAsset_ =
                    item.request.assetGuid;
                activeRequestKey_ =
                    item.request.requestKey;
                activeStop_ = workStop;
                stats_.active = 1;
            }

            AssetReimportCompletion completion{
                .assetGuid = item.request.assetGuid,
                .requestKey = item.request.requestKey,
            };
            try {
                PreparedRuntimeAsset prepared =
                    item.request.prepare(
                        workStop.get_token());
                if (prepared.cookKey.empty() ||
                    !prepared.publish) {
                    throw std::runtime_error(
                        "Prepared runtime asset is missing its cook key or publish callback.");
                }
                completion.status =
                    AssetReimportCompletionStatus::Ready;
                completion.prepared =
                    std::move(prepared);
            } catch (const std::exception& exception) {
                completion.diagnostic =
                    exception.what();
            } catch (...) {
                completion.diagnostic =
                    "Asset preparation threw an unknown exception.";
            }

            {
                std::lock_guard lock(mutex_);
                activeAsset_.reset();
                activeRequestKey_.clear();
                activeStop_.reset();
                stats_.active = 0;
                const auto latest = latestSerial_.find(
                    item.request.assetGuid);
                if (shutdown_) {
                    return;
                }
                if (latest == latestSerial_.end() ||
                    latest->second != item.serial) {
                    ++stats_.superseded;
                } else {
                    if (completion.status ==
                        AssetReimportCompletionStatus::Ready) {
                        ++stats_.prepared;
                    } else {
                        ++stats_.failed;
                    }
                    completed_.push_back(
                        std::move(completion));
                    stats_.completed =
                        static_cast<uint32_t>(
                            completed_.size());
                }
            }
            condition_.notify_all();
        }
    }

} // namespace Iridium
