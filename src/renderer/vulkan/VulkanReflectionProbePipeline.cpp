#include "renderer/vulkan/VulkanReflectionProbePipeline.h"

#include "utils/File.h"

#include <array>
#include <stdexcept>
#include <string>

namespace Iridium {

    VulkanReflectionProbePipeline::~VulkanReflectionProbePipeline() {
        cleanup();
    }

    void VulkanReflectionProbePipeline::init(VkDevice device,
        ::DescriptorAllocator& allocator) {
        if (device == VK_NULL_HANDLE || device_ != VK_NULL_HANDLE)
            throw std::logic_error(
                "Reflection-probe pipeline initialized in an invalid state");
        device_ = device;
        allocator_ = &allocator;
        try {
            std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
            for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
                bindings[binding].binding = binding;
                bindings[binding].descriptorType = binding == 2
                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                    : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[binding].descriptorCount = 1;
                bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo setInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            setInfo.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
                    &descriptorSetLayout_) != VK_SUCCESS)
                throw std::runtime_error(
                    "Failed to create reflection-probe descriptor layout");

            VkPipelineLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &descriptorSetLayout_;
            if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                    &pipelineLayout_) != VK_SUCCESS)
                throw std::runtime_error(
                    "Failed to create reflection-probe pipeline layout");

            const std::vector<char> code = readFile(
                std::string(PROJECT_ROOT_DIR) +
                "assets/shaders/reflection_probe_cluster_comp.spv");
            VkShaderModuleCreateInfo moduleInfo{
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            moduleInfo.codeSize = code.size();
            moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_, &moduleInfo, nullptr,
                    &module) != VK_SUCCESS)
                throw std::runtime_error(
                    "Failed to create reflection-probe shader module");
            VkComputePipelineCreateInfo pipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipelineInfo.layout = pipelineLayout_;
            pipelineInfo.stage.sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = module;
            pipelineInfo.stage.pName = "main";
            const VkResult result = vkCreateComputePipelines(device_,
                VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
            vkDestroyShaderModule(device_, module, nullptr);
            if (result != VK_SUCCESS)
                throw std::runtime_error(
                    "Failed to create reflection-probe compute pipeline");
        }
        catch (...) {
            cleanup();
            throw;
        }
    }

    void VulkanReflectionProbePipeline::rebuildDescriptors(
        std::span<const VkDescriptorBufferInfo> records,
        std::span<const VkDescriptorBufferInfo> activeSlots,
        std::span<const VkDescriptorBufferInfo> parameters,
        std::span<const VkDescriptorBufferInfo> headers,
        std::span<const VkDescriptorBufferInfo> indices) {
        const size_t frameCount = records.size();
        if (device_ == VK_NULL_HANDLE || allocator_ == nullptr ||
            frameCount == 0 || activeSlots.size() != frameCount ||
            parameters.size() != frameCount || headers.size() != frameCount ||
            indices.size() != frameCount)
            throw std::invalid_argument(
                "Reflection-probe descriptors require matching frame inputs");
        clearDescriptors();
        try {
            descriptorSets_.reserve(frameCount);
            for (uint32_t frame = 0; frame < frameCount; ++frame) {
                descriptorSets_.push_back(
                    allocator_->allocate(descriptorSetLayout_));
                const std::array<VkDescriptorBufferInfo, 5> buffers{
                    records[frame], activeSlots[frame], parameters[frame],
                    headers[frame], indices[frame] };
                std::array<VkWriteDescriptorSet, 5> writes{};
                for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding].sType =
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = descriptorSets_.back();
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorType = binding == 2
                        ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[binding].descriptorCount = 1;
                    writes[binding].pBufferInfo = &buffers[binding];
                }
                vkUpdateDescriptorSets(device_,
                    static_cast<uint32_t>(writes.size()), writes.data(),
                    0, nullptr);
            }
        }
        catch (...) {
            clearDescriptors();
            throw;
        }
    }

    void VulkanReflectionProbePipeline::clearDescriptors() {
        if (allocator_ != nullptr && !descriptorSets_.empty())
            allocator_->free(std::span<const VkDescriptorSet>(descriptorSets_));
        descriptorSets_.clear();
    }

    uint32_t VulkanReflectionProbePipeline::record(
        VkCommandBuffer commandBuffer, uint32_t frameIndex,
        uint32_t clusterCount) {
        if (commandBuffer == VK_NULL_HANDLE || pipeline_ == VK_NULL_HANDLE ||
            frameIndex >= descriptorSets_.size())
            throw std::logic_error(
                "Reflection-probe dispatch is not ready");
        if (clusterCount == 0) return 0;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
        vkCmdDispatch(commandBuffer, (clusterCount + 63u) / 64u, 1, 1);
        VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            1, &barrier, 0, nullptr, 0, nullptr);
        return 1;
    }

    void VulkanReflectionProbePipeline::cleanup() noexcept {
        try { clearDescriptors(); }
        catch (...) { descriptorSets_.clear(); }
        if (device_ != VK_NULL_HANDLE) {
            if (pipeline_ != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipelineLayout_ != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (descriptorSetLayout_ != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_,
                    nullptr);
        }
        device_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        descriptorSetLayout_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
    }

} // namespace Iridium
