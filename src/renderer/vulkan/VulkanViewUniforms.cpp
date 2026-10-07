#include "VulkanViewUniforms.h"

#include "DescriptorAllocator.h"

#include <cstring>

namespace Iridium {

    void VulkanViewUniforms::createBuffers(VulkanResourceAllocator& allocator) {
        for (VulkanBufferResource& buffer : buffers_) {
            buffer = allocator.createBuffer(sizeof(UniformBufferObject),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::Uniform);
        }
    }

    void VulkanViewUniforms::allocateSet(uint32_t frame,
        ::DescriptorAllocator& descriptors, VkDescriptorSetLayout layout) {
        sets_[frame] = descriptors.allocate(layout);
    }

    void VulkanViewUniforms::writeSet(uint32_t frame, VkDevice device) const {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffers_[frame].buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkWriteDescriptorSet descriptorWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        descriptorWrite.dstSet = sets_[frame];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    void VulkanViewUniforms::update(uint32_t frame, const ViewTransportRecord& view,
        bool refractionPyramidsAvailable, RenderDebugView debugView) {
        UniformBufferObject ubo{};
        ubo.model = glm::mat4(1.0f); // Handled individually via push constants
        ubo.view = view.view;
        ubo.proj = view.projection;
        ubo.inverseView = view.inverseView;
        ubo.inverseProjection = view.inverseProjection;
        ubo.cameraPosition = view.cameraPosition;
        ubo.depthRange = view.depthRange;
        ubo.renderInfo = view.renderInfo;
        ubo.renderInfo.w &= ~(ViewTransportRefractionPyramidsAvailable |
            ViewTransportDebugViewMask);
        if (refractionPyramidsAvailable)
            ubo.renderInfo.w |= ViewTransportRefractionPyramidsAvailable;
        ubo.renderInfo.w |= (static_cast<uint32_t>(debugView) <<
            ViewTransportDebugViewShift) & ViewTransportDebugViewMask;
        ubo.worldUnits = view.worldUnits;
        ubo.jitteredProjection = view.jitteredProjection;
        ubo.jitteredInverseProjection = view.jitteredInverseProjection;
        ubo.previousViewProjection = view.previousViewProjection;
        ubo.jitter = view.jitter;
        ubo.temporalInfo = view.temporalInfo;
        std::memcpy(buffers_[frame].mapped, &ubo, sizeof(ubo));
    }

    void VulkanViewUniforms::destroy(VulkanResourceAllocator& allocator) noexcept {
        for (VulkanBufferResource& buffer : buffers_) allocator.destroy(buffer);
        sets_.fill(VK_NULL_HANDLE);
    }

} // namespace Iridium
