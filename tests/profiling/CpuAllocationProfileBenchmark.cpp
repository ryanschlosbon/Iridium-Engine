#include "profiling/CpuAllocationProfile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {

    constexpr uint64_t Iterations = 1'000'000;
    constexpr size_t Repetitions = 5;
    std::atomic<uintptr_t> pointerSink{ 0 };

    uint64_t run(bool enabled) {
        if (enabled) {
            Iridium::beginCpuAllocationFrame();
        }
        else if (Iridium::isCpuAllocationFrameActive()) {
            (void)Iridium::endCpuAllocationFrame();
        }

        const auto start = std::chrono::steady_clock::now();
        for (uint64_t index = 0; index < Iterations; ++index) {
            void* memory = ::operator new(64);
            pointerSink.fetch_xor(reinterpret_cast<uintptr_t>(memory),
                std::memory_order_relaxed);
            ::operator delete(memory);
        }
        const auto stop = std::chrono::steady_clock::now();
        if (enabled) {
            const Iridium::CpuAllocationFrameSample sample =
                Iridium::endCpuAllocationFrame();
            if (sample.allocationCount != Iterations ||
                sample.requestedBytes != Iterations * 64) {
                throw std::runtime_error("Allocation benchmark sample mismatch");
            }
        }
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }

} // namespace

int main() {
    std::array<uint64_t, Repetitions> disabled{};
    std::array<uint64_t, Repetitions> enabled{};
    for (size_t repetition = 0; repetition < Repetitions; ++repetition) {
        disabled[repetition] = run(false);
        enabled[repetition] = run(true);
    }
    std::sort(disabled.begin(), disabled.end());
    std::sort(enabled.begin(), enabled.end());
    const uint64_t disabledMedian = disabled[Repetitions / 2];
    const uint64_t enabledMedian = enabled[Repetitions / 2];
    const int64_t delta = static_cast<int64_t>(enabledMedian) -
        static_cast<int64_t>(disabledMedian);
    std::cout << "{\"iterations\":" << Iterations
        << ",\"repetitions\":" << Repetitions
        << ",\"disabled_median_total_ns\":" << disabledMedian
        << ",\"enabled_median_total_ns\":" << enabledMedian
        << ",\"delta_ns_per_allocation\":"
        << static_cast<double>(delta) / static_cast<double>(Iterations)
        << "}\n";
    return 0;
}
