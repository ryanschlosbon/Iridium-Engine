#include "HeadlessComputeKernel.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Iridium::Test {
namespace {

    void check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed: " +
                std::to_string(static_cast<int>(result)));
    }

    VkDescriptorType bufferType(SpirvDescriptorKind kind) {
        switch (kind) {
        case SpirvDescriptorKind::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case SpirvDescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        default:
            throw std::runtime_error("HeadlessComputeKernel supports buffer bindings only");
        }
    }

} // namespace

HeadlessComputeKernel::HeadlessComputeKernel(const HeadlessVulkanDevice& device,
    const std::filesystem::path& spvPath)
    : device_(device), module_(SpirvModule::load(spvPath)) {
    if (module_.executionModel() != SpirvExecutionModel::GLCompute)
        throw std::runtime_error("not a compute shader: " + spvPath.string());
    const VkDevice vk = device_.device();
    bindings_ = module_.descriptorBindings();
    uint32_t setCount = 0;
    for (const auto& binding : bindings_) setCount = (std::max)(setCount, binding.set + 1u);

    uint32_t uniformCount = 0, storageCount = 0;
    for (uint32_t set = 0; set < setCount; ++set) {
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        for (const auto& binding : bindings_) {
            if (binding.set != set) continue;
            const VkDescriptorType type = bufferType(binding.kind);
            (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? uniformCount : storageCount) += 1;
            layoutBindings.push_back({ binding.binding, type, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
        }
        VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = static_cast<uint32_t>(layoutBindings.size());
        info.pBindings = layoutBindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(vk, &info, nullptr, &layout),
            "vkCreateDescriptorSetLayout");
        setLayouts_.push_back(layout);
    }

    std::vector<VkDescriptorPoolSize> sizes;
    if (uniformCount) sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformCount });
    if (storageCount) sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageCount });
    if (!setLayouts_.empty()) {
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = static_cast<uint32_t>(setLayouts_.size());
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(vk, &poolInfo, nullptr, &pool_), "vkCreateDescriptorPool");
        sets_.resize(setLayouts_.size());
        VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocation.descriptorPool = pool_;
        allocation.descriptorSetCount = static_cast<uint32_t>(setLayouts_.size());
        allocation.pSetLayouts = setLayouts_.data();
        check(vkAllocateDescriptorSets(vk, &allocation, sets_.data()),
            "vkAllocateDescriptorSets");
    }

    pushBytes_ = (module_.pushConstantExtent() + 3u) & ~3u;
    const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes_ };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts_.size());
    layoutInfo.pSetLayouts = setLayouts_.data();
    layoutInfo.pushConstantRangeCount = pushBytes_ != 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout");

    shader_ = device_.createShaderModule(spvPath);
    VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader_;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout_;
    check(vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &pipeline_), "vkCreateComputePipelines");
}

HeadlessComputeKernel::~HeadlessComputeKernel() {
    const VkDevice vk = device_.device();
    if (pipeline_) vkDestroyPipeline(vk, pipeline_, nullptr);
    if (shader_) vkDestroyShaderModule(vk, shader_, nullptr);
    if (layout_) vkDestroyPipelineLayout(vk, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(vk, pool_, nullptr);
    for (VkDescriptorSetLayout layout : setLayouts_)
        vkDestroyDescriptorSetLayout(vk, layout, nullptr);
}

void HeadlessComputeKernel::dispatch(
    const std::map<std::pair<uint32_t, uint32_t>, VkBuffer>& buffers,
    std::span<const std::byte> pushConstants, uint32_t groupCountX) {
    if (pushConstants.size() > pushBytes_)
        throw std::runtime_error("push constants exceed the reflected block");
    std::vector<VkDescriptorBufferInfo> infos(bindings_.size());
    std::vector<VkWriteDescriptorSet> writes;
    for (size_t index = 0; index < bindings_.size(); ++index) {
        const auto& binding = bindings_[index];
        const auto found = buffers.find({ binding.set, binding.binding });
        if (found == buffers.end())
            throw std::runtime_error("unbound descriptor " + binding.name);
        infos[index] = { found->second, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = sets_[binding.set];
        write.dstBinding = binding.binding;
        write.descriptorCount = 1;
        write.descriptorType = bufferType(binding.kind);
        write.pBufferInfo = &infos[index];
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device_.device(), static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr);
    std::vector<std::byte> push(pushBytes_);
    std::copy(pushConstants.begin(), pushConstants.end(), push.begin());
    device_.submitAndWait([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        if (!sets_.empty())
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                static_cast<uint32_t>(sets_.size()), sets_.data(), 0, nullptr);
        if (pushBytes_ != 0)
            vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                pushBytes_, push.data());
        vkCmdDispatch(commands, groupCountX, 1, 1);
        const VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    });
}

} // namespace Iridium::Test
