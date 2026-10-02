#include "renderer/vulkan/VulkanClusteredLightingPipeline.h"

#include "utils/File.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    VkDescriptorBufferInfo graphBuffer(const VulkanRenderGraphExecutor& graph,
        uint32_t frameIndex, const char* name) {
        const VulkanBufferResource& buffer = graph.bufferResource(frameIndex, name);
        return { buffer.buffer, 0, buffer.size };
    }

} // namespace

VulkanClusteredLightingPipeline::~VulkanClusteredLightingPipeline() {
    cleanup();
}

void VulkanClusteredLightingPipeline::init(VkDevice device,
    ::DescriptorAllocator& allocator) {
    if (device == VK_NULL_HANDLE || device_ != VK_NULL_HANDLE) {
        throw std::logic_error(
            "Clustered-lighting pipeline initialized in an invalid state");
    }
    device_ = device;
    allocator_ = &allocator;
    try {
        std::array<VkDescriptorSetLayoutBinding, BindingCount> bindings{};
        for (uint32_t binding = 0; binding < BindingCount; ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = binding == 3
                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = BindingCount;
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
                &descriptorSetLayout_) != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create clustered-lighting descriptor layout");
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = sizeof(ScanPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                &pipelineLayout_) != VK_SUCCESS) {
            throw std::runtime_error(
                "Failed to create clustered-lighting pipeline layout");
        }
        clearPipeline_ = createPipeline("assets/shaders/cluster_clear_comp.spv");
        countPipeline_ = createPipeline("assets/shaders/cluster_count_comp.spv");
        scanPipeline_ = createPipeline("assets/shaders/cluster_scan_comp.spv");
        fillPipeline_ = createPipeline("assets/shaders/cluster_fill_comp.spv");
        sortPreparePipeline_ = createPipeline(
            "assets/shaders/cluster_sort_prepare_comp.spv");
        sortPipeline_ = createPipeline("assets/shaders/cluster_sort_comp.spv");
        denseSortPipeline_ = createPipeline(
            "assets/shaders/cluster_sort_dense_comp.spv");
        finalizePipeline_ = createPipeline(
            "assets/shaders/cluster_finalize_comp.spv");
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkPipeline VulkanClusteredLightingPipeline::createPipeline(
    const char* shaderPath) {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + shaderPath);
    VkShaderModuleCreateInfo moduleInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error(
            "Failed to create clustered-lighting shader module");
    }
    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE,
        1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(device_, module, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            "Failed to create clustered-lighting compute pipeline");
    }
    return pipeline;
}

void VulkanClusteredLightingPipeline::rebuildDescriptors(
    const VulkanRenderGraphExecutor& graph,
    std::span<const VkDescriptorBufferInfo> lightRecords,
    std::span<const VkDescriptorBufferInfo> activeSlots,
    std::span<const VkDescriptorBufferInfo> fallbackCandidates,
    std::span<const VkDescriptorBufferInfo> parameters) {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr ||
        lightRecords.empty() || activeSlots.size() != lightRecords.size() ||
        fallbackCandidates.size() != lightRecords.size() ||
        parameters.size() != lightRecords.size()) {
        throw std::invalid_argument(
            "Clustered-lighting descriptors require matching frame inputs");
    }
    clearDescriptors();
    try {
        descriptorSets_.reserve(lightRecords.size());
        for (uint32_t frame = 0; frame < lightRecords.size(); ++frame) {
            descriptorSets_.push_back(allocator_->allocate(descriptorSetLayout_));
            std::array<VkDescriptorBufferInfo, BindingCount> buffers{
                lightRecords[frame], activeSlots[frame], fallbackCandidates[frame],
                parameters[frame],
                graphBuffer(graph, frame, kClusterGlobalResourceName),
                graphBuffer(graph, frame, kClusterHeaderResourceName),
                graphBuffer(graph, frame, kClusterIndexResourceName),
                graphBuffer(graph, frame, kClusterFallbackResourceName),
                graphBuffer(graph, frame, kClusterDiagnosticResourceName),
                graphBuffer(graph, frame, kClusterCountResourceName),
                graphBuffer(graph, frame, kClusterCursorResourceName),
                graphBuffer(graph, frame, kClusterScanScratchResourceName),
                graphBuffer(graph, frame, kClusterIndirectResourceName),
            };
            std::array<VkWriteDescriptorSet, BindingCount> writes{};
            for (uint32_t binding = 0; binding < BindingCount; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = descriptorSets_.back();
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = binding == 3
                    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                    : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[binding].pBufferInfo = &buffers[binding];
            }
            vkUpdateDescriptorSets(device_, BindingCount, writes.data(), 0, nullptr);
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

void VulkanClusteredLightingPipeline::clearDescriptors() {
    if (allocator_ != nullptr && !descriptorSets_.empty()) {
        allocator_->free(std::span<const VkDescriptorSet>(descriptorSets_));
    }
    descriptorSets_.clear();
}

void VulkanClusteredLightingPipeline::bindAndDispatch(
    VkCommandBuffer commandBuffer, VkPipeline pipeline, uint32_t frameIndex,
    uint32_t groupsX, uint32_t groupsY) {
    if (frameIndex >= descriptorSets_.size() || groupsX == 0 || groupsY == 0) {
        throw std::out_of_range(
            "Clustered-lighting dispatch is outside the prepared frame");
    }
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    vkCmdDispatch(commandBuffer, groupsX, groupsY, 1);
}

void VulkanClusteredLightingPipeline::computeBarrier(
    VkCommandBuffer commandBuffer) {
    VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
        0, nullptr, 0, nullptr);
}

uint32_t VulkanClusteredLightingPipeline::record(VkCommandBuffer commandBuffer,
    VulkanRenderGraphExecutor& graph, uint32_t frameIndex,
    uint32_t clusterCount, uint32_t activeLightCount) {
    if (commandBuffer == VK_NULL_HANDLE || clusterCount == 0) {
        throw std::invalid_argument(
            "Clustered-lighting recording requires a valid frame");
    }
    graph.beginPass(commandBuffer, "lighting.cluster.clear");
    bindAndDispatch(commandBuffer, clearPipeline_, frameIndex,
        (clusterCount + 255u) / 256u);
    uint32_t dispatchCount = 1;

    graph.beginPass(commandBuffer, "lighting.cluster.count");
    if (activeLightCount != 0) {
        bindAndDispatch(commandBuffer, countPipeline_, frameIndex,
            activeLightCount);
        ++dispatchCount;
    }

    graph.beginPass(commandBuffer, "lighting.cluster.scan");
    std::array<uint32_t, 8> levelCounts{};
    std::array<uint32_t, 8> levelOffsets{};
    uint32_t levelCount = 0;
    uint32_t inputCount = clusterCount;
    uint32_t scratchOffset = 0;
    while (inputCount > 1) {
        const uint32_t blockCount = (inputCount + 255u) / 256u;
        if (levelCount >= levelCounts.size()) {
            throw std::overflow_error("Cluster scan hierarchy exceeded its bound");
        }
        levelCounts[levelCount] = blockCount;
        levelOffsets[levelCount] = scratchOffset;
        const ScanPushConstants push{
            levelCount == 0 ? 0u : 1u,
            levelCount == 0 ? 0u : levelOffsets[levelCount - 1],
            scratchOffset,
            inputCount,
        };
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            scanPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer, blockCount, 1, 1);
        ++dispatchCount;
        computeBarrier(commandBuffer);
        scratchOffset += blockCount;
        inputCount = blockCount;
        ++levelCount;
    }
    if (levelCount > 2) {
        for (uint32_t parent = levelCount - 2; parent >= 1; --parent) {
            const uint32_t child = parent - 1;
            const ScanPushConstants push{ 2u, levelOffsets[child],
                levelOffsets[parent], levelCounts[child] };
            vkCmdPushConstants(commandBuffer, pipelineLayout_,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(commandBuffer,
                (push.elementCount + 255u) / 256u, 1, 1);
            ++dispatchCount;
            computeBarrier(commandBuffer);
        }
    }
    const ScanPushConstants headerAdd{ 3u, levelOffsets[0], 0u,
        clusterCount };
    vkCmdPushConstants(commandBuffer, pipelineLayout_,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(headerAdd), &headerAdd);
    vkCmdDispatch(commandBuffer, (clusterCount + 255u) / 256u, 1, 1);
    ++dispatchCount;

    graph.beginPass(commandBuffer, "lighting.cluster.fill");
    if (activeLightCount != 0) {
        bindAndDispatch(commandBuffer, fillPipeline_, frameIndex,
            activeLightCount);
        ++dispatchCount;
    }
    computeBarrier(commandBuffer);
    bindAndDispatch(commandBuffer, sortPreparePipeline_, frameIndex, 1);
    ++dispatchCount;

    graph.beginPass(commandBuffer, "lighting.cluster.finalize");
    const VulkanBufferResource& indirect = graph.bufferResource(
        frameIndex, kClusterIndirectResourceName);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        sortPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    vkCmdDispatchIndirect(commandBuffer, indirect.buffer, 0);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        denseSortPipeline_);
    vkCmdDispatchIndirect(commandBuffer, indirect.buffer, 16);
    computeBarrier(commandBuffer);
    bindAndDispatch(commandBuffer, finalizePipeline_, frameIndex, 1);
    return dispatchCount + 3;
}

void VulkanClusteredLightingPipeline::cleanup() noexcept {
    clearDescriptors();
    if (device_ != VK_NULL_HANDLE) {
        for (VkPipeline pipeline : { clearPipeline_, countPipeline_,
                scanPipeline_, fillPipeline_, sortPreparePipeline_, sortPipeline_,
                denseSortPipeline_, finalizePipeline_ }) {
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device_, pipeline, nullptr);
        }
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorSetLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    }
    clearPipeline_ = countPipeline_ = scanPipeline_ = fillPipeline_ =
        sortPreparePipeline_ = sortPipeline_ = denseSortPipeline_ =
        finalizePipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

} // namespace Iridium
