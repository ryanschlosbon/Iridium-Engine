#include "VulkanVirtualShadowFullViewPass.h"
#include <array>
#include <algorithm>
#include <limits>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace Iridium {
namespace {
void require(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Virtual shadow full-view marker Vulkan failure");
}
struct Push { glm::mat4 inverse; std::array<uint32_t, 4> source, policy; };
static_assert(sizeof(Push) == 96);
}
void VulkanVirtualShadowFullViewPass::init(VkDevice device, VkPipelineCache pipelineCache,
    std::span<const VkBuffer> buffers, const VirtualShadowGpuWorkingSetLayout& working,
    const std::filesystem::path& shaderDirectory) {
    if (!device || device_ || buffers.empty() || buffers.size() > 8 || !working.scratchCapacity)
        throw std::invalid_argument("Invalid full-view marker initialization");
    device_ = device; capacity_ = working.scratchCapacity;
    pipelineCache_ = pipelineCache;
    try {
        const uint32_t count = static_cast<uint32_t>(buffers.size());
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * 2u},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = count; pool.poolSizeCount = 2; pool.pPoolSizes = sizes.data();
        require(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_));
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 3; set.pBindings = bindings.data();
        require(vkCreateDescriptorSetLayout(device_, &set, nullptr, &setLayout_));
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &setLayout_;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        require(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_));
        std::ifstream file(shaderDirectory / "virtual_shadow_full_view_mark_comp.spv", std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open full-view marker shader");
        const auto bytes = file.tellg();
        if (bytes <= 0 || bytes % 4) throw std::runtime_error("Invalid full-view marker SPIR-V");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), bytes);
        if (!file) throw std::runtime_error("Cannot read full-view marker shader");
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
            const std::array infos{
                VkDescriptorBufferInfo{buffers[i], working.clipLevels.offset, working.clipLevels.size},
                VkDescriptorBufferInfo{buffers[i], working.rawMarks.offset, working.rawMarks.size}};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t binding = 0; binding < 2; ++binding) {
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[binding].dstSet = sets_[i]; writes[binding].dstBinding = binding + 1;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[binding].pBufferInfo = &infos[binding];
            }
            vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
        }
    } catch (...) { cleanup(); throw; }
}
void VulkanVirtualShadowFullViewPass::bindDepth(uint32_t slot, VkImageView view,
    VkSampler sampler, VkExtent2D extent, VkImageLayout imageLayout) {
    if (!device_ || slot >= sets_.size() || !view || !sampler || !extent.width || !extent.height ||
        (imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
         imageLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))
        throw std::invalid_argument("Invalid full-view marker image binding");
    const VkDescriptorImageInfo info{sampler, view, imageLayout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[slot]; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    extents_[slot] = extent;
}
uint32_t VulkanVirtualShadowFullViewPass::record(VkCommandBuffer command, uint32_t slot,
    const DirectionalVirtualShadowMarkConfig& config,
    std::span<const DirectionalVirtualShadowClipLevel> levels,
    const glm::mat4& inverse, bool reverseDepth) const {
    if (!device_ || !command || slot >= sets_.size() || !extents_[slot].width ||
        !extents_[slot].height || !config.pageSizeTexels || uint64_t{extents_[slot].width} * extents_[slot].height >
            std::numeric_limits<uint32_t>::max() ||
        config.finerLevelGuardBandPages > UINT32_MAX / config.pageSizeTexels)
        throw std::invalid_argument("Invalid full-view depth input");
    const auto grid = buildVirtualShadowFullViewPageGrid(config, levels, capacity_);
    for (uint32_t c = 0; c < 4; ++c) for (uint32_t r = 0; r < 4; ++r)
        if (!std::isfinite(inverse[c][r])) throw std::invalid_argument("Invalid inverse view projection");
    uint32_t coarsest = 0;
    for (const auto& level : levels) coarsest = (std::max)(coarsest, uint32_t{level.level});
    Push push{inverse, {extents_[slot].width, extents_[slot].height,
        static_cast<uint32_t>(levels.size()), config.pageSizeTexels},
        {config.finerLevelGuardBandPages, coarsest, reverseDepth ? 1u : 0u, 0u}};
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[slot], 0, nullptr);
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command, (grid.cellCount + 63u) / 64u, 1, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    push.policy[3] = 1; // Stage one scans every depth pixel.
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint64_t pixelCount = uint64_t{extents_[slot].width} * extents_[slot].height;
    const uint32_t groups = static_cast<uint32_t>((pixelCount + 63u) / 64u);
    const uint32_t x = (std::min)(groups, 65'535u);
    vkCmdDispatch(command, x, (groups + x - 1u) / x, 1);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    return grid.cellCount;
}
void VulkanVirtualShadowFullViewPass::cleanup() noexcept {
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
