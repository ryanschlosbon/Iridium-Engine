#pragma once

// M7R R3c.8: the per-slot camera uniform buffer and its descriptor set (set 0,
// the "global set" every mesh, culling and transparency pipeline binds). A
// backend-owned service: the backend writes it in updateCamera and stages
// globalSet(frame) into each owner's per-frame inputs.

#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderDebugView.h"

#include "VulkanFrameScheduler.h"
#include "VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

class DescriptorAllocator;

namespace Iridium {

    class VulkanViewUniforms final {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;

        void createBuffers(VulkanResourceAllocator& allocator);
        // The backend's descriptor loop allocates the slot's set (allocation
        // order is kept), then writes its uniform binding.
        void allocateSet(uint32_t frame, ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout layout);
        void writeSet(uint32_t frame, VkDevice device) const;
        // The slot's camera uniforms; renderInfo.w carries the refraction
        // pyramid availability and the debug view.
        void update(uint32_t frame, const ViewTransportRecord& view,
            bool refractionPyramidsAvailable, RenderDebugView debugView);
        void destroy(VulkanResourceAllocator& allocator) noexcept;

        [[nodiscard]] VkDescriptorSet globalSet(uint32_t frame) const noexcept {
            return sets_[frame];
        }

    private:
        std::array<VulkanBufferResource, FrameCount> buffers_{};
        std::array<VkDescriptorSet, FrameCount> sets_{};
    };

} // namespace Iridium
