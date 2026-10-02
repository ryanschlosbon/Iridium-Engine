#include "VulkanVirtualShadowMarkingPass.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {
void require(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Virtual shadow compute Vulkan failure: " +
            std::to_string(result));
}
void computeBarrier(VkCommandBuffer command) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}

void VulkanVirtualShadowMarkingPass::init(VkDevice device,
    std::span<const VkBuffer> workingSets,
    const VirtualShadowGpuWorkingSetLayout& layout,
    const VirtualShadowResourceConfig& config,
    const std::filesystem::path& shaderDirectory) {
    if (!device || device_ || workingSets.empty() || workingSets.size() > 8 ||
        std::ranges::find(workingSets, VK_NULL_HANDLE) != workingSets.end())
        throw std::invalid_argument("Invalid virtual shadow compute initialization");
    if (const auto error = validateVirtualShadowResourceConfig(config); !error.empty())
        throw std::invalid_argument(error);
    if (layout.scratchCapacity != config.receiverMarkCapacity)
        throw std::invalid_argument("Virtual shadow compute layout capacity mismatch");
    device_ = device;
    config_ = config;
    try {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            static_cast<uint32_t>(workingSets.size()) * 9u};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = static_cast<uint32_t>(workingSets.size()) * 2u;
        pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        require(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_));
        auto create = [&](Pipeline& pipeline, const char* filename,
            std::span<const VirtualShadowGpuBufferRange> ranges, uint32_t pushBytes) {
            std::vector<VkDescriptorSetLayoutBinding> bindings(ranges.size());
            for (uint32_t i = 0; i < bindings.size(); ++i)
                bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            set.bindingCount = static_cast<uint32_t>(bindings.size());
            set.pBindings = bindings.data();
            require(vkCreateDescriptorSetLayout(device_, &set, nullptr, &pipeline.descriptorLayout));
            const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayout.setLayoutCount = 1;
            pipelineLayout.pSetLayouts = &pipeline.descriptorLayout;
            pipelineLayout.pushConstantRangeCount = 1;
            pipelineLayout.pPushConstantRanges = &push;
            require(vkCreatePipelineLayout(device_, &pipelineLayout, nullptr, &pipeline.layout));
            std::ifstream stream(shaderDirectory / filename, std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("Cannot open virtual shadow compute shader");
            const auto bytes = stream.tellg();
            if (bytes <= 0 || bytes % 4 != 0)
                throw std::runtime_error("Invalid virtual shadow SPIR-V size");
            std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4u);
            stream.seekg(0); stream.read(reinterpret_cast<char*>(code.data()), bytes);
            if (!stream) throw std::runtime_error("Cannot read virtual shadow compute shader");
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize = static_cast<size_t>(bytes); module.pCode = code.data();
            VkShaderModule shader{};
            require(vkCreateShaderModule(device_, &module, nullptr, &shader));
            VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
            compute.layout = pipeline.layout;
            const auto result = vkCreateComputePipelines(device_, VK_NULL_HANDLE,
                1, &compute, nullptr, &pipeline.handle);
            vkDestroyShaderModule(device_, shader, nullptr);
            require(result);
            pipeline.sets.resize(workingSets.size());
            std::vector<VkDescriptorSetLayout> layouts(workingSets.size(), pipeline.descriptorLayout);
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = pool_;
            allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
            allocate.pSetLayouts = layouts.data();
            require(vkAllocateDescriptorSets(device_, &allocate, pipeline.sets.data()));
            for (size_t frame = 0; frame < workingSets.size(); ++frame) {
                std::vector<VkDescriptorBufferInfo> infos(ranges.size());
                std::vector<VkWriteDescriptorSet> writes(ranges.size());
                for (uint32_t i = 0; i < ranges.size(); ++i) {
                    infos[i] = {workingSets[frame], ranges[i].offset, ranges[i].size};
                    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    writes[i].dstSet = pipeline.sets[frame]; writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo = &infos[i];
                }
                vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                    writes.data(), 0, nullptr);
            }
        };
        const std::array markRanges{layout.clipLevels, layout.receivers, layout.rawMarks};
        create(marker_, "virtual_shadow_directional_mark_comp.spv", markRanges, 24);
        const std::array compactRanges{layout.clipLevels, layout.rawMarks,
            layout.alternateRequests, layout.denseRequests, layout.outputRequests, layout.telemetry};
        create(compactor_, "virtual_shadow_directional_compact_parallel_comp.spv", compactRanges, 32);
    } catch (...) { cleanup(); throw; }
}

void VulkanVirtualShadowMarkingPass::cleanup() noexcept {
    if (device_) {
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        for (auto* pipeline : {&marker_, &compactor_}) {
            if (pipeline->handle) vkDestroyPipeline(device_, pipeline->handle, nullptr);
            if (pipeline->layout) vkDestroyPipelineLayout(device_, pipeline->layout, nullptr);
            if (pipeline->descriptorLayout)
                vkDestroyDescriptorSetLayout(device_, pipeline->descriptorLayout, nullptr);
            *pipeline = {};
        }
    }
    device_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
}

void VulkanVirtualShadowMarkingPass::record(VkCommandBuffer command,
    uint32_t frameIndex, uint32_t receiverCount, uint32_t levelCount,
    uint32_t coarsestLevel, uint32_t guardBandPages) const {
    if (!device_ || !command || frameIndex >= marker_.sets.size() ||
        receiverCount > config_.receiverMarkCapacity || levelCount == 0 ||
        levelCount > 16 || coarsestLevel > 255 ||
        guardBandPages > UINT32_MAX / config_.pageSizeTexels)
        throw std::invalid_argument("Invalid virtual shadow compute record dimensions");
    // All transfer/compute producers of these frame-slot inputs precede this pass.
    VkMemoryBarrier input{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    input.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    input.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &input, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, marker_.handle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, marker_.layout,
        0, 1, &marker_.sets[frameIndex], 0, nullptr);
    const std::array<uint32_t, 6> markPush{receiverCount, levelCount,
        config_.pageSizeTexels, guardBandPages, coarsestLevel, VirtualShadowMapAbiVersion};
    vkCmdPushConstants(command, marker_.layout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(markPush), markPush.data());
    if (receiverCount) vkCmdDispatch(command, (receiverCount + 63u) / 64u, 1, 1);
    computeBarrier(command);
    recordCompaction(command, frameIndex, receiverCount, levelCount);
}

void VulkanVirtualShadowMarkingPass::recordCompaction(VkCommandBuffer command,
    uint32_t frameIndex, uint32_t receiverCount, uint32_t levelCount) const {
    if (!device_ || !command || frameIndex >= compactor_.sets.size() ||
        receiverCount > config_.receiverMarkCapacity || !levelCount || levelCount > 16)
        throw std::invalid_argument("Invalid virtual shadow raw-mark compaction dimensions");
    computeBarrier(command);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, compactor_.handle);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, compactor_.layout,
        0, 1, &compactor_.sets[frameIndex], 0, nullptr);
    const uint32_t capacity = (std::max)(256u, std::bit_ceil(receiverCount));
    auto dispatch = [&](uint32_t stage, uint32_t count, uint32_t width = 0) {
        const std::array<uint32_t, 8> push{stage, receiverCount, levelCount, capacity,
            config_.compactedRequestCapacity, VirtualShadowMapAbiVersion, width, 0};
        vkCmdPushConstants(command, compactor_.layout, VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(push), push.data());
        if (count) vkCmdDispatch(command, (count + 255u) / 256u, 1, 1);
        computeBarrier(command);
    };
    auto sort = [&](uint32_t local, uint32_t aToB, uint32_t bToA) {
        dispatch(local, capacity);
        bool sourceA = true;
        for (uint32_t width = 256; width < capacity; width <<= 1u) {
            dispatch(sourceA ? aToB : bToA, capacity, width); sourceA = !sourceA;
        }
        if (!sourceA) dispatch(5, capacity);
    };
    dispatch(0, capacity); dispatch(1, receiverCount);
    sort(2, 3, 4);
    dispatch(6, receiverCount); dispatch(7, capacity);
    sort(8, 9, 10);
    dispatch(11, capacity); dispatch(12, 256); dispatch(13, capacity);
}

} // namespace Iridium
