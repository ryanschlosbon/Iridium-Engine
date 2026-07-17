#include "VulkanSceneDescriptors.h"

#include <array>
#include <stdexcept>

namespace Iridium {

    namespace {
        VkWriteDescriptorSet imageWrite(VkDescriptorSet set, uint32_t binding,
            const VkDescriptorImageInfo& image) {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            return write;
        }
    }

    void VulkanSceneDescriptors::init(VkDevice device, ::DescriptorAllocator& allocator,
        VkDescriptorSetLayout layout) {
        if (device == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
            throw std::invalid_argument("Scene descriptors require a valid device and layout.");
        }
        if (device_ != VK_NULL_HANDLE || allocator_ != nullptr || layout_ != VK_NULL_HANDLE) {
            throw std::logic_error("Scene descriptors were initialized twice.");
        }
        device_ = device;
        allocator_ = &allocator;
        layout_ = layout;
    }

    void VulkanSceneDescriptors::rebuild(const VulkanFrameTargets& frameTargets) {
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr || layout_ == VK_NULL_HANDLE) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        if (frameTargets.size() == 0) {
            throw std::invalid_argument("Scene descriptors require at least one frame target.");
        }

        if (!sets_.empty()) {
            allocator_->free(std::span<const VkDescriptorSet>(sets_));
            sets_.clear();
        }

        try {
            sets_.reserve(frameTargets.size());
            for (size_t i = 0; i < frameTargets.size(); ++i) {
                sets_.push_back(allocator_->allocate(layout_));
                const VulkanPerImageTargets& target = frameTargets.get(i);
                const VkDescriptorImageInfo depth{ frameTargets.sampler(), target.depth.view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo normal{ frameTargets.sampler(), target.normal.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo albedo{ frameTargets.sampler(), target.albedo.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo emissive{ frameTargets.sampler(), target.emissive.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo opaqueCopy{ frameTargets.sampler(), target.opaqueCopy.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo glassDepth{ frameTargets.sampler(), target.glassDepth.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

                std::array<VkWriteDescriptorSet, 7> writes{};
                writes[0] = imageWrite(sets_.back(), 0, depth);
                writes[1] = imageWrite(sets_.back(), 1, normal);
                writes[2] = imageWrite(sets_.back(), 2, albedo);
                writes[3] = imageWrite(sets_.back(), 4, opaqueCopy);
                writes[4] = imageWrite(sets_.back(), 5, glassDepth);
                writes[5] = imageWrite(sets_.back(), 6, emissive);
                uint32_t writeCount = 6;
                if (hasEnvironment_) {
                    writes[6] = imageWrite(sets_.back(), 3, environment_);
                    writeCount = 7;
                }
                vkUpdateDescriptorSets(device_, writeCount, writes.data(), 0, nullptr);
            }
        } catch (...) {
            if (!sets_.empty()) {
                allocator_->free(std::span<const VkDescriptorSet>(sets_));
                sets_.clear();
            }
            throw;
        }
    }

    void VulkanSceneDescriptors::setEnvironment(VkDescriptorImageInfo environment) {
        if (environment.imageView == VK_NULL_HANDLE || environment.sampler == VK_NULL_HANDLE ||
            environment.imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            throw std::invalid_argument("Invalid environment descriptor.");
        }
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
            throw std::logic_error("Scene descriptors are not initialized.");
        }
        environment_ = environment;
        hasEnvironment_ = true;
        for (VkDescriptorSet set : sets_) {
            const VkWriteDescriptorSet write = imageWrite(set, 3, environment_);
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    }

    void VulkanSceneDescriptors::cleanup() {
        if (allocator_ != nullptr && !sets_.empty()) {
            allocator_->free(std::span<const VkDescriptorSet>(sets_));
        }
        sets_.clear();
        environment_ = {};
        hasEnvironment_ = false;
        device_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        layout_ = VK_NULL_HANDLE;
    }

    VkDescriptorSet VulkanSceneDescriptors::get(uint32_t imageIndex) const {
        if (imageIndex >= sets_.size()) {
            throw std::out_of_range("Scene descriptor image index is out of range.");
        }
        return sets_[imageIndex];
    }

    size_t VulkanSceneDescriptors::size() const noexcept {
        return sets_.size();
    }

} // namespace Iridium
