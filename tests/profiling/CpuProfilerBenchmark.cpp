#include "profiling/CpuProfiler.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

    using namespace Iridium;

    constexpr size_t WarmupFrames = 1000;
    constexpr size_t MeasuredFrames = 10000;

    void simulateInstrumentedFrame(CpuProfiler& profiler) {
        const bool frameStarted = profiler.beginFrame();
        {
            CpuScope frame(profiler, "cpu.frame.total");
            const auto marker = [&profiler](const char* name) {
                CpuScope scope(profiler, name);
            };
            marker("cpu.platform.events");
            marker("cpu.input");
            marker("cpu.scene.asset_swaps");
            marker("cpu.scene.transforms");
            marker("cpu.renderer.begin_frame");
            marker("cpu.renderer.upload_wait");
            marker("cpu.renderer.frame_fence_wait");
            marker("cpu.renderer.acquire");
            marker("cpu.editor.build");
            marker("cpu.render.extract");
            marker("cpu.render.sort.opaque");
            marker("cpu.render.sort.transparent");
            marker("cpu.render.record.gbuffer");
            marker("cpu.render.record.lighting");
            marker("cpu.render.record.transparency");
            marker("cpu.render.record.ui");
            marker("cpu.renderer.submit");
            marker("cpu.renderer.present");

            profiler.recordCounter("draw.requested.opaque", 82);
            profiler.recordCounter("draw.requested.transparent", 5);
            profiler.recordCounter("draw.requested.selection", 87);
            profiler.recordCounter("instance.requested", 1);
            profiler.recordCounter("submesh.requested", 87);
            profiler.recordCounter("transparent.primitive.requested", 5);
            profiler.recordCounter("light.scene", 0);
            profiler.recordCounter("changed.transforms", 0,
                ProfileCounterStatus::Unavailable);
            profiler.recordCounter("changed.materials", 0,
                ProfileCounterStatus::Unavailable);
            profiler.recordCounter("changed.lights", 0,
                ProfileCounterStatus::Unavailable);
            profiler.recordCounter("changed.instances", 0,
                ProfileCounterStatus::Unavailable);
            profiler.recordCounter("draw.recorded.opaque", 82);
            profiler.recordCounter("draw.recorded.selection", 87);
            profiler.recordCounter("draw.recorded.lighting", 2);
            profiler.recordCounter("draw.recorded.transparent.depth", 5);
            profiler.recordCounter("draw.recorded.transparent.forward", 5);
            profiler.recordCounter("draw.recorded.transparent", 10);
            profiler.recordCounter("draw.recorded.ui", 8);
            profiler.recordCounter("draw.recorded.total", 189);
            profiler.recordCounter("dispatch.recorded", 0);
            profiler.recordCounter("triangle.submitted", 612'591);
            profiler.recordCounter("material.binds", 174);
            profiler.recordCounter("material.unique", 87);
            profiler.recordCounter("material.unique_overflow", 0);
            profiler.recordCounter("pipeline.binds", 9);
            profiler.recordCounter("pipeline.unique", 7);
            profiler.recordCounter("pipeline.unique_overflow", 0);
            profiler.recordCounter("transparent.bucket.background_packets", 4);
            profiler.recordCounter("transparent.bucket.foreground_packets", 1);
            profiler.recordCounter("transparent.bucket.nonempty", 2);
            profiler.recordCounter("ui.untracked_callbacks", 0);
        }
        if (frameStarted) {
            (void)profiler.endFrame();
        }
    }

    ProfileStatistics measure(bool enabled) {
        CpuProfiler profiler(enabled);
        for (size_t frame = 0; frame < WarmupFrames; ++frame) {
            simulateInstrumentedFrame(profiler);
        }

        std::vector<uint64_t> samples;
        samples.reserve(MeasuredFrames);
        for (size_t frame = 0; frame < MeasuredFrames; ++frame) {
            const auto start = std::chrono::steady_clock::now();
            simulateInstrumentedFrame(profiler);
            const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            samples.push_back(static_cast<uint64_t>(duration));
        }
        return calculateProfileStatistics(samples);
    }

} // namespace

int main() {
    const ProfileStatistics disabled = measure(false);
    const ProfileStatistics enabled = measure(true);
    const int64_t medianDelta = static_cast<int64_t>(enabled.median) -
        static_cast<int64_t>(disabled.median);
    const int64_t p99Delta = static_cast<int64_t>(enabled.p99) -
        static_cast<int64_t>(disabled.p99);

    std::cout << "IRIDIUM_CPU_PROFILER_BENCHMARK {"
        << "\"samples\":" << MeasuredFrames
        << ",\"disabled_median_ns\":" << disabled.median
        << ",\"disabled_p99_ns\":" << disabled.p99
        << ",\"enabled_median_ns\":" << enabled.median
        << ",\"enabled_p99_ns\":" << enabled.p99
        << ",\"median_delta_ns\":" << medianDelta
        << ",\"p99_delta_ns\":" << p99Delta
        << "}\n";

    constexpr uint64_t DisabledMedianLimitNanoseconds = 50'000;
    constexpr uint64_t EnabledMedianLimitNanoseconds = 100'000;
    return disabled.median <= DisabledMedianLimitNanoseconds &&
        enabled.median <= EnabledMedianLimitNanoseconds ? 0 : 1;
}
