#include "MemoryProfile.h"

#include <algorithm>

namespace Iridium {

    namespace {

        constexpr size_t categoryIndex(ProfileMemoryCategory category) noexcept {
            return static_cast<size_t>(category);
        }

    } // namespace

    const char* profileMemoryCategoryName(ProfileMemoryCategory category) noexcept {
        switch (category) {
        case ProfileMemoryCategory::GeometryVertex: return "buffer.geometry.vertex";
        case ProfileMemoryCategory::GeometryIndex: return "buffer.geometry.index";
        case ProfileMemoryCategory::Uniform: return "buffer.uniform";
        case ProfileMemoryCategory::UploadStaging: return "buffer.upload_staging";
        case ProfileMemoryCategory::CaptureReadback: return "buffer.capture_readback";
        case ProfileMemoryCategory::Texture: return "image.texture";
        case ProfileMemoryCategory::Environment: return "image.environment";
        case ProfileMemoryCategory::ShadowDirectional:
            return "image.shadow.directional";
        case ProfileMemoryCategory::ShadowLocal:
            return "image.shadow.local";
        case ProfileMemoryCategory::ExternalSwapchain: return "external.swapchain";
        case ProfileMemoryCategory::RenderGraphTransient:
            return "render_graph.transient";
        case ProfileMemoryCategory::MaterialGpu: return "buffer.material_gpu";
        case ProfileMemoryCategory::LightGpu: return "buffer.light_gpu";
        case ProfileMemoryCategory::OtherUnclassified: return "other.unclassified";
        case ProfileMemoryCategory::Count: break;
        }
        return "other.unclassified";
    }

    const char* profileMemoryLifetimeClass(ProfileMemoryCategory category) noexcept {
        switch (category) {
        case ProfileMemoryCategory::UploadStaging:
            return "upload_batch";
        case ProfileMemoryCategory::CaptureReadback:
            return "capture_request";
        case ProfileMemoryCategory::ExternalSwapchain:
            return "external.swapchain";
        case ProfileMemoryCategory::RenderGraphTransient:
            return "frame_context.graph";
        default:
            return "persistent";
        }
    }

    void MemoryProfileAccumulator::add(Totals& totals,
        const ProfileMemoryAllocation& allocation) noexcept {
        totals.requestedLiveBytes += allocation.requestedBytes;
        totals.committedLiveBytes += allocation.committedBytes;
        ++totals.liveAllocationCount;
        totals.requestedPeakBytes = std::max(totals.requestedPeakBytes,
            totals.requestedLiveBytes);
        totals.committedPeakBytes = std::max(totals.committedPeakBytes,
            totals.committedLiveBytes);
        totals.peakAllocationCount = std::max(totals.peakAllocationCount,
            totals.liveAllocationCount);
        if (allocation.memoryTypeIndex < 32) {
            totals.observedMemoryTypeMask |= uint32_t{ 1 } << allocation.memoryTypeIndex;
        }
        if (allocation.memoryHeapIndex < 32) {
            totals.observedMemoryHeapMask |= uint32_t{ 1 } << allocation.memoryHeapIndex;
        }
    }

    void MemoryProfileAccumulator::remove(Totals& totals,
        const ProfileMemoryAllocation& allocation) noexcept {
        totals.requestedLiveBytes = totals.requestedLiveBytes >= allocation.requestedBytes
            ? totals.requestedLiveBytes - allocation.requestedBytes
            : 0;
        totals.committedLiveBytes = totals.committedLiveBytes >= allocation.committedBytes
            ? totals.committedLiveBytes - allocation.committedBytes
            : 0;
        if (totals.liveAllocationCount > 0) {
            --totals.liveAllocationCount;
        }
    }

    void MemoryProfileAccumulator::recordAllocation(
        ProfileMemoryAllocation& allocation, ProfileMemoryCategory category,
        uint64_t requestedBytes, uint64_t committedBytes,
        uint32_t memoryTypeIndex, uint32_t memoryHeapIndex) noexcept {
        if (allocation.valid || category == ProfileMemoryCategory::Count ||
            memoryHeapIndex >= heaps_.size()) {
            return;
        }
        allocation = {
            category, requestedBytes, committedBytes, memoryTypeIndex,
            memoryHeapIndex, true
        };
        add(categories_[categoryIndex(category)], allocation);
        add(heaps_[memoryHeapIndex], allocation);
        add(total_, allocation);
    }

    void MemoryProfileAccumulator::recordFree(
        ProfileMemoryAllocation& allocation) noexcept {
        if (!allocation.valid || allocation.category == ProfileMemoryCategory::Count ||
            allocation.memoryHeapIndex >= heaps_.size()) {
            allocation = {};
            return;
        }
        remove(categories_[categoryIndex(allocation.category)], allocation);
        remove(heaps_[allocation.memoryHeapIndex], allocation);
        remove(total_, allocation);
        allocation = {};
    }

    void MemoryProfileAccumulator::reclassify(ProfileMemoryAllocation& allocation,
        ProfileMemoryCategory category) noexcept {
        if (!allocation.valid || category == ProfileMemoryCategory::Count ||
            allocation.category == category) {
            return;
        }
        remove(categories_[categoryIndex(allocation.category)], allocation);
        allocation.category = category;
        add(categories_[categoryIndex(category)], allocation);
    }

    FrameMemoryProfile MemoryProfileAccumulator::snapshot() const noexcept {
        FrameMemoryProfile result{};
        result.categoryCount = static_cast<uint32_t>(categories_.size());
        result.engineAllocationTotalsAvailable = true;
        result.engineRequestedLiveBytes = total_.requestedLiveBytes;
        result.engineRequestedPeakBytes = total_.requestedPeakBytes;
        result.engineCommittedLiveBytes = total_.committedLiveBytes;
        result.engineCommittedPeakBytes = total_.committedPeakBytes;
        result.engineLiveAllocationCount = total_.liveAllocationCount;
        result.enginePeakAllocationCount = total_.peakAllocationCount;
        for (size_t index = 0; index < categories_.size(); ++index) {
            const ProfileMemoryCategory category =
                static_cast<ProfileMemoryCategory>(index);
            const Totals& totals = categories_[index];
            result.categories[index] = {
                profileMemoryCategoryName(category),
                profileMemoryLifetimeClass(category),
                totals.requestedLiveBytes,
                totals.requestedPeakBytes,
                totals.committedLiveBytes,
                totals.committedPeakBytes,
                totals.liveAllocationCount,
                totals.peakAllocationCount,
                totals.observedMemoryTypeMask,
                totals.observedMemoryHeapMask,
                category != ProfileMemoryCategory::ExternalSwapchain,
                category != ProfileMemoryCategory::ExternalSwapchain,
                category != ProfileMemoryCategory::ExternalSwapchain,
            };
        }
        for (size_t index = 0; index < heaps_.size(); ++index) {
            result.heaps[index].engineCommittedLiveBytes =
                heaps_[index].committedLiveBytes;
            result.heaps[index].engineCommittedPeakBytes =
                heaps_[index].committedPeakBytes;
        }
        return result;
    }

} // namespace Iridium
