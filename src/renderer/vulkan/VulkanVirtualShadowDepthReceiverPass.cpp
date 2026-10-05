#include "VulkanVirtualShadowDepthReceiverPass.h"
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace Iridium {
namespace {
void require(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Virtual shadow depth receiver Vulkan failure");
}
struct Push { glm::mat4 inverse; std::array<uint32_t, 4> region, source; };
static_assert(sizeof(Push) == 96);
}
void VulkanVirtualShadowDepthReceiverPass::init(VkDevice device, VkPipelineCache pipelineCache,
    std::span<const VkBuffer> buffers, const VirtualShadowGpuWorkingSetLayout& working,
    const std::filesystem::path& shaderDirectory) {
    if (!device || device_ || buffers.empty() || buffers.size() > 8 || !working.scratchCapacity)
        throw std::invalid_argument("Invalid depth receiver initialization");
    device_ = device; capacity_ = working.scratchCapacity;
    pipelineCache_ = pipelineCache;
    try {
        const uint32_t count = static_cast<uint32_t>(buffers.size());
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = count; pool.poolSizeCount = 2; pool.pPoolSizes = sizes.data();
        require(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_));
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 2; set.pBindings = bindings.data();
        require(vkCreateDescriptorSetLayout(device_, &set, nullptr, &setLayout_));
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &setLayout_;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        require(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_));
        std::ifstream file(shaderDirectory / "virtual_shadow_depth_receivers_comp.spv", std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open depth receiver shader");
        const auto bytes = file.tellg();
        if (bytes <= 0 || bytes % 4) throw std::runtime_error("Invalid depth receiver SPIR-V");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), bytes);
        if (!file) throw std::runtime_error("Cannot read depth receiver shader");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = size_t(bytes); module.pCode = code.data();
        VkShaderModule shader{}; require(vkCreateShaderModule(device_, &module, nullptr, &shader));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout = layout_;
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        const auto result = vkCreateComputePipelines(device_, pipelineCache_, 1, &compute, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, shader, nullptr); require(result);
        sets_.resize(count); extents_.resize(count);
        std::vector<VkDescriptorSetLayout> layouts(count, setLayout_);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_; allocate.descriptorSetCount = count; allocate.pSetLayouts = layouts.data();
        require(vkAllocateDescriptorSets(device_, &allocate, sets_.data()));
        for (uint32_t i = 0; i < count; ++i) {
            const VkDescriptorBufferInfo info{buffers[i], working.receivers.offset, working.receivers.size};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = sets_[i]; write.dstBinding = 1; write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
    } catch (...) { cleanup(); throw; }
}
void VulkanVirtualShadowDepthReceiverPass::bindDepth(uint32_t slot, VkImageView view,
    VkSampler sampler, VkExtent2D extent, VkImageLayout imageLayout) {
    if (!device_ || slot >= sets_.size() || !view || !sampler || !extent.width || !extent.height ||
        (imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
         imageLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))
        throw std::invalid_argument("Invalid depth receiver image binding");
    const VkDescriptorImageInfo info{sampler, view, imageLayout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[slot]; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    extents_[slot] = extent;
}
void VulkanVirtualShadowDepthReceiverPass::record(VkCommandBuffer command, uint32_t slot,
    const VirtualShadowDepthReceiverRegion& region, const glm::mat4& inverse) const {
    if (!device_ || !command || slot >= sets_.size() ||
        region.sourceWidth != extents_[slot].width || region.sourceHeight != extents_[slot].height)
        throw std::invalid_argument("Depth receiver source extent mismatch");
    const auto count = validateVirtualShadowDepthReceiverRegion(region, capacity_);
    for (uint32_t c = 0; c < 4; ++c) for (uint32_t r = 0; r < 4; ++r)
        if (!std::isfinite(inverse[c][r])) throw std::invalid_argument("Invalid inverse view projection");
    const Push push{inverse, {region.originX, region.originY, region.width, region.height},
        {region.sourceWidth, region.sourceHeight, region.reverseDepth ? 1u : 0u, capacity_}};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[slot], 0, nullptr);
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, (count + 63u) / 64u, 1, 1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}
void VulkanVirtualShadowDepthReceiverPass::cleanup() noexcept {
    if (device_) {
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    }
    device_ = {}; pool_ = {}; pipeline_ = {}; layout_ = {}; setLayout_ = {};
    sets_.clear(); extents_.clear(); capacity_ = 0;
}
}
