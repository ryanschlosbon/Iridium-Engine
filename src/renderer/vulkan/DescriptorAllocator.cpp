#include "DescriptorAllocator.h"
#include <stdexcept>

void DescriptorAllocator::init(VkDevice logicalDevice) {
    device = logicalDevice;
    currentPool = createNewPool(); // Start with one chunk
}

void DescriptorAllocator::cleanup() {
    for (auto pool : activePools) {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    activePools.clear();
    setOwners.clear();
    currentPool = VK_NULL_HANDLE;
}

VkDescriptorPool DescriptorAllocator::createNewPool() {
    // We define a standard "chunk" size. 
    // You can make these smaller (e.g., 100) since we generate them dynamically!
    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 }
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 100; // Total sets per chunk

    VkDescriptorPool newPool;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &newPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create a new descriptor pool chunk!");
    }

    activePools.push_back(newPool);
    return newPool;
}

VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = currentPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set;
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &set);

    // THE MAGIC: If the current chunk is full, spin up a new one and try again!
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {

        currentPool = createNewPool();
        allocInfo.descriptorPool = currentPool;

        // Try one more time with the fresh pool
        result = vkAllocateDescriptorSets(device, &allocInfo, &set);

        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate descriptor set even with a new pool!");
        }
    }
    else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor set!");
    }

    setOwners.emplace(set, allocInfo.descriptorPool);
    return set;
}

void DescriptorAllocator::free(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) {
        return;
    }

    auto owner = setOwners.find(set);
    if (owner == setOwners.end()) {
        return;
    }

    VkDescriptorPool pool = owner->second;
    if (vkFreeDescriptorSets(device, pool, 1, &set) != VK_SUCCESS) {
        throw std::runtime_error("Failed to free descriptor set!");
    }

    setOwners.erase(owner);
}

void DescriptorAllocator::free(std::span<const VkDescriptorSet> sets) {
    for (VkDescriptorSet set : sets) {
        free(set);
    }
}
