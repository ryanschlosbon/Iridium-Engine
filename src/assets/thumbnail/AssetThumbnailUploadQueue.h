#pragma once

#include "assets/thumbnail/AssetThumbnail.h"

#include <cstdint>
#include <deque>
#include <functional>

namespace Iridium {

    struct AssetThumbnailUploadDrain {
        uint64_t budgetBytes = 0;
        uint64_t uploadedBytes = 0;
        uint32_t uploaded = 0;
        uint32_t cancelled = 0;
        uint32_t deferredByBudget = 0;
        uint32_t queuedAfterDrain = 0;
    };

    class AssetThumbnailUploadQueue {
    public:
        void enqueue(AssetThumbnailPixels thumbnail);
        [[nodiscard]] AssetThumbnailUploadDrain drain(
            uint64_t budgetBytes,
            const std::function<bool(AssetGuid)>&
                isDemanded,
            const std::function<void(
                const AssetThumbnailPixels&)>& publish);
        [[nodiscard]] size_t size() const noexcept {
            return queued_.size();
        }
        void clear() noexcept {
            queued_.clear();
        }

    private:
        std::deque<AssetThumbnailPixels> queued_;
    };

} // namespace Iridium
