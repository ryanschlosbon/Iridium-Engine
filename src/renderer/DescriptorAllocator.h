#pragma once
#include <vulkan/vulkan.h>
#include <vector>

class DescriptorAllocator {
public:
    void init(VkDevice device);
    void cleanup();

    // The magic function: give it a layout, and it guarantees a valid Descriptor Set
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    VkDescriptorPool getPool() { return currentPool; }

private:
    VkDevice device;
    VkDescriptorPool currentPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> activePools;

    // Helper to spin up a new "chunk" of memory
    VkDescriptorPool createNewPool();
};