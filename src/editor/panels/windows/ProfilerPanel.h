#pragma once

#include "../EditorPanel.h"
#include "profiling/CpuProfiler.h"

#include <array>
#include <cstddef>
#include <cstdint>

class ProfilerPanel final : public EditorPanel {
public:
    static constexpr size_t RangeCount = 19;
    static constexpr size_t GpuRangeCount = 12;

    ProfilerPanel(bool* isOpen, Iridium::CpuProfiler* profiler);

    void OnImGuiRender(Registry& registry,
        Iridium::AssetManager* assetManager) override;

private:
    bool* isOpen_ = nullptr;
    Iridium::CpuProfiler* profiler_ = nullptr;
    std::array<Iridium::ProfileStatistics, RangeCount> rangeStatistics_{};
    std::array<Iridium::ProfileStatistics, GpuRangeCount> gpuRangeStatistics_{};
    uint64_t lastStatisticsFrame_ = 0;
};
