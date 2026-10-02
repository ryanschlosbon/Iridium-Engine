#include "renderer/vulkan/VulkanIndexedTextureTable.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Iridium {

    namespace {
        void requireSuccess(VkResult result, const char* message) {
            if (result != VK_SUCCESS) throw std::runtime_error(message);
        }

        constexpr VkDescriptorBindingFlags IndexedBindingFlags =
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    void VulkanIndexedTextureTable::init(
        VkDevice device, uint32_t initialCapacity, uint32_t maximumCapacity) {
        if (device_ != VK_NULL_HANDLE) {
            throw std::logic_error("Indexed texture table initialized more than once");
        }
        if (device == VK_NULL_HANDLE || initialCapacity < 2 ||
            maximumCapacity < initialCapacity) {
            throw std::invalid_argument("Indexed texture table capacities are invalid");
        }

        device_ = device;
        logicalCapacity_ = initialCapacity;
        maximumCapacity_ = maximumCapacity;
        viewShadow_.resize(maximumCapacity_);
        samplerShadow_.resize(maximumCapacity_);
        viewPresent_.assign(maximumCapacity_, 0);
        samplerPresent_.assign(maximumCapacity_, 0);
        viewRevisions_.assign(maximumCapacity_, 0);
        samplerRevisions_.assign(maximumCapacity_, 0);

        try {
            const std::array<VkDescriptorSetLayoutBinding, 2> materialBindings{
                VkDescriptorSetLayoutBinding{
                    .binding = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
                VkDescriptorSetLayoutBinding{
                    .binding = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    .descriptorCount = maximumCapacity_,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
            };
            const std::array<VkDescriptorBindingFlags, 2> materialFlags{
                0,
                IndexedBindingFlags |
                    VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo materialFlagsInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            materialFlagsInfo.bindingCount =
                static_cast<uint32_t>(materialFlags.size());
            materialFlagsInfo.pBindingFlags = materialFlags.data();
            VkDescriptorSetLayoutCreateInfo materialLayoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            materialLayoutInfo.pNext = &materialFlagsInfo;
            materialLayoutInfo.flags =
                VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            materialLayoutInfo.bindingCount =
                static_cast<uint32_t>(materialBindings.size());
            materialLayoutInfo.pBindings = materialBindings.data();
            requireSuccess(vkCreateDescriptorSetLayout(device_,
                &materialLayoutInfo, nullptr, &materialViewLayout_),
                "Failed to create indexed material/view descriptor layout");

            const VkDescriptorSetLayoutBinding samplerBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = maximumCapacity_,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            };
            const VkDescriptorBindingFlags samplerFlags =
                IndexedBindingFlags |
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo samplerFlagsInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            samplerFlagsInfo.bindingCount = 1;
            samplerFlagsInfo.pBindingFlags = &samplerFlags;
            VkDescriptorSetLayoutCreateInfo samplerLayoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            samplerLayoutInfo.pNext = &samplerFlagsInfo;
            samplerLayoutInfo.flags =
                VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            samplerLayoutInfo.bindingCount = 1;
            samplerLayoutInfo.pBindings = &samplerBinding;
            requireSuccess(vkCreateDescriptorSetLayout(device_,
                &samplerLayoutInfo, nullptr, &samplerLayout_),
                "Failed to create indexed sampler descriptor layout");

            for (FrameTable& frame : frames_) {
                frame = createFrameTable(initialCapacity, {});
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    VulkanIndexedTextureTable::FrameTable
        VulkanIndexedTextureTable::createFrameTable(
            uint32_t capacity, const FrameTable& previous) const {
        FrameTable result{};
        result.capacity = capacity;
        result.materialBuffer = previous.materialBuffer;
        result.materialBufferSet = previous.materialBufferSet;
        result.synchronizedRevision = previous.synchronizedRevision;
        try {
            const std::array<VkDescriptorPoolSize, 3> sizes{
                VkDescriptorPoolSize{
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                VkDescriptorPoolSize{
                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity },
                VkDescriptorPoolSize{
                    VK_DESCRIPTOR_TYPE_SAMPLER, capacity },
            };
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
            poolInfo.maxSets = SetsPerFrame;
            poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
            poolInfo.pPoolSizes = sizes.data();
            requireSuccess(vkCreateDescriptorPool(
                device_, &poolInfo, nullptr, &result.pool),
                "Failed to create indexed texture frame descriptor pool");

            const std::array<VkDescriptorSetLayout, SetsPerFrame> layouts{
                materialViewLayout_, samplerLayout_ };
            const std::array<uint32_t, SetsPerFrame> counts{
                capacity, capacity };
            VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO };
            variableInfo.descriptorSetCount = SetsPerFrame;
            variableInfo.pDescriptorCounts = counts.data();
            VkDescriptorSetAllocateInfo allocateInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocateInfo.pNext = &variableInfo;
            allocateInfo.descriptorPool = result.pool;
            allocateInfo.descriptorSetCount = SetsPerFrame;
            allocateInfo.pSetLayouts = layouts.data();
            std::array<VkDescriptorSet, SetsPerFrame> sets{};
            requireSuccess(vkAllocateDescriptorSets(
                device_, &allocateInfo, sets.data()),
                "Failed to allocate indexed texture frame descriptor sets");
            result.materialViews = sets[0];
            result.samplers = sets[1];
            writeFrameShadow(result);
            result.synchronizedRevision = shadowRevision_;
        } catch (...) {
            if (result.pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, result.pool, nullptr);
            }
            throw;
        }
        return result;
    }

    void VulkanIndexedTextureTable::writeFrameShadow(FrameTable& frame) const {
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(static_cast<size_t>(frame.capacity) * 2 + 1);
        if (frame.materialBufferSet) {
            writes.push_back({
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = frame.materialViews,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &frame.materialBuffer,
            });
        }
        for (uint32_t index = 0; index < frame.capacity; ++index) {
            if (viewPresent_[index]) {
                writes.push_back({
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.materialViews,
                    .dstBinding = 1,
                    .dstArrayElement = index,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    .pImageInfo = &viewShadow_[index],
                });
            }
            if (samplerPresent_[index]) {
                writes.push_back({
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.samplers,
                    .dstBinding = 0,
                    .dstArrayElement = index,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                    .pImageInfo = &samplerShadow_[index],
                });
            }
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(device_,
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    void VulkanIndexedTextureTable::destroyFrameTable(FrameTable& frame) noexcept {
        if (device_ != VK_NULL_HANDLE && frame.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, frame.pool, nullptr);
        }
        frame = {};
    }

    void VulkanIndexedTextureTable::cleanup() noexcept {
        if (device_ == VK_NULL_HANDLE) return;
        for (FrameTable& frame : frames_) destroyFrameTable(frame);
        if (samplerLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, samplerLayout_, nullptr);
        }
        if (materialViewLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, materialViewLayout_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
        materialViewLayout_ = VK_NULL_HANDLE;
        samplerLayout_ = VK_NULL_HANDLE;
        logicalCapacity_ = 0;
        maximumCapacity_ = 0;
        viewShadow_.clear();
        samplerShadow_.clear();
        viewPresent_.clear();
        samplerPresent_.clear();
        viewRevisions_.clear();
        samplerRevisions_.clear();
        shadowRevision_ = 0;
        fallbackView_ = {};
        fallbackSampler_ = {};
        fallbackSet_ = false;
    }

    void VulkanIndexedTextureTable::ensureFrameCapacity(
        uint32_t frameIndex, uint32_t requiredCapacity) {
        if (!active() || frameIndex >= FrameSetCount) {
            throw std::invalid_argument("Indexed texture frame index is invalid");
        }
        if (requiredCapacity > maximumCapacity_) {
            throw std::out_of_range(
                "Indexed texture descriptor maximum capacity was exhausted");
        }
        logicalCapacity_ = (std::max)(logicalCapacity_, requiredCapacity);
        FrameTable& frame = frames_[frameIndex];
        if (frame.capacity >= logicalCapacity_) return;

        uint32_t replacementCapacity = frame.capacity;
        while (replacementCapacity < logicalCapacity_) {
            const uint64_t doubled = static_cast<uint64_t>(replacementCapacity) * 2;
            replacementCapacity = static_cast<uint32_t>(
                (std::min)(doubled, static_cast<uint64_t>(maximumCapacity_)));
        }
        FrameTable replacement = createFrameTable(replacementCapacity, frame);
        destroyFrameTable(frame);
        frame = replacement;
    }

    void VulkanIndexedTextureTable::synchronizeFrame(uint32_t frameIndex) {
        if (!active() || frameIndex >= FrameSetCount) {
            throw std::invalid_argument("Indexed texture frame index is invalid");
        }
        FrameTable& frame = frames_[frameIndex];
        if (frame.synchronizedRevision == shadowRevision_) return;

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(static_cast<size_t>(frame.capacity) * 2);
        for (uint32_t index = 0; index < frame.capacity; ++index) {
            if (viewPresent_[index] &&
                viewRevisions_[index] > frame.synchronizedRevision) {
                writes.push_back({
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.materialViews,
                    .dstBinding = 1,
                    .dstArrayElement = index,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    .pImageInfo = &viewShadow_[index],
                });
            }
            if (samplerPresent_[index] &&
                samplerRevisions_[index] > frame.synchronizedRevision) {
                writes.push_back({
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = frame.samplers,
                    .dstBinding = 0,
                    .dstArrayElement = index,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                    .pImageInfo = &samplerShadow_[index],
                });
            }
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(device_,
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        frame.synchronizedRevision = shadowRevision_;
    }

    void VulkanIndexedTextureTable::write(
        uint32_t viewIndex, VkImageView view,
        uint32_t samplerIndex, VkSampler sampler) {
        if (!active() || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
            viewIndex >= maximumCapacity_ || samplerIndex >= maximumCapacity_) {
            throw std::out_of_range("Indexed texture descriptor write is out of range");
        }
        logicalCapacity_ = (std::max)(logicalCapacity_,
            (std::max)(viewIndex, samplerIndex) + 1);
        viewShadow_[viewIndex] = {
            VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        samplerShadow_[samplerIndex] = {
            sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        viewPresent_[viewIndex] = 1;
        samplerPresent_[samplerIndex] = 1;
        ++shadowRevision_;
        if (shadowRevision_ == 0) {
            throw std::overflow_error("Indexed texture shadow revision overflowed");
        }
        viewRevisions_[viewIndex] = shadowRevision_;
        samplerRevisions_[samplerIndex] = shadowRevision_;
        if (viewIndex == 0 && samplerIndex == 0) {
            fallbackView_ = viewShadow_[0];
            fallbackSampler_ = samplerShadow_[0];
            fallbackSet_ = true;
        }
    }

    void VulkanIndexedTextureTable::writeFallback(
        uint32_t viewIndex, uint32_t samplerIndex) {
        if (!active() || !fallbackSet_ ||
            viewIndex >= maximumCapacity_ || samplerIndex >= maximumCapacity_) {
            return;
        }
        viewShadow_[viewIndex] = fallbackView_;
        samplerShadow_[samplerIndex] = fallbackSampler_;
        viewPresent_[viewIndex] = 1;
        samplerPresent_[samplerIndex] = 1;
        ++shadowRevision_;
        if (shadowRevision_ == 0) {
            throw std::overflow_error("Indexed texture shadow revision overflowed");
        }
        viewRevisions_[viewIndex] = shadowRevision_;
        samplerRevisions_[samplerIndex] = shadowRevision_;
    }

    void VulkanIndexedTextureTable::bindMaterialBuffer(
        uint32_t frameIndex, VkBuffer buffer, VkDeviceSize size) {
        if (!active() || frameIndex >= FrameSetCount ||
            buffer == VK_NULL_HANDLE || size == 0) {
            throw std::invalid_argument("Indexed material buffer binding is invalid");
        }
        FrameTable& frame = frames_[frameIndex];
        frame.materialBuffer = { buffer, 0, size };
        frame.materialBufferSet = true;
        VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.materialViews,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &frame.materialBuffer,
        };
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    uint32_t VulkanIndexedTextureTable::frameCapacity(
        uint32_t frameIndex) const noexcept {
        return frameIndex < frames_.size() ? frames_[frameIndex].capacity : 0;
    }

    std::array<VkDescriptorSet, VulkanIndexedTextureTable::SetsPerFrame>
        VulkanIndexedTextureTable::descriptorSets(
            uint32_t frameIndex) const noexcept {
        if (frameIndex >= frames_.size()) return {};
        return { frames_[frameIndex].materialViews, frames_[frameIndex].samplers };
    }

} // namespace Iridium
