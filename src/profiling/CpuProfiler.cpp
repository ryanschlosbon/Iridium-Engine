#include "CpuProfiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>

namespace Iridium {

    namespace {

        struct ThreadScopeEntry {
            CpuProfiler* profiler = nullptr;
            uint64_t generation = 0;
            uint64_t eventId = 0;
        };

        struct ThreadScopeStack {
            std::array<ThreadScopeEntry, CpuProfiler::MaxNestedScopesPerThread> entries{};
            size_t depth = 0;
        };

        thread_local ThreadScopeStack threadScopeStack;

        size_t nearestRankIndex(size_t sampleCount, double percentile) {
            const size_t rank = static_cast<size_t>(
                std::ceil(percentile * static_cast<double>(sampleCount)));
            return std::max<size_t>(1, rank) - 1;
        }

        struct FrameRangeDuration {
            const char* name = nullptr;
            uint64_t durationNanoseconds = 0;
            bool available = true;
            bool durationOverflow = false;
        };

        struct RunRangeAccumulator {
            const char* name = nullptr;
            std::array<uint64_t, CpuProfiler::RunStatisticSampleCapacity> samples{};
            size_t sampleCount = 0;
            uint64_t latestAvailableFrameId = 0;
            uint64_t latestAvailableDuration = 0;
            uint64_t explicitlyUnavailableFrameCount = 0;
            uint64_t sampleCapacityOverflowCount = 0;
            uint64_t durationOverflowFrameCount = 0;
        };

        template <size_t Capacity>
        RunRangeAccumulator* findRunAccumulator(
            std::array<RunRangeAccumulator, Capacity>& accumulators,
            size_t accumulatorCount, std::string_view name) noexcept {
            for (size_t index = 0; index < accumulatorCount; ++index) {
                if (accumulators[index].name != nullptr &&
                    name == accumulators[index].name) {
                    return &accumulators[index];
                }
            }
            return nullptr;
        }

        template <size_t Capacity>
        RunRangeAccumulator* findOrCreateRunAccumulator(
            std::array<RunRangeAccumulator, Capacity>& accumulators,
            size_t& accumulatorCount, const char* name) noexcept {
            if (name == nullptr) {
                return nullptr;
            }
            if (RunRangeAccumulator* existing = findRunAccumulator(
                accumulators, accumulatorCount, name)) {
                return existing;
            }
            if (accumulatorCount >= Capacity) {
                return nullptr;
            }
            RunRangeAccumulator& accumulator = accumulators[accumulatorCount++];
            accumulator.name = name;
            return &accumulator;
        }

        template <size_t Capacity>
        FrameRangeDuration* findOrCreateFrameRange(
            std::array<FrameRangeDuration, Capacity>& ranges,
            size_t& rangeCount, const char* name) noexcept {
            if (name == nullptr) {
                return nullptr;
            }
            for (size_t index = 0; index < rangeCount; ++index) {
                if (ranges[index].name != nullptr &&
                    std::string_view(name) == ranges[index].name) {
                    return &ranges[index];
                }
            }
            if (rangeCount >= Capacity) {
                return nullptr;
            }
            FrameRangeDuration& range = ranges[rangeCount++];
            range.name = name;
            return &range;
        }

        void addFrameRangeDuration(FrameRangeDuration& range,
            uint64_t durationNanoseconds) noexcept {
            if (range.durationOverflow) {
                return;
            }
            if (durationNanoseconds > std::numeric_limits<uint64_t>::max() -
                range.durationNanoseconds) {
                range.durationNanoseconds = 0;
                range.durationOverflow = true;
                return;
            }
            range.durationNanoseconds += durationNanoseconds;
        }

        void appendRunSample(RunRangeAccumulator& accumulator,
            const FrameRangeDuration& range, uint64_t frameId) noexcept {
            if (range.durationOverflow) {
                ++accumulator.durationOverflowFrameCount;
                return;
            }
            if (!range.available) {
                ++accumulator.explicitlyUnavailableFrameCount;
                return;
            }
            if (accumulator.sampleCount >= CpuProfiler::RunStatisticSampleCapacity) {
                ++accumulator.sampleCapacityOverflowCount;
                return;
            }
            accumulator.samples[accumulator.sampleCount++] =
                range.durationNanoseconds;
            if (frameId >= accumulator.latestAvailableFrameId) {
                accumulator.latestAvailableFrameId = frameId;
                accumulator.latestAvailableDuration = range.durationNanoseconds;
            }
        }

    } // namespace

    struct CpuProfiler::RunStatisticsStorage {
        std::array<RunRangeAccumulator, MaxCpuRunStatisticRanges> cpuRanges{};
        std::array<RunRangeAccumulator, MaxGpuRunStatisticRanges> gpuRanges{};
        size_t cpuRangeCount = 0;
        size_t gpuRangeCount = 0;
        uint64_t cpuDetailOverflowFrameCount = 0;
        uint64_t gpuDetailOverflowFrameCount = 0;
        uint64_t unaggregatedCpuRangeValueCount = 0;
        uint64_t unaggregatedGpuRangeValueCount = 0;
    };

    CpuScope::CpuScope(CpuProfiler& profiler, const char* name) noexcept
        : CpuScope(&profiler, name) {}

    CpuScope::CpuScope(CpuProfiler* profiler, const char* name) noexcept {
        if (profiler == nullptr || name == nullptr || threadScopeStack.depth >=
            CpuProfiler::MaxNestedScopesPerThread) {
            if (profiler != nullptr && threadScopeStack.depth >=
                CpuProfiler::MaxNestedScopesPerThread) {
                profiler->recordNestingError();
            }
            return;
        }

        uint64_t parentEventId = 0;
        if (threadScopeStack.depth > 0) {
            const ThreadScopeEntry& parent =
                threadScopeStack.entries[threadScopeStack.depth - 1];
            if (parent.profiler == profiler &&
                parent.generation == profiler->currentGeneration()) {
                parentEventId = parent.eventId;
            }
        }

        token_ = profiler->beginEvent(name, parentEventId);
        if (!token_.active) {
            return;
        }

        threadScopeStack.entries[threadScopeStack.depth++] = {
            profiler, token_.generation, token_.eventId
        };
    }

    CpuScope::~CpuScope() {
        if (!token_.active) {
            return;
        }

        if (threadScopeStack.depth == 0) {
            token_.profiler->recordNestingError();
        }
        else {
            const ThreadScopeEntry& entry =
                threadScopeStack.entries[threadScopeStack.depth - 1];
            if (entry.profiler != token_.profiler || entry.eventId != token_.eventId) {
                token_.profiler->recordNestingError();
            }
            else {
                --threadScopeStack.depth;
            }
        }

        token_.profiler->endEvent(token_);
    }

    CpuProfiler::CpuProfiler(bool enabled) {
        setEnabled(enabled);
    }

    CpuProfiler::~CpuProfiler() = default;

    void CpuProfiler::prepareStorage() {
        if (storagePrepared_) {
            return;
        }
        completedFrames_ = std::make_unique<
            std::array<CpuFrameProfile, CompletedFrameCapacity>>();
        runStatistics_ = std::make_unique<RunStatisticsStorage>();
        for (CpuFrameProfile& frame : *completedFrames_) {
            frame.events.reserve(MaxEventsPerFrame);
            frame.gpuRanges.reserve(MaxGpuRangesPerFrame);
            frame.counters.reserve(MaxCountersPerFrame);
        }
        storagePrepared_ = true;
    }

    void CpuProfiler::setEnabled(bool enabled) {
        if (frameOpen_.load(std::memory_order_acquire) ||
            activeWriters_.load(std::memory_order_acquire) != 0) {
            throw std::logic_error("CpuProfiler cannot change state during an active frame");
        }
        if (enabled) {
            prepareStorage();
        }
        enabled_.store(enabled, std::memory_order_release);
    }

    bool CpuProfiler::isEnabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    bool CpuProfiler::isFrameOpen() const noexcept {
        return frameOpen_.load(std::memory_order_acquire);
    }

    bool CpuProfiler::beginFrame(uint64_t frameId) {
        if (!isEnabled()) {
            return false;
        }
        if (frameOpen_.load(std::memory_order_acquire)) {
            throw std::logic_error("CpuProfiler frame was begun twice");
        }
        if (activeWriters_.load(std::memory_order_acquire) != 0) {
            ++droppedFrames_;
            return false;
        }

        uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (generation == 0) {
            generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        const uint64_t assignedFrameId = frameId != 0 ? frameId : nextFrameId_;
        if (assignedFrameId < nextFrameId_) {
            throw std::invalid_argument("CpuProfiler frame IDs must increase monotonically");
        }
        if (assignedFrameId == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("CpuProfiler frame ID space was exhausted");
        }
        currentFrameId_ = assignedFrameId;
        nextFrameId_ = assignedFrameId + 1;
        frameStartNanoseconds_ = nowNanoseconds();
        nextEventId_.store(1, std::memory_order_relaxed);
        eventWriteCount_.store(0, std::memory_order_relaxed);
        counterWriteCount_.store(0, std::memory_order_relaxed);
        activeDroppedEvents_.store(0, std::memory_order_relaxed);
        activeDroppedCounters_.store(0, std::memory_order_relaxed);
        activeNestingErrors_.store(0, std::memory_order_relaxed);
        activeMemory_ = {};
        activeMemoryRecorded_ = false;
        frameOpen_.store(true, std::memory_order_release);
        return true;
    }

    bool CpuProfiler::endFrame() {
        if (!isEnabled() || !frameOpen_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }

        if (activeWriters_.load(std::memory_order_acquire) != 0) {
            ++droppedFrames_;
            return false;
        }

        CpuFrameProfile& destination = (*completedFrames_)[nextCompletedSlot_];
        destination.frameId = currentFrameId_;

        const uint32_t eventCount = std::min<uint32_t>(
            eventWriteCount_.load(std::memory_order_acquire),
            static_cast<uint32_t>(MaxEventsPerFrame));
        destination.events.assign(activeEvents_.begin(), activeEvents_.begin() + eventCount);
        destination.gpuRanges.clear();

        const uint32_t counterCount = std::min<uint32_t>(
            counterWriteCount_.load(std::memory_order_acquire),
            static_cast<uint32_t>(MaxCountersPerFrame));
        destination.counters.assign(
            activeCounters_.begin(), activeCounters_.begin() + counterCount);
        destination.memory = activeMemoryRecorded_ ? activeMemory_ : FrameMemoryProfile{};

        destination.droppedEvents = activeDroppedEvents_.load(std::memory_order_acquire);
        destination.droppedGpuRanges = 0;
        destination.droppedCounters = activeDroppedCounters_.load(std::memory_order_acquire);
        destination.nestingErrors = activeNestingErrors_.load(std::memory_order_acquire);
        destination.gpuRangesAttached = false;

        recordCpuRunStatistics(destination);

        nextCompletedSlot_ = (nextCompletedSlot_ + 1) % CompletedFrameCapacity;
        completedCount_ = std::min(completedCount_ + 1, CompletedFrameCapacity);
        ++totalCompletedFrames_;
        return true;
    }

    CpuEventToken CpuProfiler::beginEvent(const char* name,
        uint64_t parentEventId) noexcept {
        CpuEventToken token{};
        if (name == nullptr || !isEnabled() ||
            !frameOpen_.load(std::memory_order_acquire)) {
            return token;
        }

        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        if (!frameOpen_.load(std::memory_order_acquire)) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return token;
        }

        token.profiler = this;
        token.name = name;
        token.eventId = nextEventId_.fetch_add(1, std::memory_order_relaxed);
        token.parentEventId = parentEventId;
        token.generation = generation_.load(std::memory_order_acquire);
        token.threadId = currentThreadId();
        token.startNanoseconds = nowNanoseconds();
        token.active = true;
        return token;
    }

    void CpuProfiler::endEvent(CpuEventToken& token) noexcept {
        if (!token.active || token.profiler != this) {
            return;
        }

        const uint64_t endNanoseconds = nowNanoseconds();
        if (frameOpen_.load(std::memory_order_acquire) &&
            token.generation == generation_.load(std::memory_order_acquire)) {
            const uint32_t index = eventWriteCount_.fetch_add(1,
                std::memory_order_acq_rel);
            if (index < MaxEventsPerFrame) {
                CpuProfileEvent& event = activeEvents_[index];
                event.name = token.name;
                event.eventId = token.eventId;
                event.parentEventId = token.parentEventId;
                event.threadId = token.threadId;
                event.startNanoseconds = token.startNanoseconds >= frameStartNanoseconds_
                    ? token.startNanoseconds - frameStartNanoseconds_
                    : 0;
                event.durationNanoseconds = endNanoseconds >= token.startNanoseconds
                    ? endNanoseconds - token.startNanoseconds
                    : 0;
            }
            else {
                activeDroppedEvents_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        token.active = false;
        activeWriters_.fetch_sub(1, std::memory_order_release);
    }

    void CpuProfiler::recordCounter(const char* name, uint64_t value,
        ProfileCounterStatus status, ProfileCounterUnit unit) noexcept {
        if (name == nullptr || !isEnabled() ||
            !frameOpen_.load(std::memory_order_acquire)) {
            return;
        }

        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        if (!frameOpen_.load(std::memory_order_acquire)) {
            activeWriters_.fetch_sub(1, std::memory_order_release);
            return;
        }

        const uint32_t index = counterWriteCount_.fetch_add(1,
            std::memory_order_acq_rel);
        if (index < MaxCountersPerFrame) {
            activeCounters_[index] = { name, value, status, unit };
        }
        else {
            activeDroppedCounters_.fetch_add(1, std::memory_order_relaxed);
        }
        activeWriters_.fetch_sub(1, std::memory_order_release);
    }

    bool CpuProfiler::attachCounter(uint64_t frameId, const char* name,
        uint64_t value, ProfileCounterStatus status,
        ProfileCounterUnit unit) noexcept {
        if (!isEnabled() || frameId == 0 || name == nullptr) {
            return false;
        }

        const size_t first = completedCount_ == CompletedFrameCapacity
            ? nextCompletedSlot_
            : 0;
        for (size_t index = 0; index < completedCount_; ++index) {
            CpuFrameProfile& frame =
                (*completedFrames_)[(first + index) % CompletedFrameCapacity];
            if (frame.frameId != frameId) {
                continue;
            }

            for (FrameProfileCounter& counter : frame.counters) {
                if (counter.name != nullptr &&
                    std::string_view(name) == counter.name) {
                    counter = { name, value, status, unit };
                    return true;
                }
            }
            if (frame.counters.size() >= MaxCountersPerFrame) {
                if (frame.droppedCounters <
                    std::numeric_limits<uint32_t>::max()) {
                    ++frame.droppedCounters;
                }
                return false;
            }
            frame.counters.push_back({ name, value, status, unit });
            return true;
        }
        return false;
    }

    void CpuProfiler::recordMemorySnapshot(
        const FrameMemoryProfile& memory) noexcept {
        if (!isEnabled() || !frameOpen_.load(std::memory_order_acquire)) {
            return;
        }
        activeWriters_.fetch_add(1, std::memory_order_acq_rel);
        if (frameOpen_.load(std::memory_order_acquire)) {
            activeMemory_ = memory;
            activeMemoryRecorded_ = true;
        }
        activeWriters_.fetch_sub(1, std::memory_order_release);
    }

    bool CpuProfiler::attachGpuRanges(uint64_t frameId,
        std::span<const GpuProfileRange> ranges, uint32_t droppedRanges) noexcept {
        if (!isEnabled() || frameId == 0) {
            return false;
        }

        const size_t first = completedCount_ == CompletedFrameCapacity
            ? nextCompletedSlot_
            : 0;
        for (size_t index = 0; index < completedCount_; ++index) {
            CpuFrameProfile& frame =
                (*completedFrames_)[(first + index) % CompletedFrameCapacity];
            if (frame.frameId != frameId) {
                continue;
            }

            const size_t retained = std::min(ranges.size(), MaxGpuRangesPerFrame);
            if (frame.gpuRangesAttached) {
                return false;
            }
            frame.gpuRanges.assign(ranges.begin(), ranges.begin() + retained);
            const size_t overflow = ranges.size() - retained;
            frame.droppedGpuRanges = droppedRanges + static_cast<uint32_t>(
                std::min(overflow, static_cast<size_t>(
                    std::numeric_limits<uint32_t>::max() - droppedRanges)));
            frame.gpuRangesAttached = true;
            recordGpuRunStatistics(frame);
            return true;
        }
        return false;
    }

    void CpuProfiler::recordNestingError() noexcept {
        if (frameOpen_.load(std::memory_order_acquire)) {
            activeNestingErrors_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void CpuProfiler::recordCpuRunStatistics(
        const CpuFrameProfile& frame) noexcept {
        if (frame.droppedEvents != 0) {
            ++runStatistics_->cpuDetailOverflowFrameCount;
            return;
        }

        std::array<FrameRangeDuration, MaxEventsPerFrame> frameRanges{};
        size_t frameRangeCount = 0;
        for (const CpuProfileEvent& event : frame.events) {
            FrameRangeDuration* range = findOrCreateFrameRange(
                frameRanges, frameRangeCount, event.name);
            if (range == nullptr) {
                ++runStatistics_->unaggregatedCpuRangeValueCount;
                continue;
            }
            addFrameRangeDuration(*range, event.durationNanoseconds);
        }

        for (size_t index = 0; index < frameRangeCount; ++index) {
            const FrameRangeDuration& range = frameRanges[index];
            RunRangeAccumulator* accumulator = findOrCreateRunAccumulator(
                runStatistics_->cpuRanges, runStatistics_->cpuRangeCount,
                range.name);
            if (accumulator == nullptr) {
                ++runStatistics_->unaggregatedCpuRangeValueCount;
                continue;
            }
            appendRunSample(*accumulator, range, frame.frameId);
        }
    }

    void CpuProfiler::recordGpuRunStatistics(
        const CpuFrameProfile& frame) noexcept {
        const uint32_t unavailableRangeCount = static_cast<uint32_t>(
            std::count_if(frame.gpuRanges.begin(), frame.gpuRanges.end(),
                [](const GpuProfileRange& range) { return !range.available; }));
        // The existing attachment contract includes unavailable query results in
        // droppedGpuRanges. Count only excess values as actual detail overflow.
        if (frame.droppedGpuRanges > unavailableRangeCount) {
            ++runStatistics_->gpuDetailOverflowFrameCount;
        }

        std::array<FrameRangeDuration, MaxGpuRangesPerFrame> frameRanges{};
        size_t frameRangeCount = 0;
        for (const GpuProfileRange& gpuRange : frame.gpuRanges) {
            FrameRangeDuration* range = findOrCreateFrameRange(
                frameRanges, frameRangeCount, gpuRange.name);
            if (range == nullptr) {
                ++runStatistics_->unaggregatedGpuRangeValueCount;
                continue;
            }
            if (!gpuRange.available) {
                range->available = false;
                continue;
            }
            addFrameRangeDuration(*range, gpuRange.durationNanoseconds);
        }

        for (size_t index = 0; index < frameRangeCount; ++index) {
            const FrameRangeDuration& range = frameRanges[index];
            RunRangeAccumulator* accumulator = findOrCreateRunAccumulator(
                runStatistics_->gpuRanges, runStatistics_->gpuRangeCount,
                range.name);
            if (accumulator == nullptr) {
                ++runStatistics_->unaggregatedGpuRangeValueCount;
                continue;
            }
            appendRunSample(*accumulator, range, frame.frameId);
        }
    }

    std::vector<CpuFrameProfile> CpuProfiler::snapshotCompletedFrames() const {
        std::vector<CpuFrameProfile> result;
        result.reserve(completedCount_);
        const size_t first = completedCount_ == CompletedFrameCapacity
            ? nextCompletedSlot_
            : 0;
        for (size_t index = 0; index < completedCount_; ++index) {
            result.push_back((*completedFrames_)[
                (first + index) % CompletedFrameCapacity]);
        }
        return result;
    }

    ProfileRunStatistics CpuProfiler::snapshotRunStatistics() const {
        ProfileRunStatistics result{};
        result.completedFrameCount = totalCompletedFrames_;
        if (!runStatistics_) {
            return result;
        }

        const auto appendStatistics = [this](const auto& accumulators,
            size_t accumulatorCount,
            std::vector<ProfileRangeRunStatistics>& destination) {
            destination.reserve(accumulatorCount);
            for (size_t index = 0; index < accumulatorCount; ++index) {
                const RunRangeAccumulator& accumulator = accumulators[index];
                ProfileRangeRunStatistics range{};
                range.name = accumulator.name;
                range.statistics = calculateProfileStatistics(
                    std::span<const uint64_t>(accumulator.samples.data(),
                        accumulator.sampleCount));
                if (range.statistics.sampleCount != 0) {
                    range.statistics.current = accumulator.latestAvailableDuration;
                }
                range.missingFrameCount = totalCompletedFrames_ >=
                    accumulator.sampleCount
                    ? totalCompletedFrames_ - accumulator.sampleCount
                    : 0;
                range.explicitlyUnavailableFrameCount =
                    accumulator.explicitlyUnavailableFrameCount;
                range.sampleCapacityOverflowCount =
                    accumulator.sampleCapacityOverflowCount;
                range.durationOverflowFrameCount =
                    accumulator.durationOverflowFrameCount;
                destination.push_back(range);
            }
        };

        appendStatistics(runStatistics_->cpuRanges,
            runStatistics_->cpuRangeCount, result.cpuRanges);
        appendStatistics(runStatistics_->gpuRanges,
            runStatistics_->gpuRangeCount, result.gpuRanges);
        result.cpuDetailOverflowFrameCount =
            runStatistics_->cpuDetailOverflowFrameCount;
        result.gpuDetailOverflowFrameCount =
            runStatistics_->gpuDetailOverflowFrameCount;
        result.unaggregatedCpuRangeValueCount =
            runStatistics_->unaggregatedCpuRangeValueCount;
        result.unaggregatedGpuRangeValueCount =
            runStatistics_->unaggregatedGpuRangeValueCount;
        return result;
    }

    const CpuFrameProfile* CpuProfiler::latestCompletedFrame() const noexcept {
        if (completedCount_ == 0) {
            return nullptr;
        }
        const size_t slot = (nextCompletedSlot_ + CompletedFrameCapacity - 1) %
            CompletedFrameCapacity;
        return &(*completedFrames_)[slot];
    }

    const CpuFrameProfile* CpuProfiler::latestCompletedGpuFrame() const noexcept {
        for (size_t offset = 0; offset < completedCount_; ++offset) {
            const size_t slot = (nextCompletedSlot_ + CompletedFrameCapacity - 1 - offset) %
                CompletedFrameCapacity;
            if (!(*completedFrames_)[slot].gpuRanges.empty()) {
                return &(*completedFrames_)[slot];
            }
        }
        return nullptr;
    }

    ProfileStatistics CpuProfiler::calculateRangeStatistics(
        std::string_view name) const {
        std::array<uint64_t, CompletedFrameCapacity> samples{};
        size_t sampleCount = 0;
        const size_t first = completedCount_ == CompletedFrameCapacity
            ? nextCompletedSlot_
            : 0;
        for (size_t frameIndex = 0; frameIndex < completedCount_; ++frameIndex) {
            const CpuFrameProfile& frame =
                (*completedFrames_)[(first + frameIndex) % CompletedFrameCapacity];
            uint64_t duration = 0;
            bool present = false;
            for (const CpuProfileEvent& event : frame.events) {
                if (event.name != nullptr && name == event.name) {
                    duration += event.durationNanoseconds;
                    present = true;
                }
            }
            if (present) {
                samples[sampleCount++] = duration;
            }
        }
        return Iridium::calculateProfileStatistics(
            std::span<const uint64_t>(samples.data(), sampleCount));
    }

    ProfileStatistics CpuProfiler::calculateGpuRangeStatistics(
        std::string_view name) const {
        std::array<uint64_t, CompletedFrameCapacity> samples{};
        size_t sampleCount = 0;
        const size_t first = completedCount_ == CompletedFrameCapacity
            ? nextCompletedSlot_
            : 0;
        for (size_t frameIndex = 0; frameIndex < completedCount_; ++frameIndex) {
            const CpuFrameProfile& frame =
                (*completedFrames_)[(first + frameIndex) % CompletedFrameCapacity];
            uint64_t duration = 0;
            bool present = false;
            for (const GpuProfileRange& range : frame.gpuRanges) {
                if (range.available && range.name != nullptr && name == range.name) {
                    duration += range.durationNanoseconds;
                    present = true;
                }
            }
            if (present) {
                samples[sampleCount++] = duration;
            }
        }
        return Iridium::calculateProfileStatistics(
            std::span<const uint64_t>(samples.data(), sampleCount));
    }

    size_t CpuProfiler::completedFrameCount() const noexcept {
        return completedCount_;
    }

    uint64_t CpuProfiler::totalCompletedFrameCount() const noexcept {
        return totalCompletedFrames_;
    }

    uint64_t CpuProfiler::droppedFrameCount() const noexcept {
        return droppedFrames_;
    }

    uint64_t CpuProfiler::currentFrameId() const noexcept {
        return currentFrameId_;
    }

    uint64_t CpuProfiler::currentGeneration() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    uint64_t CpuProfiler::nowNanoseconds() noexcept {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t CpuProfiler::currentThreadId() noexcept {
        return static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    ProfileStatistics calculateProfileStatistics(std::span<const uint64_t> samples) {
        ProfileStatistics result{};
        if (samples.empty()) {
            return result;
        }

        std::vector<uint64_t> sorted(samples.begin(), samples.end());
        result.current = sorted.back();
        std::sort(sorted.begin(), sorted.end());
        result.sampleCount = sorted.size();
        result.minimum = sorted.front();
        result.maximum = sorted.back();
        result.median = sorted[nearestRankIndex(sorted.size(), 0.50)];
        result.p95 = sorted[nearestRankIndex(sorted.size(), 0.95)];
        result.p99 = sorted[nearestRankIndex(sorted.size(), 0.99)];
        return result;
    }

} // namespace Iridium
