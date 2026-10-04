#include "profiling/CpuAllocationProfile.h"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <new>
#include <thread>

namespace {

    int failures = 0;

    void require(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

} // namespace

int main() {
    using namespace Iridium;

    require(!isCpuAllocationFrameActive(), "diagnostic starts disabled");
    beginCpuAllocationFrame();
    require(isCpuAllocationFrameActive(), "begin enables diagnostic");

    void* first = ::operator new(64);
    void* second = ::operator new[](96);
    void* aligned = ::operator new(128, std::align_val_t{ 64 });

    const CpuAllocationFrameSample sample = endCpuAllocationFrame();
    require(!isCpuAllocationFrameActive(), "end disables diagnostic");
    require(sample.allocationCount == 3, "all global C++ allocations are counted");
    require(sample.requestedBytes == 288, "requested allocation bytes are summed");

    ::operator delete(first);
    ::operator delete[](second);
    ::operator delete(aligned, std::align_val_t{ 64 });

    void* outside = ::operator new(32);
    const CpuAllocationFrameSample unchanged = endCpuAllocationFrame();
    require(unchanged.allocationCount == 3,
        "disabled allocations do not change the completed sample");
    require(unchanged.requestedBytes == 288,
        "disabled allocation bytes do not change the completed sample");
    ::operator delete(outside);

    beginCpuAllocationFrame();
    const CpuAllocationFrameSample reset = endCpuAllocationFrame();
    require(reset.allocationCount == 0, "begin resets the allocation count");
    require(reset.requestedBytes == 0, "begin resets requested bytes");

    // M7R R5b.1: counters are thread-scoped. The frame thread (the caller of
    // begin) and threads inside a CpuAllocationFrameScope count as frame
    // allocations; every other thread counts as background.
    std::atomic<int> phase{ 0 };
    std::thread other([&phase]() {
        while (phase.load() != 1) {
            std::this_thread::yield();
        }
        ::operator delete(::operator new(40)); // background
        {
            CpuAllocationFrameScope frameWork;
            ::operator delete(::operator new(24)); // frame (frame-critical task)
            {
                CpuAllocationFrameScope nested;
                ::operator delete(::operator new(8)); // frame
            }
        }
        ::operator delete(::operator new(40)); // background again
        phase.store(2);
    });
    beginCpuAllocationFrame();
    ::operator delete(::operator new(16)); // frame thread
    phase.store(1);
    while (phase.load() != 2) {
        std::this_thread::yield();
    }
    const CpuAllocationFrameSample scoped = endCpuAllocationFrame();
    other.join();
    require(scoped.allocationCount == 3, "frame thread and frame scopes count as frame");
    require(scoped.requestedBytes == 48, "frame bytes are thread-scoped");
    require(scoped.backgroundAllocationCount == 2, "other threads count as background");
    require(scoped.backgroundRequestedBytes == 80, "background bytes are separate");

    beginCpuAllocationFrame();
    const CpuAllocationFrameSample resetBackground = endCpuAllocationFrame();
    require(resetBackground.backgroundAllocationCount == 0 &&
        resetBackground.backgroundRequestedBytes == 0,
        "begin resets the background counters");

    if (failures == 0) {
        std::cout << "CpuAllocationProfileTests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
