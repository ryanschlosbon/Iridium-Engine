#pragma once

#include "renderer/rhi/PipelineTypes.h"
#include "renderer/rhi/ResourcePool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace Iridium {

    struct VulkanPipelineTarget {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        uint32_t colorAttachmentCount = 0;
    };

    struct VulkanPipelineRecord {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        RenderPassClass renderPass = RenderPassClass::GBuffer;
    };

    struct PipelineStateDescHash {
        std::size_t operator()(const PipelineStateDesc& desc) const noexcept {
            std::size_t seed = 0;
            const auto combine = [&seed](uint32_t value) {
                seed ^= std::hash<uint32_t>{}(value) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            };

            combine(static_cast<uint32_t>(desc.shaderProgram));
            combine(static_cast<uint32_t>(desc.renderPass));
            combine(static_cast<uint32_t>(desc.topology));
            combine(static_cast<uint32_t>(desc.polygonMode));
            combine(static_cast<uint32_t>(desc.cullMode));
            combine(static_cast<uint32_t>(desc.frontFace));
            combine(static_cast<uint32_t>(desc.blendMode));
            combine(static_cast<uint32_t>(desc.depthCompare));
            combine(desc.colorWriteMask);
            combine(desc.depthTest ? 1u : 0u);
            combine(desc.depthWrite ? 1u : 0u);
            return seed;
        }
    };

    class VulkanPipelineLibrary final {
    public:
        VulkanPipelineLibrary() = default;
        ~VulkanPipelineLibrary() = default;

        VulkanPipelineLibrary(const VulkanPipelineLibrary&) = delete;
        VulkanPipelineLibrary& operator=(const VulkanPipelineLibrary&) = delete;
        VulkanPipelineLibrary(VulkanPipelineLibrary&&) = delete;
        VulkanPipelineLibrary& operator=(VulkanPipelineLibrary&&) = delete;

        void init(VkDevice device, VulkanPipelineTarget gBufferTarget, VulkanPipelineTarget forwardTarget);
        void cleanup() noexcept;

        PipelineHandle getOrCreatePipeline(const PipelineStateDesc& desc);
        const VulkanPipelineRecord* get(PipelineHandle handle) const noexcept;
        std::size_t pipelineCount() const noexcept { return pipelineMap_.size(); }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VulkanPipelineTarget gBufferTarget_{};
        VulkanPipelineTarget forwardTarget_{};
        ResourcePool<VulkanPipelineRecord, PipelineHandle> pipelineRecords_;
        std::unordered_map<PipelineStateDesc, PipelineHandle, PipelineStateDescHash> pipelineMap_;

        VkPipeline createPipeline(const PipelineStateDesc& desc, const VulkanPipelineTarget& target);
        VkShaderModule createShaderModule(const std::vector<char>& code) const;
        const VulkanPipelineTarget& getTarget(RenderPassClass renderPass) const;
    };

} // namespace Iridium
