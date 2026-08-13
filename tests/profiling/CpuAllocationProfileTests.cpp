#include "profiling/CpuAllocationProfile.h"

#include <cstddef>
#include <iostream>
#include <new>

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

    if (failures == 0) {
        std::cout << "CpuAllocationProfileTests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
