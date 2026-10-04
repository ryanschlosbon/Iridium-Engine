#pragma once

// Backend-internal include of the Vulkan Memory Allocator (M7R R4b). Every
// translation unit that includes vk_mem_alloc.h goes through this header so the
// configuration macros, which change VMA's struct layouts, agree with the single
// implementation TU (VulkanMemoryAllocatorImpl.cpp):
// - Vulkan entry points come from the statically linked loader;
// - the allocator targets Vulkan 1.3 (VMA_VULKAN_VERSION caps what VMA may use).
// VMA is third-party code (MIT, pinned in cmake/IridiumDependencies.cmake); its
// warnings are suppressed.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1003000

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
