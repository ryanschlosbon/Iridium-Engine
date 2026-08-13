#pragma once

#include "MemoryProfile.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class ProfileCounterStatus : uint8_t {
        Exact,
        Estimated,
        NotApplicable,
        Unavailable,
    };

    enum class ProfileCounterUnit : uint8_t {
        Count,
        Bytes,
        Millionths,
    };

    struct CpuProfileEvent {
        const char* name = nullptr;
        uint64_t eventId = 0;
        uint64_t parentEventId = 0;
        uint64_t threadId = 0;
        uint64_t startNanoseconds = 0;
        uint64_t durationNanoseconds = 0;
    };

    struct FrameProfileCounter {
        const char* name = nullptr;
        uint64_t value = 0;
        ProfileCounterStatus status = ProfileCounterStatus::Exact;
        ProfileCounterUnit unit = ProfileCounterUnit::Count;
    };

    struct GpuProfileRange {
        const char* name = nullptr;
        uint64_t startNanoseconds = 0;
        uint64_t durationNanoseconds = 0;
        bool available = false;
    };

    struct CpuFrameProfile {
        uint64_t frameId = 0;
        std::vector<CpuProfileEvent> events;
        std::vector<GpuProfileRange> gpuRanges;
        std::vector<FrameProfileCounter> counters;
        FrameMemoryProfile memory;
        uint32_t droppedEvents = 0;
        uint32_t droppedGpuRanges = 0;
        uint32_t droppedCounters = 0;
        uint32_t nestingErrors = 0;
        bool gpuRangesAttached = false;
    };

    struct ProfileStatistics {
        uint64_t current = 0;
        uint64_t median = 0;
        uint64_t p95 = 0;
        uint64_t p99 = 0;
        uint64_t minimum = 0;
        uint64_t maximum = 0;
        size_t sampleCount = 0;
    };

    struct ProfileRangeRunStatistics {
        const char* name = nullptr;
        ProfileStatistics statistics;
        uint64_t missingFrameCount = 0;
        uint64_t explicitlyUnavailableFrameCount = 0;
        uint64_t sampleCapacityOverflowCount = 0;
        uint64_t durationOverflowFrameCount = 0;
    };

    struct ProfileRunStatistics {
        uint64_t completedFrameCount = 0;
        std::vector<ProfileRangeRunStatistics> cpuRanges;
        std::vector<ProfileRangeRunStatistics> gpuRanges;
        uint64_t cpuDetailOverflowFrameCount = 0;
        uint64_t gpuDetailOverflowFrameCount = 0;
        uint64_t unaggregatedCpuRangeValueCount = 0;
        uint64_t unaggregatedGpuRangeValueCount = 0;
    };

    class CpuProfiler;

    struct CpuEventToken {
        CpuProfiler* profiler = nullptr;
        const char* name = nullptr;
        uint64_t eventId = 0;
        uint64_t parentEventId = 0;
        uint64_t generation = 0;
        uint64_t threadId = 0;
        uint64_t startNanoseconds = 0;
        bool active = false;
    };

    class CpuScope final {
    public:
        // Names are retained by pointer in bounded frame storage and must have
        // static lifetime (normally a string literal or interned stable name).
        CpuScope(CpuProfiler& profiler, const char* name) noexcept;
        CpuScope(CpuProfiler* profiler, const char* name) noexcept;
        ~CpuScope();

        CpuScope(const CpuScope&) = delete;
        CpuScope& operator=(const CpuScope&) = delete;
        CpuScope(CpuScope&&) = delete;
        CpuScope& operator=(CpuScope&&) = delete;

        [[nodiscard]] bool isActive() const noexcept { return token_.active; }

    private:
        CpuEventToken token_{};
    };

    class CpuProfiler final {
    public:
        static constexpr size_t MaxEventsPerFrame = 512;
        static constexpr size_t MaxGpuRangesPerFrame = 32;
        // M5 lighting, shadow, probe, and residency diagnostics bring the
        // production frame to 140 counters before the end-of-frame C++
        // allocation sample. Keep bounded headroom for later attached backend
        // diagnostics without silently dropping the acceptance counters.
        static constexpr size_t MaxCountersPerFrame = 192;
        static constexpr size_t CompletedFrameCapacity = 512;
        static constexpr size_t MaxNestedScopesPerThread = 64;
        // Exact nearest-rank statistics are retained for the complete contracted
        // M0 baseline. Detail remains in the smaller completed-frame ring above.
        static constexpr size_t RunStatisticSampleCapacity = 10'000;
        static constexpr size_t MaxCpuRunStatisticRanges = 64;
        static constexpr size_t MaxGpuRunStatisticRanges = MaxGpuRangesPerFrame;

        explicit CpuProfiler(bool enabled = false);
        ~CpuProfiler();

        CpuProfiler(const CpuProfiler&) = delete;
        CpuProfiler& operator=(const CpuProfiler&) = delete;

        void setEnabled(bool enabled);
        [[nodiscard]] bool isEnabled() const noexcept;
        [[nodiscard]] bool isFrameOpen() const noexcept;

        // Frame boundaries are owned by the main application thread. beginFrame
        // refuses to reuse storage while a prior-frame scope is still alive.
        // A nonzero frameId binds CPU events to the engine's shared frame clock.
        // Zero selects the next internal ID and is primarily useful for tests.
        [[nodiscard]] bool beginFrame(uint64_t frameId = 0);
        [[nodiscard]] bool endFrame();

        void recordCounter(const char* name, uint64_t value,
            ProfileCounterStatus status = ProfileCounterStatus::Exact,
            ProfileCounterUnit unit = ProfileCounterUnit::Count) noexcept;
        // Delayed backend diagnostics update an existing same-name counter or
        // append a new one to the bounded retained frame. Names must have static
        // lifetime. Returns false when the frame is no longer retained or the
        // frame counter capacity is exhausted.
        [[nodiscard]] bool attachCounter(uint64_t frameId, const char* name,
            uint64_t value,
            ProfileCounterStatus status = ProfileCounterStatus::Exact,
            ProfileCounterUnit unit = ProfileCounterUnit::Count) noexcept;
        void recordMemorySnapshot(const FrameMemoryProfile& memory) noexcept;

        // GPU results arrive after the owning backend frame fence completes.
        // They are attached to an already-completed CPU frame by shared frame ID.
        // Names must have static lifetime, matching CPU event names.
        [[nodiscard]] bool attachGpuRanges(uint64_t frameId,
            std::span<const GpuProfileRange> ranges,
            uint32_t droppedRanges = 0) noexcept;

        [[nodiscard]] std::vector<CpuFrameProfile> snapshotCompletedFrames() const;
        // Returns exact statistics over every available sample retained by the
        // explicitly bounded full-run accumulators, not just the detail ring.
        [[nodiscard]] ProfileRunStatistics snapshotRunStatistics() const;
        // Main-thread read-only views remain valid until the next successful
        // endFrame overwrites their ring slot.
        [[nodiscard]] const CpuFrameProfile* latestCompletedFrame() const noexcept;
        [[nodiscard]] const CpuFrameProfile* latestCompletedGpuFrame() const noexcept;
        [[nodiscard]] ProfileStatistics calculateRangeStatistics(
            std::string_view name) const;
        [[nodiscard]] ProfileStatistics calculateGpuRangeStatistics(
            std::string_view name) const;
        [[nodiscard]] size_t completedFrameCount() const noexcept;
        [[nodiscard]] uint64_t totalCompletedFrameCount() const noexcept;
        [[nodiscard]] uint64_t droppedFrameCount() const noexcept;
        [[nodiscard]] uint64_t currentFrameId() const noexcept;
        [[nodiscard]] uint64_t currentGeneration() const noexcept;

    private:
        friend class CpuScope;

        [[nodiscard]] CpuEventToken beginEvent(const char* name,
            uint64_t parentEventId) noexcept;
        void endEvent(CpuEventToken& token) noexcept;
        void recordNestingError() noexcept;
        void recordCpuRunStatistics(const CpuFrameProfile& frame) noexcept;
        void recordGpuRunStatistics(const CpuFrameProfile& frame) noexcept;
        void prepareStorage();

        static uint64_t nowNanoseconds() noexcept;
        static uint64_t currentThreadId() noexcept;

        std::atomic<bool> enabled_{ false };
        std::atomic<bool> frameOpen_{ false };
        std::atomic<uint32_t> activeWriters_{ 0 };
        std::atomic<uint32_t> eventWriteCount_{ 0 };
        std::atomic<uint32_t> counterWriteCount_{ 0 };
        std::atomic<uint64_t> nextEventId_{ 1 };
        std::atomic<uint64_t> generation_{ 0 };
        std::atomic<uint32_t> activeDroppedEvents_{ 0 };
        std::atomic<uint32_t> activeDroppedCounters_{ 0 };
        std::atomic<uint32_t> activeNestingErrors_{ 0 };

        std::array<CpuProfileEvent, MaxEventsPerFrame> activeEvents_{};
        std::array<FrameProfileCounter, MaxCountersPerFrame> activeCounters_{};
        FrameMemoryProfile activeMemory_{};
        std::unique_ptr<std::array<CpuFrameProfile, CompletedFrameCapacity>>
            completedFrames_;
        struct RunStatisticsStorage;
        std::unique_ptr<RunStatisticsStorage> runStatistics_;

        uint64_t currentFrameId_ = 0;
        uint64_t nextFrameId_ = 1;
        uint64_t frameStartNanoseconds_ = 0;
        uint64_t droppedFrames_ = 0;
        uint64_t totalCompletedFrames_ = 0;
        size_t completedCount_ = 0;
        size_t nextCompletedSlot_ = 0;
        bool storagePrepared_ = false;
        bool activeMemoryRecorded_ = false;
    };

    [[nodiscard]] ProfileStatistics calculateProfileStatistics(
        std::span<const uint64_t> samples);

} // namespace Iridium
