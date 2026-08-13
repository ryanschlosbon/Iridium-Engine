#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Iridium {

    enum class ProfileMemoryCategory : uint8_t {
        GeometryVertex,
        GeometryIndex,
        Uniform,
        UploadStaging,
        CaptureReadback,
        Texture,
        Environment,
        ShadowDirectional,
        ShadowLocal,
        ExternalSwapchain,
        RenderGraphTransient,
        MaterialGpu,
        LightGpu,
        OtherUnclassified,
        Count,
    };

    inline constexpr size_t ProfileMemoryCategoryCount =
        static_cast<size_t>(ProfileMemoryCategory::Count);
    inline constexpr size_t ProfileMemoryHeapCapacity = 16;

    struct ProfileMemoryAllocation {
        ProfileMemoryCategory category = ProfileMemoryCategory::OtherUnclassified;
        uint64_t requestedBytes = 0;
        uint64_t committedBytes = 0;
        uint32_t memoryTypeIndex = 0;
        uint32_t memoryHeapIndex = 0;
        bool valid = false;
    };

    struct ProfileMemoryCategorySnapshot {
        const char* name = nullptr;
        const char* lifetimeClass = nullptr;
        uint64_t requestedLiveBytes = 0;
        uint64_t requestedPeakBytes = 0;
        uint64_t committedLiveBytes = 0;
        uint64_t committedPeakBytes = 0;
        uint64_t liveAllocationCount = 0;
        uint64_t peakAllocationCount = 0;
        uint32_t observedMemoryTypeMask = 0;
        uint32_t observedMemoryHeapMask = 0;
        bool requestedBytesAvailable = true;
        bool committedBytesAvailable = true;
        bool engineOwned = true;
    };

    struct ProfileMemoryHeapSnapshot {
        uint64_t heapSizeBytes = 0;
        uint64_t engineCommittedLiveBytes = 0;
        uint64_t engineCommittedPeakBytes = 0;
        uint64_t driverBudgetBytes = 0;
        uint64_t driverUsageBytes = 0;
        uint32_t flags = 0;
        bool driverBudgetAvailable = false;
    };

    struct FrameMemoryProfile {
        std::array<ProfileMemoryCategorySnapshot, ProfileMemoryCategoryCount> categories{};
        std::array<ProfileMemoryHeapSnapshot, ProfileMemoryHeapCapacity> heaps{};
        uint32_t categoryCount = 0;
        uint32_t heapCount = 0;
        uint64_t engineRequestedLiveBytes = 0;
        uint64_t engineRequestedPeakBytes = 0;
        uint64_t engineCommittedLiveBytes = 0;
        uint64_t engineCommittedPeakBytes = 0;
        uint64_t engineLiveAllocationCount = 0;
        uint64_t enginePeakAllocationCount = 0;
        bool engineAllocationTotalsAvailable = false;
        bool driverHeapBudgetAvailable = false;
    };

    [[nodiscard]] const char* profileMemoryCategoryName(
        ProfileMemoryCategory category) noexcept;
    [[nodiscard]] const char* profileMemoryLifetimeClass(
        ProfileMemoryCategory category) noexcept;

    // Fixed-capacity accounting used by backend allocators. It tracks engine-owned
    // allocations only; driver heap usage is populated independently by the backend.
    class MemoryProfileAccumulator final {
    public:
        void recordAllocation(ProfileMemoryAllocation& allocation,
            ProfileMemoryCategory category, uint64_t requestedBytes,
            uint64_t committedBytes, uint32_t memoryTypeIndex,
            uint32_t memoryHeapIndex) noexcept;
        void recordFree(ProfileMemoryAllocation& allocation) noexcept;
        void reclassify(ProfileMemoryAllocation& allocation,
            ProfileMemoryCategory category) noexcept;
        [[nodiscard]] FrameMemoryProfile snapshot() const noexcept;

    private:
        struct Totals {
            uint64_t requestedLiveBytes = 0;
            uint64_t requestedPeakBytes = 0;
            uint64_t committedLiveBytes = 0;
            uint64_t committedPeakBytes = 0;
            uint64_t liveAllocationCount = 0;
            uint64_t peakAllocationCount = 0;
            uint32_t observedMemoryTypeMask = 0;
            uint32_t observedMemoryHeapMask = 0;
        };

        static void add(Totals& totals, const ProfileMemoryAllocation& allocation) noexcept;
        static void remove(Totals& totals, const ProfileMemoryAllocation& allocation) noexcept;

        std::array<Totals, ProfileMemoryCategoryCount> categories_{};
        std::array<Totals, ProfileMemoryHeapCapacity> heaps_{};
        Totals total_{};
    };

} // namespace Iridium
