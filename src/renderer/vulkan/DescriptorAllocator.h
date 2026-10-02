#pragma once
#include <vulkan/vulkan.h>
#include <span>
#include <unordered_map>
#include <vector>

class DescriptorAllocator {
public:
    void init(VkDevice device);
    void cleanup();

    // The magic function: give it a layout, and it guarantees a valid Descriptor Set
    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void free(VkDescriptorSet set);
    void free(std::span<const VkDescriptorSet> sets);
    VkDescriptorPool getPool() { return currentPool; }

private:
    VkDevice device;
    VkDescriptorPool currentPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> activePools;
    std::unordered_map<VkDescriptorSet, VkDescriptorPool> setOwners;

    // Helper to spin up a new "chunk" of memory
    VkDescriptorPool createNewPool();
};
