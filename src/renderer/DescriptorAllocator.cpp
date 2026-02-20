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
    currentPool = VK_NULL_HANDLE;
}

VkDescriptorPool DescriptorAllocator::createNewPool() {
    // We define a standard "chunk" size. 
    // You can make these smaller (e.g., 100) since we generate them dynamically!
    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 }
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
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

    return set;
}