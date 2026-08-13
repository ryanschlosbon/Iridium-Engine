#include "assets/thumbnail/AssetThumbnailUploadQueue.h"

#include <stdexcept>

namespace Iridium {

    void AssetThumbnailUploadQueue::enqueue(
        AssetThumbnailPixels thumbnail) {
        if (!thumbnail.valid()) {
            throw std::invalid_argument(
                "Only valid thumbnail pixels may enter the upload queue.");
        }
        queued_.push_back(
            std::move(thumbnail));
    }

    AssetThumbnailUploadDrain
        AssetThumbnailUploadQueue::drain(
            uint64_t budgetBytes,
            const std::function<bool(
                AssetGuid)>& isDemanded,
            const std::function<void(
                const AssetThumbnailPixels&)>&
                    publish) {
        if (!isDemanded || !publish) {
            throw std::invalid_argument(
                "Thumbnail upload drain requires demand and publication callbacks.");
        }
        AssetThumbnailUploadDrain result{
            .budgetBytes = budgetBytes,
        };
        while (!queued_.empty()) {
            const AssetThumbnailPixels& thumbnail =
                queued_.front();
            if (!isDemanded(
                    thumbnail.assetGuid)) {
                queued_.pop_front();
                ++result.cancelled;
                continue;
            }
            const uint64_t bytes =
                thumbnail.rgba8.size();
            if (bytes >
                budgetBytes -
                    result.uploadedBytes) {
                ++result.deferredByBudget;
                break;
            }
            publish(thumbnail);
            result.uploadedBytes += bytes;
            ++result.uploaded;
            queued_.pop_front();
        }
        result.queuedAfterDrain =
            static_cast<uint32_t>(
                queued_.size());
        return result;
    }

} // namespace Iridium
