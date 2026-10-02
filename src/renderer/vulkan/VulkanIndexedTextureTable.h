#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Iridium {

    // M3.3 production descriptor table. The layouts declare the capability-
    // bounded maximum while each frame owns variable-count descriptor sets.
    // A frame set is replaced only after that frame's fence has completed.
    class VulkanIndexedTextureTable {
    public:
        static constexpr uint32_t FrameSetCount = 2;
        static constexpr uint32_t SetsPerFrame = 2;

        VulkanIndexedTextureTable() = default;
        VulkanIndexedTextureTable(const VulkanIndexedTextureTable&) = delete;
        VulkanIndexedTextureTable& operator=(const VulkanIndexedTextureTable&) = delete;

        void init(VkDevice device, uint32_t initialCapacity,
            uint32_t maximumCapacity);
        void cleanup() noexcept;

        // Must be called only for a frame slot whose fence has completed.
        void ensureFrameCapacity(uint32_t frameIndex, uint32_t requiredCapacity);
        // Applies shadow-table changes only to the fence-safe frame slot.
        void synchronizeFrame(uint32_t frameIndex);

        void write(uint32_t viewIndex, VkImageView view,
            uint32_t samplerIndex, VkSampler sampler);
        void writeFallback(uint32_t viewIndex, uint32_t samplerIndex);
        void bindMaterialBuffer(
            uint32_t frameIndex, VkBuffer buffer, VkDeviceSize size);

        [[nodiscard]] bool active() const noexcept {
            return device_ != VK_NULL_HANDLE;
        }
        [[nodiscard]] uint32_t viewCapacity() const noexcept {
            return logicalCapacity_;
        }
        [[nodiscard]] uint32_t samplerCapacity() const noexcept {
            return logicalCapacity_;
        }
        [[nodiscard]] uint32_t maximumCapacity() const noexcept {
            return maximumCapacity_;
        }
        [[nodiscard]] uint32_t requiredCapacity() const noexcept {
            return logicalCapacity_;
        }
        [[nodiscard]] uint32_t frameCapacity(uint32_t frameIndex) const noexcept;
        [[nodiscard]] VkDescriptorSetLayout materialViewLayout() const noexcept {
            return materialViewLayout_;
        }
        [[nodiscard]] VkDescriptorSetLayout samplerLayout() const noexcept {
            return samplerLayout_;
        }
        [[nodiscard]] std::array<VkDescriptorSet, SetsPerFrame>
            descriptorSets(uint32_t frameIndex) const noexcept;

    private:
        struct FrameTable {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            VkDescriptorSet materialViews = VK_NULL_HANDLE;
            VkDescriptorSet samplers = VK_NULL_HANDLE;
            uint32_t capacity = 0;
            VkDescriptorBufferInfo materialBuffer{};
            bool materialBufferSet = false;
            uint64_t synchronizedRevision = 0;
        };

        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout materialViewLayout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout samplerLayout_ = VK_NULL_HANDLE;
        std::array<FrameTable, FrameSetCount> frames_{};
        uint32_t logicalCapacity_ = 0;
        uint32_t maximumCapacity_ = 0;
        std::vector<VkDescriptorImageInfo> viewShadow_;
        std::vector<VkDescriptorImageInfo> samplerShadow_;
        std::vector<uint8_t> viewPresent_;
        std::vector<uint8_t> samplerPresent_;
        std::vector<uint64_t> viewRevisions_;
        std::vector<uint64_t> samplerRevisions_;
        uint64_t shadowRevision_ = 0;
        VkDescriptorImageInfo fallbackView_{};
        VkDescriptorImageInfo fallbackSampler_{};
        bool fallbackSet_ = false;

        [[nodiscard]] FrameTable createFrameTable(
            uint32_t capacity, const FrameTable& previous) const;
        void destroyFrameTable(FrameTable& frame) noexcept;
        void writeFrameShadow(FrameTable& frame) const;
    };

} // namespace Iridium
