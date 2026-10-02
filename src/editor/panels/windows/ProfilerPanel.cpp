#include "ProfilerPanel.h"
#include "profiling/TransparencyProfilePresentation.h"

#include <imgui.h>

#include <array>

namespace {

    constexpr std::array<const char*, ProfilerPanel::RangeCount> RangeNames = {
        "cpu.frame.total",
        "cpu.platform.events",
        "cpu.input",
        "cpu.scene.asset_swaps",
        "cpu.scene.transforms",
        "cpu.renderer.begin_frame",
        "cpu.renderer.upload_wait",
        "cpu.renderer.frame_fence_wait",
        "cpu.renderer.acquire",
        "cpu.editor.build",
        "cpu.render.extract",
        "cpu.render.sort.opaque",
        "cpu.render.sort.transparent",
        "cpu.render.record.gbuffer",
        "cpu.render.record.lighting",
        "cpu.render.record.transparency",
        "cpu.render.record.ui",
        "cpu.renderer.submit",
        "cpu.renderer.present",
    };

    constexpr std::array<const char*, ProfilerPanel::GpuRangeCount> GpuRangeNames = {
        "gpu.frame",
        "gpu.gbuffer.opaque",
        "gpu.gbuffer.selection",
        "gpu.lighting.deferred",
        "gpu.lighting.selection_outline",
        "gpu.transparency.background.copy",
        "gpu.transparency.background.depth",
        "gpu.transparency.background.forward",
        "gpu.transparency.foreground.copy",
        "gpu.transparency.foreground.depth",
        "gpu.transparency.foreground.forward",
        "gpu.ui",
    };

    const char* statusName(Iridium::ProfileCounterStatus status) noexcept {
        switch (status) {
        case Iridium::ProfileCounterStatus::Exact: return "exact";
        case Iridium::ProfileCounterStatus::Estimated: return "estimated";
        case Iridium::ProfileCounterStatus::NotApplicable: return "n/a";
        case Iridium::ProfileCounterStatus::Unavailable: return "unavailable";
        }
        return "unavailable";
    }

    const char* unitName(Iridium::ProfileCounterUnit unit) noexcept {
        switch (unit) {
        case Iridium::ProfileCounterUnit::Count: return "count";
        case Iridium::ProfileCounterUnit::Bytes: return "bytes";
        case Iridium::ProfileCounterUnit::Millionths: return "millionths";
        }
        return "count";
    }

    double milliseconds(uint64_t nanoseconds) noexcept {
        return static_cast<double>(nanoseconds) / 1'000'000.0;
    }

    double mebibytes(uint64_t bytes) noexcept {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

} // namespace

ProfilerPanel::ProfilerPanel(bool* isOpen, Iridium::CpuProfiler* profiler)
    : isOpen_(isOpen), profiler_(profiler) {}

void ProfilerPanel::OnImGuiRender(Registry& registry,
    Iridium::AssetManager* assetManager) {
    (void)registry;
    (void)assetManager;
    if (isOpen_ == nullptr || !*isOpen_) {
        return;
    }

    if (!ImGui::Begin("Profiler", isOpen_)) {
        ImGui::End();
        return;
    }

    if (profiler_ == nullptr || !profiler_->isEnabled()) {
        ImGui::TextUnformatted("CPU collection is disabled.");
        ImGui::TextUnformatted("Restart with --profile-cpu to collect frames.");
        ImGui::End();
        return;
    }

    const Iridium::CpuFrameProfile* frame = profiler_->latestCompletedFrame();
    if (frame == nullptr) {
        ImGui::TextUnformatted("Waiting for the first completed profile frame...");
        ImGui::End();
        return;
    }

    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frame->frameId));
    ImGui::SameLine();
    ImGui::Text("Retained: %zu / %zu", profiler_->completedFrameCount(),
        Iridium::CpuProfiler::CompletedFrameCapacity);
    ImGui::SameLine();
    ImGui::Text("Completed: %llu", static_cast<unsigned long long>(
        profiler_->totalCompletedFrameCount()));
    ImGui::Text("Dropped frames: %llu | overflow events/counters: %u/%u | nesting: %u",
        static_cast<unsigned long long>(profiler_->droppedFrameCount()),
        frame->droppedEvents, frame->droppedCounters, frame->nestingErrors);

    constexpr uint64_t StatisticsRefreshFrames = 30;
    if (lastStatisticsFrame_ == 0 ||
        frame->frameId >= lastStatisticsFrame_ + StatisticsRefreshFrames) {
        for (size_t index = 0; index < RangeNames.size(); ++index) {
            rangeStatistics_[index] =
                profiler_->calculateRangeStatistics(RangeNames[index]);
        }
        for (size_t index = 0; index < GpuRangeNames.size(); ++index) {
            gpuRangeStatistics_[index] =
                profiler_->calculateGpuRangeStatistics(GpuRangeNames[index]);
        }
        lastStatisticsFrame_ = frame->frameId;
    }

    if (ImGui::BeginTable("CPU ranges", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0.0f, 300.0f))) {
        ImGui::TableSetupColumn("Range");
        ImGui::TableSetupColumn("Current ms");
        ImGui::TableSetupColumn("Median ms");
        ImGui::TableSetupColumn("p95 ms");
        ImGui::TableSetupColumn("p99 ms");
        ImGui::TableSetupColumn("Samples");
        ImGui::TableHeadersRow();
        for (size_t index = 0; index < RangeNames.size(); ++index) {
            const Iridium::ProfileStatistics& statistics = rangeStatistics_[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(RangeNames[index]);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.4f", milliseconds(statistics.current));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.4f", milliseconds(statistics.median));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.4f", milliseconds(statistics.p95));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.4f", milliseconds(statistics.p99));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%zu", statistics.sampleCount);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("GPU ranges (delayed)");
    const Iridium::CpuFrameProfile* gpuFrame =
        profiler_->latestCompletedGpuFrame();
    if (gpuFrame == nullptr) {
        ImGui::TextUnformatted("GPU timestamps disabled or awaiting fence-delayed results.");
    }
    else {
        ImGui::Text("Latest GPU frame: %llu | dropped/unavailable ranges: %u",
            static_cast<unsigned long long>(gpuFrame->frameId),
            gpuFrame->droppedGpuRanges);
        if (ImGui::BeginTable("GPU ranges", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0.0f, 240.0f))) {
            ImGui::TableSetupColumn("Range");
            ImGui::TableSetupColumn("Current ms");
            ImGui::TableSetupColumn("Median ms");
            ImGui::TableSetupColumn("p95 ms");
            ImGui::TableSetupColumn("p99 ms");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableHeadersRow();
            for (size_t index = 0; index < GpuRangeNames.size(); ++index) {
                const Iridium::ProfileStatistics& statistics =
                    gpuRangeStatistics_[index];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(GpuRangeNames[index]);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.4f", milliseconds(statistics.current));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.4f", milliseconds(statistics.median));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.4f", milliseconds(statistics.p95));
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.4f", milliseconds(statistics.p99));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%zu", statistics.sampleCount);
            }
            ImGui::EndTable();
        }
    }

    ImGui::SeparatorText("Engine memory");
    if (!frame->memory.engineAllocationTotalsAvailable) {
        ImGui::TextUnformatted("Engine allocation totals unavailable.");
    }
    else {
        ImGui::Text("Committed: %.2f MiB live / %.2f MiB peak | requested: %.2f MiB live",
            mebibytes(frame->memory.engineCommittedLiveBytes),
            mebibytes(frame->memory.engineCommittedPeakBytes),
            mebibytes(frame->memory.engineRequestedLiveBytes));
        ImGui::Text("Allocations: %llu live / %llu peak",
            static_cast<unsigned long long>(frame->memory.engineLiveAllocationCount),
            static_cast<unsigned long long>(frame->memory.enginePeakAllocationCount));
        if (ImGui::BeginTable("Memory categories", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0.0f, 220.0f))) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Lifetime");
            ImGui::TableSetupColumn("Requested MiB");
            ImGui::TableSetupColumn("Committed MiB");
            ImGui::TableHeadersRow();
            for (uint32_t index = 0; index < frame->memory.categoryCount &&
                index < frame->memory.categories.size(); ++index) {
                const Iridium::ProfileMemoryCategorySnapshot& category =
                    frame->memory.categories[index];
                if (category.liveAllocationCount == 0 &&
                    category.peakAllocationCount == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(category.name != nullptr
                    ? category.name
                    : "unavailable");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(category.lifetimeClass != nullptr
                    ? category.lifetimeClass
                    : "unavailable");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", mebibytes(category.requestedLiveBytes));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", mebibytes(category.committedLiveBytes));
            }
            ImGui::EndTable();
        }
    }

    ImGui::SeparatorText("Transparency diagnostics");
    uint32_t transparencyCounterCount = 0;
    uint32_t activeRiskCount = 0;
    for (const Iridium::FrameProfileCounter& counter : frame->counters) {
        if (counter.name == nullptr ||
            Iridium::transparencyProfileGroup(counter.name) ==
                Iridium::TransparencyProfileGroup::None) {
            continue;
        }
        ++transparencyCounterCount;
        if (counter.value != 0 &&
            Iridium::isTransparencyRiskCounter(counter.name)) {
            ++activeRiskCount;
        }
    }
    if (activeRiskCount == 0) {
        ImGui::Text("%u counters | no active fallback, rejection, or overflow signals",
            transparencyCounterCount);
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
            "%u counters | %u active fallback/rejection/overflow signals",
            transparencyCounterCount, activeRiskCount);
    }
    ImGui::TextDisabled(
        "Nonzero warning rows indicate bounded fallback or degraded work, not necessarily a renderer failure.");
    if (ImGui::BeginTable("Transparency counters", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0.0f, 280.0f))) {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Counter");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Unit");
        ImGui::TableHeadersRow();
        for (const Iridium::FrameProfileCounter& counter : frame->counters) {
            if (counter.name == nullptr) continue;
            const Iridium::TransparencyProfileGroup group =
                Iridium::transparencyProfileGroup(counter.name);
            if (group == Iridium::TransparencyProfileGroup::None) continue;
            const bool warning = counter.value != 0 &&
                Iridium::isTransparencyRiskCounter(counter.name);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(
                Iridium::transparencyProfileGroupName(group).data());
            ImGui::TableSetColumnIndex(1);
            if (warning) {
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                    "%s", counter.name);
            }
            else {
                ImGui::TextUnformatted(counter.name);
            }
            ImGui::TableSetColumnIndex(2);
            if (counter.unit == Iridium::ProfileCounterUnit::Bytes) {
                ImGui::Text("%llu (%.2f MiB)",
                    static_cast<unsigned long long>(counter.value),
                    mebibytes(counter.value));
            }
            else if (counter.unit ==
                    Iridium::ProfileCounterUnit::Millionths) {
                ImGui::Text("%.6f", static_cast<double>(counter.value) /
                    1'000'000.0);
            }
            else {
                ImGui::Text("%llu",
                    static_cast<unsigned long long>(counter.value));
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(statusName(counter.status));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(unitName(counter.unit));
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Active transparency GPU ranges (fence-delayed current frame)");
    if (gpuFrame == nullptr) {
        ImGui::TextUnformatted("GPU timestamps disabled or awaiting results.");
    }
    else if (ImGui::BeginTable("Transparency GPU ranges", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0.0f, 220.0f))) {
        ImGui::TableSetupColumn("Range");
        ImGui::TableSetupColumn("Current ms");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (const Iridium::GpuProfileRange& range : gpuFrame->gpuRanges) {
            if (range.name == nullptr ||
                !Iridium::isTransparencyGpuRange(range.name)) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(range.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.4f", milliseconds(range.durationNanoseconds));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(range.available ? "available" : "unavailable");
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Latest counters");
    if (ImGui::BeginTable("Profiler counters", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
        ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn("Counter");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Unit");
        ImGui::TableHeadersRow();
        for (const Iridium::FrameProfileCounter& counter : frame->counters) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(counter.name != nullptr ? counter.name : "unavailable");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(counter.value));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(statusName(counter.status));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(unitName(counter.unit));
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
