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

    // M7R R5c.8: the frame-allocation observer (the qualification allocation
    // trace) sees exactly the frame allocations, after they are counted, and
    // the frame serial advances once per begin.
    static std::atomic<uint64_t> observedCalls{ 0 };
    static std::atomic<uint64_t> observedBytes{ 0 };
    setCpuFrameAllocationObserver([](std::size_t bytes) noexcept {
        observedCalls.fetch_add(1);
        observedBytes.fetch_add(bytes);
    });
    const uint64_t serialBefore =
        cpu_allocation_detail::allocationFrameSerial.load();
    ::operator delete(::operator new(12)); // outside a frame: not observed
    std::atomic<int> observedPhase{ 0 };
    std::thread background([&observedPhase]() {
        while (observedPhase.load() != 1) std::this_thread::yield();
        ::operator delete(::operator new(40)); // background: not observed
        {
            CpuAllocationFrameScope frameWork;
            ::operator delete(::operator new(24)); // frame: observed
        }
        observedPhase.store(2);
    });
    beginCpuAllocationFrame();
    ::operator delete(::operator new(16)); // frame thread: observed
    observedPhase.store(1);
    while (observedPhase.load() != 2) std::this_thread::yield();
    const CpuAllocationFrameSample observedSample = endCpuAllocationFrame();
    background.join();
    setCpuFrameAllocationObserver(nullptr);
    beginCpuAllocationFrame();
    ::operator delete(::operator new(8)); // observer removed
    (void)endCpuAllocationFrame();
    require(observedCalls.load() == observedSample.allocationCount &&
        observedCalls.load() == 2, "the observer sees each frame allocation once");
    require(observedBytes.load() == 40, "the observer receives the requested bytes");
    require(cpu_allocation_detail::allocationFrameSerial.load() == serialBefore + 2,
        "each begin advances the allocation frame serial");

    if (failures == 0) {
        std::cout << "CpuAllocationProfileTests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
