#include "profiling/CpuProfileExport.h"
#include "profiling/CpuProfiler.h"
#include "profiling/GpuTimestamp.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    const CpuProfileEvent* findEvent(const CpuFrameProfile& frame, const char* name) {
        const auto event = std::find_if(frame.events.begin(), frame.events.end(),
            [name](const CpuProfileEvent& candidate) {
                return std::string(candidate.name) == name;
            });
        return event == frame.events.end() ? nullptr : &*event;
    }

    const ProfileRangeRunStatistics* findRunRange(
        const std::vector<ProfileRangeRunStatistics>& ranges, const char* name) {
        const auto range = std::find_if(ranges.begin(), ranges.end(),
            [name](const ProfileRangeRunStatistics& candidate) {
                return candidate.name != nullptr &&
                    std::string(candidate.name) == name;
            });
        return range == ranges.end() ? nullptr : &*range;
    }

    bool testDisabledBehavior() {
        CpuProfiler profiler;
        CHECK(!profiler.isEnabled());
        CHECK(!profiler.beginFrame());
        {
            CpuScope scope(profiler, "cpu.disabled");
            CHECK(!scope.isActive());
        }
        profiler.recordCounter("draw.requested.opaque", 4);
        CHECK(!profiler.endFrame());
        CHECK(profiler.completedFrameCount() == 0);
        CHECK(profiler.snapshotCompletedFrames().empty());
        return true;
    }

    bool testNestingAndCounters() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame());
        {
            CpuScope outer(profiler, "cpu.frame.total");
            CHECK(outer.isActive());
            {
                CpuScope inner(profiler, "cpu.input");
                CHECK(inner.isActive());
            }
            profiler.recordCounter("draw.requested.opaque", 17,
                ProfileCounterStatus::Estimated, ProfileCounterUnit::Count);
        }
        CHECK(profiler.endFrame());

        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 1);
        CHECK(frames[0].frameId == 1);
        CHECK(frames[0].events.size() == 2);
        CHECK(frames[0].counters.size() == 1);
        CHECK(frames[0].droppedEvents == 0);
        CHECK(frames[0].nestingErrors == 0);

        const CpuProfileEvent* outer = findEvent(frames[0], "cpu.frame.total");
        const CpuProfileEvent* inner = findEvent(frames[0], "cpu.input");
        CHECK(outer != nullptr);
        CHECK(inner != nullptr);
        CHECK(outer->parentEventId == 0);
        CHECK(inner->parentEventId == outer->eventId);
        CHECK(inner->threadId == outer->threadId);
        CHECK(frames[0].counters[0].value == 17);
        CHECK(frames[0].counters[0].status == ProfileCounterStatus::Estimated);
        return true;
    }

    bool testConcurrentCollection() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame());

        constexpr size_t ThreadCount = 8;
        constexpr size_t EventsPerThread = 32;
        std::array<std::thread, ThreadCount> workers;
        for (std::thread& worker : workers) {
            worker = std::thread([&profiler]() {
                for (size_t event = 0; event < EventsPerThread; ++event) {
                    CpuScope scope(profiler, "cpu.worker.job");
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }

        CHECK(profiler.endFrame());
        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 1);
        CHECK(frames[0].events.size() == ThreadCount * EventsPerThread);
        CHECK(frames[0].droppedEvents == 0);
        CHECK(frames[0].nestingErrors == 0);
        return true;
    }

    bool testOverflowAndActiveScopeDrop() {
        static_assert(CpuProfiler::MaxCountersPerFrame >= 192,
            "production profiling requires M5 counter headroom");
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame());
        for (size_t event = 0; event < CpuProfiler::MaxEventsPerFrame + 8; ++event) {
            CpuScope scope(profiler, "cpu.overflow");
        }
        for (size_t counter = 0; counter < CpuProfiler::MaxCountersPerFrame + 3;
            ++counter) {
            profiler.recordCounter("counter.overflow", counter);
        }
        CHECK(profiler.endFrame());

        std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.back().events.size() == CpuProfiler::MaxEventsPerFrame);
        CHECK(frames.back().counters.size() == CpuProfiler::MaxCountersPerFrame);
        CHECK(frames.back().droppedEvents == 8);
        CHECK(frames.back().droppedCounters == 3);

        CHECK(profiler.beginFrame());
        {
            CpuScope unfinished(profiler, "cpu.unfinished");
            CHECK(!profiler.endFrame());
        }
        CHECK(profiler.droppedFrameCount() == 1);

        CHECK(profiler.beginFrame());
        CHECK(profiler.endFrame());
        frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 2);
        CHECK(frames.back().frameId == 3);
        return true;
    }

    bool testCompletedFrameRingWraparound() {
        CpuProfiler profiler(true);
        constexpr size_t ExtraFrames = 5;
        for (size_t frame = 0;
            frame < CpuProfiler::CompletedFrameCapacity + ExtraFrames; ++frame) {
            CHECK(profiler.beginFrame());
            CHECK(profiler.endFrame());
        }

        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == CpuProfiler::CompletedFrameCapacity);
        CHECK(profiler.totalCompletedFrameCount() ==
            CpuProfiler::CompletedFrameCapacity + ExtraFrames);
        CHECK(frames.front().frameId == ExtraFrames + 1);
        CHECK(frames.back().frameId == CpuProfiler::CompletedFrameCapacity + ExtraFrames);
        return true;
    }

    bool testExactFullRunStatisticsBeyondDetailRing() {
        CpuProfiler profiler(true);
        constexpr size_t MissingCpuInterval = 10;
        constexpr size_t UnavailableGpuInterval = 20;

        for (uint64_t frame = 1;
            frame <= CpuProfiler::RunStatisticSampleCapacity; ++frame) {
            CHECK(profiler.beginFrame(frame));
            if (frame % MissingCpuInterval != 0) {
                CpuScope scope(profiler, "cpu.full_run");
            }
            CHECK(profiler.endFrame());

            const bool gpuAvailable = frame % UnavailableGpuInterval != 0;
            const std::array gpuRanges{
                GpuProfileRange{ "gpu.full_run", 0, frame, gpuAvailable },
            };
            CHECK(profiler.attachGpuRanges(frame, gpuRanges));
        }

        CHECK(profiler.completedFrameCount() ==
            CpuProfiler::CompletedFrameCapacity);
        CHECK(profiler.totalCompletedFrameCount() ==
            CpuProfiler::RunStatisticSampleCapacity);

        const ProfileRunStatistics run = profiler.snapshotRunStatistics();
        CHECK(run.completedFrameCount == CpuProfiler::RunStatisticSampleCapacity);
        const ProfileRangeRunStatistics* cpu = findRunRange(
            run.cpuRanges, "cpu.full_run");
        CHECK(cpu != nullptr);
        CHECK(cpu->statistics.sampleCount == 9'000);
        CHECK(cpu->missingFrameCount == 1'000);
        CHECK(cpu->sampleCapacityOverflowCount == 0);
        CHECK(cpu->durationOverflowFrameCount == 0);
        CHECK(cpu->statistics.minimum <= cpu->statistics.median);
        CHECK(cpu->statistics.median <= cpu->statistics.p95);
        CHECK(cpu->statistics.p95 <= cpu->statistics.p99);
        CHECK(cpu->statistics.p99 <= cpu->statistics.maximum);

        const ProfileRangeRunStatistics* gpu = findRunRange(
            run.gpuRanges, "gpu.full_run");
        CHECK(gpu != nullptr);
        CHECK(gpu->statistics.sampleCount == 9'500);
        CHECK(gpu->missingFrameCount == 500);
        CHECK(gpu->explicitlyUnavailableFrameCount == 500);
        CHECK(gpu->statistics.current == 9'999);
        CHECK(gpu->statistics.minimum == 1);
        CHECK(gpu->statistics.median == 4'999);
        CHECK(gpu->statistics.p95 == 9'499);
        CHECK(gpu->statistics.p99 == 9'899);
        CHECK(gpu->statistics.maximum == 9'999);
        CHECK(run.cpuDetailOverflowFrameCount == 0);
        CHECK(run.gpuDetailOverflowFrameCount == 0);
        return true;
    }

    bool testRunStatisticStorageOverflowIsTruthful() {
        CpuProfiler profiler(true);
        constexpr uint64_t ExtraFrames = 3;
        constexpr uint64_t FrameCount =
            CpuProfiler::RunStatisticSampleCapacity + ExtraFrames;

        for (uint64_t frame = 1; frame <= FrameCount; ++frame) {
            CHECK(profiler.beginFrame(frame));
            {
                CpuScope scope(profiler, "cpu.capacity");
            }
            CHECK(profiler.endFrame());
            const std::array gpuRanges{
                GpuProfileRange{ "gpu.capacity", 0, frame, true },
            };
            CHECK(profiler.attachGpuRanges(frame, gpuRanges));
        }

        const ProfileRunStatistics run = profiler.snapshotRunStatistics();
        const ProfileRangeRunStatistics* cpu = findRunRange(
            run.cpuRanges, "cpu.capacity");
        const ProfileRangeRunStatistics* gpu = findRunRange(
            run.gpuRanges, "gpu.capacity");
        CHECK(cpu != nullptr);
        CHECK(gpu != nullptr);
        CHECK(cpu->statistics.sampleCount == CpuProfiler::RunStatisticSampleCapacity);
        CHECK(cpu->missingFrameCount == ExtraFrames);
        CHECK(cpu->sampleCapacityOverflowCount == ExtraFrames);
        CHECK(gpu->statistics.sampleCount == CpuProfiler::RunStatisticSampleCapacity);
        CHECK(gpu->missingFrameCount == ExtraFrames);
        CHECK(gpu->sampleCapacityOverflowCount == ExtraFrames);
        CHECK(gpu->statistics.current == CpuProfiler::RunStatisticSampleCapacity);
        CHECK(gpu->statistics.maximum == CpuProfiler::RunStatisticSampleCapacity);

        CpuProfiler rangeCapacityProfiler(true);
        CHECK(rangeCapacityProfiler.beginFrame());
        std::array<std::array<char, 32>,
            CpuProfiler::MaxCpuRunStatisticRanges + 1> names{};
        for (size_t index = 0; index < names.size(); ++index) {
            std::snprintf(names[index].data(), names[index].size(),
                "cpu.capacity.%zu", index);
            CpuScope scope(rangeCapacityProfiler, names[index].data());
        }
        CHECK(rangeCapacityProfiler.endFrame());
        const ProfileRunStatistics rangeCapacityRun =
            rangeCapacityProfiler.snapshotRunStatistics();
        CHECK(rangeCapacityRun.cpuRanges.size() ==
            CpuProfiler::MaxCpuRunStatisticRanges);
        CHECK(rangeCapacityRun.unaggregatedCpuRangeValueCount == 1);

        CpuProfiler durationOverflowProfiler(true);
        CHECK(durationOverflowProfiler.beginFrame());
        CHECK(durationOverflowProfiler.endFrame());
        constexpr std::array overflowingRanges{
            GpuProfileRange{ "gpu.duration_overflow", 0,
                std::numeric_limits<uint64_t>::max(), true },
            GpuProfileRange{ "gpu.duration_overflow", 0, 1, true },
        };
        CHECK(durationOverflowProfiler.attachGpuRanges(1, overflowingRanges));
        const ProfileRunStatistics durationOverflowRun =
            durationOverflowProfiler.snapshotRunStatistics();
        const ProfileRangeRunStatistics* overflowed = findRunRange(
            durationOverflowRun.gpuRanges, "gpu.duration_overflow");
        CHECK(overflowed != nullptr);
        CHECK(overflowed->statistics.sampleCount == 0);
        CHECK(overflowed->missingFrameCount == 1);
        CHECK(overflowed->durationOverflowFrameCount == 1);
        return true;
    }

    bool testExplicitFrameIdentifiers() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame(501));
        CHECK(profiler.currentFrameId() == 501);
        CHECK(profiler.endFrame());
        CHECK(profiler.beginFrame(503));
        CHECK(profiler.endFrame());

        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 2);
        CHECK(frames[0].frameId == 501);
        CHECK(frames[1].frameId == 503);
        return true;
    }

    bool testNearestRankStatistics() {
        std::array<uint64_t, 100> samples{};
        for (size_t index = 0; index < samples.size(); ++index) {
            samples[index] = index + 1;
        }
        const ProfileStatistics statistics = calculateProfileStatistics(samples);
        CHECK(statistics.current == 100);
        CHECK(statistics.median == 50);
        CHECK(statistics.p95 == 95);
        CHECK(statistics.p99 == 99);
        CHECK(statistics.minimum == 1);
        CHECK(statistics.maximum == 100);
        CHECK(statistics.sampleCount == 100);
        return true;
    }

    bool testLiveCompletedFrameQueries() {
        CpuProfiler profiler(true);
        CHECK(profiler.latestCompletedFrame() == nullptr);
        CHECK(profiler.beginFrame());
        {
            CpuScope first(profiler, "cpu.repeated");
        }
        {
            CpuScope second(profiler, "cpu.repeated");
        }
        CHECK(profiler.endFrame());
        CHECK(profiler.beginFrame());
        CHECK(profiler.endFrame());

        CHECK(profiler.latestCompletedFrame() != nullptr);
        CHECK(profiler.latestCompletedFrame()->frameId == 2);
        const ProfileStatistics statistics =
            profiler.calculateRangeStatistics("cpu.repeated");
        CHECK(statistics.sampleCount == 1);
        return true;
    }

    bool testDelayedGpuRangeAttachment() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame(41));
        CHECK(profiler.endFrame());
        CHECK(profiler.beginFrame(42));
        CHECK(profiler.endFrame());

        constexpr std::array ranges{
            GpuProfileRange{ "gpu.frame", 0, 2'000'000, true },
            GpuProfileRange{ "gpu.gbuffer.opaque", 100, 700'000, true },
            GpuProfileRange{ "gpu.unavailable", 0, 0, false },
        };
        CHECK(profiler.attachGpuRanges(41, ranges, 2));
        CHECK(!profiler.attachGpuRanges(41, ranges, 2));
        CHECK(!profiler.attachGpuRanges(40, ranges));

        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.front().frameId == 41);
        CHECK(frames.front().gpuRanges.size() == ranges.size());
        CHECK(frames.front().droppedGpuRanges == 2);
        CHECK(frames.back().gpuRanges.empty());

        const ProfileRunStatistics run = profiler.snapshotRunStatistics();
        CHECK(run.gpuDetailOverflowFrameCount == 1);

        const ProfileStatistics frameStatistics =
            profiler.calculateGpuRangeStatistics("gpu.frame");
        CHECK(frameStatistics.sampleCount == 1);
        CHECK(frameStatistics.current == 2'000'000);
        CHECK(profiler.calculateGpuRangeStatistics(
            "gpu.unavailable").sampleCount == 0);
        return true;
    }

    bool testDelayedCounterAttachment() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame(71));
        profiler.recordCounter("transparent.fragment_invocations", 0,
            ProfileCounterStatus::Unavailable);
        CHECK(profiler.endFrame());
        CHECK(profiler.beginFrame(72));
        CHECK(profiler.endFrame());

        CHECK(profiler.attachCounter(71,
            "transparent.fragment_invocations", 123'456,
            ProfileCounterStatus::Exact));
        CHECK(profiler.attachCounter(71,
            "transparent.fullscreen_equivalents", 500'000,
            ProfileCounterStatus::Estimated,
            ProfileCounterUnit::Millionths));
        CHECK(!profiler.attachCounter(70, "counter.not_retained", 1));

        std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        CHECK(frames.size() == 2);
        CHECK(frames[0].counters.size() == 2);
        CHECK(std::string(frames[0].counters[0].name) ==
            "transparent.fragment_invocations");
        CHECK(frames[0].counters[0].value == 123'456);
        CHECK(frames[0].counters[0].status == ProfileCounterStatus::Exact);
        CHECK(frames[0].counters[1].value == 500'000);
        CHECK(frames[0].counters[1].status == ProfileCounterStatus::Estimated);
        CHECK(frames[0].counters[1].unit == ProfileCounterUnit::Millionths);
        CHECK(frames[1].counters.empty());

        CHECK(profiler.beginFrame(73));
        for (size_t index = 0; index < CpuProfiler::MaxCountersPerFrame; ++index) {
            profiler.recordCounter("counter.capacity", index);
        }
        CHECK(profiler.endFrame());
        CHECK(!profiler.attachCounter(73, "counter.delayed_overflow", 1));
        frames = profiler.snapshotCompletedFrames();
        CHECK(frames.back().counters.size() == CpuProfiler::MaxCountersPerFrame);
        CHECK(frames.back().droppedCounters == 1);
        return true;
    }

    bool testGpuTimestampConversion() {
        CHECK(calculateGpuTimestampDurationNanoseconds(10, 25, 64, 1.0) == 15);
        CHECK(calculateGpuTimestampDurationNanoseconds(10, 25, 64, 0.5) == 8);
        CHECK(calculateGpuTimestampDurationNanoseconds(250, 5, 8, 2.0) == 22);
        CHECK(calculateGpuTimestampDurationNanoseconds(10, 25, 0, 1.0) == 0);
        CHECK(calculateGpuTimestampDurationNanoseconds(10, 25, 65, 1.0) == 0);
        return true;
    }

    bool testMemoryProfileAccounting() {
        MemoryProfileAccumulator accumulator;
        ProfileMemoryAllocation texture{};
        ProfileMemoryAllocation staging{};
        accumulator.recordAllocation(texture, ProfileMemoryCategory::Texture,
            100, 128, 2, 0);
        accumulator.recordAllocation(staging, ProfileMemoryCategory::UploadStaging,
            50, 64, 3, 1);

        FrameMemoryProfile memory = accumulator.snapshot();
        CHECK(memory.engineAllocationTotalsAvailable);
        CHECK(memory.engineRequestedLiveBytes == 150);
        CHECK(memory.engineCommittedLiveBytes == 192);
        CHECK(memory.engineCommittedPeakBytes == 192);
        CHECK(memory.engineLiveAllocationCount == 2);

        accumulator.reclassify(texture, ProfileMemoryCategory::Environment);
        memory = accumulator.snapshot();
        const auto& textureCategory = memory.categories[
            static_cast<size_t>(ProfileMemoryCategory::Texture)];
        const auto& environmentCategory = memory.categories[
            static_cast<size_t>(ProfileMemoryCategory::Environment)];
        CHECK(textureCategory.requestedLiveBytes == 0);
        CHECK(textureCategory.requestedPeakBytes == 100);
        CHECK(environmentCategory.requestedLiveBytes == 100);
        CHECK(memory.engineRequestedLiveBytes == 150);

        accumulator.recordFree(texture);
        accumulator.recordFree(staging);
        memory = accumulator.snapshot();
        CHECK(memory.engineCommittedLiveBytes == 0);
        CHECK(memory.engineCommittedPeakBytes == 192);
        CHECK(memory.engineLiveAllocationCount == 0);
        CHECK(memory.enginePeakAllocationCount == 2);
        return true;
    }

    bool testJsonLinesExport() {
        CpuProfiler profiler(true);
        CHECK(profiler.beginFrame());
        {
            CpuScope scope(profiler, "cpu.frame.total");
        }
        {
            CpuScope firstWait(profiler, "cpu.renderer.frame_fence_wait");
        }
        {
            CpuScope secondWait(profiler, "cpu.renderer.frame_fence_wait");
        }
        profiler.recordCounter("draw.requested.opaque", 9);
        MemoryProfileAccumulator memoryAccumulator;
        ProfileMemoryAllocation allocation{};
        memoryAccumulator.recordAllocation(allocation,
            ProfileMemoryCategory::GeometryVertex, 96, 128, 0, 0);
        FrameMemoryProfile memory = memoryAccumulator.snapshot();
        memory.heapCount = 1;
        memory.heaps[0].heapSizeBytes = 1024;
        profiler.recordMemorySnapshot(memory);
        CHECK(profiler.endFrame());
        constexpr std::array gpuRanges{
            GpuProfileRange{ "gpu.frame", 0, 500'000, true },
        };
        CHECK(profiler.attachGpuRanges(1, gpuRanges));

        CpuProfileRunMetadata metadata{};
        metadata.runId = "unit-test";
        metadata.buildConfiguration = "test";
        metadata.sourceCommit = "0123456789abcdef";
        metadata.sourceBranch = "test-branch";
        metadata.validationEnabled = true;
        metadata.windowVisible = false;
        metadata.windowDecorated = false;
        metadata.requestedWindowWidth = 1280;
        metadata.requestedWindowHeight = 720;
        metadata.renderWidth = 1920;
        metadata.renderHeight = 1080;
        metadata.cpuProfilingEnabled = true;
        metadata.gpuProfilingRequested = true;
        metadata.gpuProfilingAvailable = true;
        metadata.gpuTimestampPeriodNanoseconds = 1.0;
        metadata.gpuTimestampValidBits = 64;
        metadata.sourceImportNanoseconds = 0;
        metadata.modelLoadNanoseconds = 42;
        metadata.unavailableFields = { "gpu_ranges", "driver_heap" };

        std::ostringstream output;
        writeCpuProfileJsonLines(output, profiler, metadata);

        std::vector<nlohmann::json> records;
        std::istringstream input(output.str());
        for (std::string line; std::getline(input, line);) {
            if (!line.empty()) {
                records.push_back(nlohmann::json::parse(line));
            }
        }
        CHECK(records.size() == 3);
        CHECK(records[0]["type"] == "run_header");
        CHECK(records[0]["frames_retained"] == 1);
        CHECK(!records[0]["window_visible"].get<bool>());
        CHECK(!records[0]["window_decorated"].get<bool>());
        CHECK(records[0]["requested_window_size"]["width"] == 1280);
        CHECK(records[0]["render_extent"]["width"] == 1920);
        CHECK(records[0]["profiling"]["gpu_available"].get<bool>());
        CHECK(records[0]["startup"]["source_import_ns"] == 0);
        CHECK(records[0]["startup"]["model_load_ns"] == 42);
        CHECK(records[0]["run_statistic_sample_capacity"] == 10'000);
        CHECK(records[1]["type"] == "frame");
        CHECK(records[1]["cpu_events"].size() == 3);
        CHECK(records[1]["gpu_ranges"].size() == 1);
        CHECK(records[1]["gpu_ranges_available"].get<bool>());
        CHECK(records[1]["memory"]["engine_totals"]["committed_live_bytes"] == 128);
        CHECK(records[1]["memory"]["categories"].size() ==
            ProfileMemoryCategoryCount);
        CHECK(records[1]["memory"]["categories"][0]["delta_available"] == false);
        CHECK(records[1]["counters"][0]["status"] == "exact");
        CHECK(records[2]["type"] == "run_summary");
        CHECK(records[2]["aggregate_scope"] == "all_completed_frames");
        CHECK(records[2]["aggregate_storage"]["sample_capacity_per_range"] ==
            10'000);
        CHECK(records[2]["cpu_ranges"]["cpu.frame.total"]["sample_count"] == 1);
        CHECK(records[2]["cpu_ranges"]["cpu.frame.total"]
            ["missing_frame_count"] == 0);
        CHECK(records[2]["cpu_ranges"]["cpu.renderer.frame_fence_wait"]["sample_count"] == 1);
        CHECK(records[2]["gpu_ranges"]["gpu.frame"]["sample_count"] == 1);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Disabled behavior", testDisabledBehavior },
        { "Nesting and counters", testNestingAndCounters },
        { "Concurrent collection", testConcurrentCollection },
        { "Overflow and active-scope drop", testOverflowAndActiveScopeDrop },
        { "Completed-frame ring wraparound", testCompletedFrameRingWraparound },
        { "Exact full-run statistics beyond detail ring",
            testExactFullRunStatisticsBeyondDetailRing },
        { "Run-statistic storage overflow is truthful",
            testRunStatisticStorageOverflowIsTruthful },
        { "Explicit frame identifiers", testExplicitFrameIdentifiers },
        { "Nearest-rank statistics", testNearestRankStatistics },
        { "Live completed-frame queries", testLiveCompletedFrameQueries },
        { "Delayed GPU range attachment", testDelayedGpuRangeAttachment },
        { "Delayed counter attachment", testDelayedCounterAttachment },
        { "GPU timestamp conversion", testGpuTimestampConversion },
        { "Memory profile accounting", testMemoryProfileAccounting },
        { "JSON Lines export", testJsonLinesExport },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
