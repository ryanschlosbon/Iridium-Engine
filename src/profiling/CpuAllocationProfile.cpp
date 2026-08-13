#include "CpuAllocationProfile.h"

#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

    std::atomic<bool> g_cpuAllocationFrameActive{ false };
    std::atomic<uint64_t> g_cpuAllocationCount{ 0 };
    std::atomic<uint64_t> g_cpuAllocationBytes{ 0 };

    void recordAllocation(std::size_t size) noexcept {
        if (!g_cpuAllocationFrameActive.load(std::memory_order_relaxed)) {
            return;
        }
        g_cpuAllocationCount.fetch_add(1, std::memory_order_relaxed);
        g_cpuAllocationBytes.fetch_add(static_cast<uint64_t>(size),
            std::memory_order_relaxed);
    }

    [[nodiscard]] void* allocateUnaligned(std::size_t size) {
        const std::size_t request = size == 0 ? 1 : size;
        void* memory = std::malloc(request);
        if (memory == nullptr) {
            throw std::bad_alloc{};
        }
        recordAllocation(size);
        return memory;
    }

    [[nodiscard]] void* allocateAligned(std::size_t size, std::size_t alignment) {
        const std::size_t request = size == 0 ? 1 : size;
#if defined(_WIN32)
        void* memory = _aligned_malloc(request, alignment);
#else
        void* memory = nullptr;
        if (posix_memalign(&memory, alignment, request) != 0) {
            memory = nullptr;
        }
#endif
        if (memory == nullptr) {
            throw std::bad_alloc{};
        }
        recordAllocation(size);
        return memory;
    }

    void freeAligned(void* memory) noexcept {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

} // namespace

namespace Iridium {

    void beginCpuAllocationFrame() noexcept {
        // Reset while disabled so an allocation racing with a frame boundary is
        // attributed to neither frame instead of corrupting both.
        g_cpuAllocationFrameActive.store(false, std::memory_order_release);
        g_cpuAllocationCount.store(0, std::memory_order_relaxed);
        g_cpuAllocationBytes.store(0, std::memory_order_relaxed);
        g_cpuAllocationFrameActive.store(true, std::memory_order_release);
    }

    CpuAllocationFrameSample endCpuAllocationFrame() noexcept {
        g_cpuAllocationFrameActive.store(false, std::memory_order_release);
        return {
            g_cpuAllocationCount.load(std::memory_order_relaxed),
            g_cpuAllocationBytes.load(std::memory_order_relaxed),
        };
    }

    bool isCpuAllocationFrameActive() noexcept {
        return g_cpuAllocationFrameActive.load(std::memory_order_acquire);
    }

} // namespace Iridium

void* operator new(std::size_t size) {
    return allocateUnaligned(size);
}

void* operator new[](std::size_t size) {
    return allocateUnaligned(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocateUnaligned(size);
    }
    catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocateUnaligned(size);
    }
    catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory, std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    freeAligned(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(size, static_cast<std::size_t>(alignment));
    }
    catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    try {
        return allocateAligned(size, static_cast<std::size_t>(alignment));
    }
    catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory, std::align_val_t,
    const std::nothrow_t&) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t,
    const std::nothrow_t&) noexcept {
    freeAligned(memory);
}
