#pragma once

#include "VulkanResourceAllocator.h"
#include "VulkanResourceState.h"

namespace Iridium {

    class VulkanCommandList {
    public:
        explicit VulkanCommandList(VkCommandBuffer commandBuffer = VK_NULL_HANDLE) noexcept
            : commandBuffer_(commandBuffer) {}

        [[nodiscard]] VkCommandBuffer native() const noexcept { return commandBuffer_; }
        [[nodiscard]] bool isValid() const noexcept { return commandBuffer_ != VK_NULL_HANDLE; }

        void transition(VulkanImageResource& resource, ResourceState newState) const {
            if (resource.state == newState) {
                return;
            }

            const VulkanStateInfo oldInfo = getVulkanStateInfo(resource.state, resource.aspect);
            const VulkanStateInfo newInfo = getVulkanStateInfo(newState, resource.aspect);
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = oldInfo.access;
            barrier.dstAccessMask = newInfo.access;
            barrier.oldLayout = oldInfo.layout;
            barrier.newLayout = newInfo.layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = resource.image;
            barrier.subresourceRange.aspectMask = resource.aspect;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = resource.mipLevels;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = resource.arrayLayers;

            vkCmdPipelineBarrier(commandBuffer_, oldInfo.stages, newInfo.stages, 0,
                0, nullptr, 0, nullptr, 1, &barrier);
            resource.state = newState;
        }

        void transition(VulkanBufferResource& resource, ResourceState newState) const {
            if (resource.state == newState) {
                return;
            }

            const VulkanStateInfo oldInfo = getVulkanStateInfo(resource.state, 0);
            const VulkanStateInfo newInfo = getVulkanStateInfo(newState, 0);
            VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            barrier.srcAccessMask = oldInfo.access;
            barrier.dstAccessMask = newInfo.access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = resource.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;

            vkCmdPipelineBarrier(commandBuffer_, oldInfo.stages, newInfo.stages, 0,
                0, nullptr, 1, &barrier, 0, nullptr);
            resource.state = newState;
        }

        void markState(VulkanImageResource& resource, ResourceState state) const noexcept {
            resource.state = state;
        }

        void markState(VulkanBufferResource& resource, ResourceState state) const noexcept {
            resource.state = state;
        }

        void copyBuffer(const VulkanBufferResource& source, VulkanBufferResource& destination,
            VkDeviceSize size, VkDeviceSize sourceOffset = 0, VkDeviceSize destinationOffset = 0) const {
            VkBufferCopy region{};
            region.srcOffset = sourceOffset;
            region.dstOffset = destinationOffset;
            region.size = size;
            vkCmdCopyBuffer(commandBuffer_, source.buffer, destination.buffer, 1, &region);
        }

        void copyBufferToImage(const VulkanBufferResource& source, VulkanImageResource& destination,
            const VkBufferImageCopy& region) const {
            vkCmdCopyBufferToImage(commandBuffer_, source.buffer, destination.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        void copyImageToBuffer(const VulkanImageResource& source,
            VulkanBufferResource& destination, const VkBufferImageCopy& region) const {
            vkCmdCopyImageToBuffer(commandBuffer_, source.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.buffer, 1, &region);
        }

        void copyImage(const VulkanImageResource& source, VulkanImageResource& destination,
            const VkImageCopy& region) const {
            vkCmdCopyImage(commandBuffer_, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

    private:
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    };

} // namespace Iridium
