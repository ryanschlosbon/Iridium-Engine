#include "renderer/vulkan/VulkanWeightedOitPass.h"

#include "renderer/rhi/Mesh.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanVertexUtils.h"
#include "utils/File.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

    VkPipelineColorBlendAttachmentState additiveBlend() {
        VkPipelineColorBlendAttachmentState result{};
        result.blendEnable = VK_TRUE;
        result.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        result.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        result.colorBlendOp = VK_BLEND_OP_ADD;
        result.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        result.alphaBlendOp = VK_BLEND_OP_ADD;
        result.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        return result;
    }

    VkPipelineColorBlendAttachmentState revealageBlend() {
        VkPipelineColorBlendAttachmentState result{};
        result.blendEnable = VK_TRUE;
        result.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        result.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        result.colorBlendOp = VK_BLEND_OP_ADD;
        result.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        result.alphaBlendOp = VK_BLEND_OP_ADD;
        result.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        return result;
    }

    VkPipelineColorBlendAttachmentState premultipliedBlend() {
        VkPipelineColorBlendAttachmentState result{};
        result.blendEnable = VK_TRUE;
        result.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        result.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        result.colorBlendOp = VK_BLEND_OP_ADD;
        result.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        result.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        result.alphaBlendOp = VK_BLEND_OP_ADD;
        result.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        return result;
    }

} // namespace

void VulkanWeightedOitPass::init(VkDevice device,
    ::DescriptorAllocator& descriptors,
    VkPipelineLayout forwardPipelineLayout) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        forwardPipelineLayout == VK_NULL_HANDLE) {
        throw std::invalid_argument("Invalid WeightedOIT initialization");
    }
    device_ = device;
    descriptors_ = &descriptors;
    accumulationPipelineLayout_ = forwardPipelineLayout;
    try {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        }};
        VkDescriptorSetLayoutCreateInfo descriptorInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_, &descriptorInfo,
            nullptr, &resolveDescriptorLayout_),
            "vkCreateDescriptorSetLayout(WeightedOIT resolve)");
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1u;
        layoutInfo.pSetLayouts = &resolveDescriptorLayout_;
        const VkPushConstantRange resolvePushRange{
            VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(uint32_t) };
        layoutInfo.pushConstantRangeCount = 1u;
        layoutInfo.pPushConstantRanges = &resolvePushRange;
        requireSuccess(vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
            &resolvePipelineLayout_),
            "vkCreatePipelineLayout(WeightedOIT resolve)");
        accumulationPipeline_ = createAccumulationPipeline();
        resolvePipeline_ = createResolvePipeline();
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkPipeline VulkanWeightedOitPass::createAccumulationPipeline() const {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        vertex = createShaderModule(
            "assets/shaders/weighted_oit_instanced_vert.spv");
        fragment = createShaderModule(
            "assets/shaders/weighted_oit_material_indexed_frag.spv");
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr },
        }};
        const auto vertexBinding = VulkanVertexUtils::getBindingDescription();
        const auto vertexAttributes = VulkanVertexUtils::getAttributeDescriptions();
        const std::array<VkVertexInputBindingDescription, 2> bindings{{
            vertexBinding,
            { 1u, sizeof(glm::mat4), VK_VERTEX_INPUT_RATE_INSTANCE },
        }};
        std::array<VkVertexInputAttributeDescription, 10> attributes{};
        std::copy(vertexAttributes.begin(), vertexAttributes.end(),
            attributes.begin());
        for (uint32_t column = 0u; column < 4u; ++column) {
            attributes[6u + column] = {
                6u + column, 1u, VK_FORMAT_R32G32B32A32_SFLOAT,
                column * sizeof(glm::vec4) };
        }
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(
            bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const std::array<VkDynamicState, 2> dynamicStates{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();
        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1u;
        viewportState.scissorCount = 1u;
        VkPipelineRasterizationStateCreateInfo rasterizer{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisampling{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        const std::array<VkPipelineColorBlendAttachmentState, 2> blends{
            additiveBlend(), revealageBlend() };
        VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = static_cast<uint32_t>(blends.size());
        colorBlend.pAttachments = blends.data();
        VkGraphicsPipelineCreateInfo info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamicState;
        info.layout = accumulationPipelineLayout_;
        // R4a: dynamic rendering into accumulation + revealage, testing the
        // opaque depth read-only (DEPTH_STENCIL_READ_ONLY_OPTIMAL, STORE_OP_NONE).
        const std::array<VkFormat, 2> colorFormats{
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16_SFLOAT };
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
        rendering.pColorAttachmentFormats = colorFormats.data();
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        info.pNext = &rendering;
        info.renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1u,
            &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines(WeightedOIT accumulation)");
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        return pipeline;
    }
    catch (...) {
        if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragment, nullptr);
        if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
}

VkPipeline VulkanWeightedOitPass::createResolvePipeline() const {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        vertex = createShaderModule("assets/shaders/output_vert.spv");
        fragment = createShaderModule("assets/shaders/weighted_oit_resolve_frag.spv");
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr },
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const std::array<VkDynamicState, 2> dynamicStates{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();
        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1u;
        viewportState.scissorCount = 1u;
        VkPipelineRasterizationStateCreateInfo rasterizer{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisampling{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        const VkPipelineColorBlendAttachmentState blend = premultipliedBlend();
        VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1u;
        colorBlend.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamicState;
        info.layout = resolvePipelineLayout_;
        // R4a: dynamic rendering into scene colour (no depth attachment).
        const VkFormat colorFormat = VulkanSceneColorFormat;
        VkPipelineRenderingCreateInfo rendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1u;
        rendering.pColorAttachmentFormats = &colorFormat;
        info.pNext = &rendering;
        info.renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1u,
            &info, nullptr, &pipeline),
            "vkCreateGraphicsPipelines(WeightedOIT resolve)");
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        return pipeline;
    }
    catch (...) {
        if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragment, nullptr);
        if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
}

VkShaderModule VulkanWeightedOitPass::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &info, nullptr, &module),
        "vkCreateShaderModule(WeightedOIT)");
    return module;
}

void VulkanWeightedOitPass::rebuildDescriptors(
    const VulkanFrameTargets& frameTargets) {
    if (device_ == VK_NULL_HANDLE || descriptors_ == nullptr)
        throw std::logic_error("WeightedOIT pass is not initialized");
    clearDescriptors();
    resolveDescriptorSets_.resize(frameTargets.size(), VK_NULL_HANDLE);
    try {
        for (uint32_t frame = 0u; frame < frameTargets.size(); ++frame) {
            const VulkanFrameContextTargets& target = frameTargets.get(frame);
            if (!target.weightedOitAccumulation.isValid() ||
                !target.weightedOitRevealage.isValid()) {
                continue;
            }
            VkDescriptorSet& set = resolveDescriptorSets_[frame];
            set = descriptors_->allocate(resolveDescriptorLayout_);
            const std::array<VkDescriptorImageInfo, 2> images{{
                { frameTargets.sampler(), target.weightedOitAccumulation.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { frameTargets.sampler(), target.weightedOitRevealage.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            }};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t binding = 0u; binding < writes.size(); ++binding) {
                writes[binding] = {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                    binding, 0u, 1u,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    &images[binding], nullptr, nullptr };
            }
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                writes.data(), 0u, nullptr);
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

VkDescriptorSet VulkanWeightedOitPass::resolveDescriptorSet(
    uint32_t frameIndex) const {
    if (frameIndex >= resolveDescriptorSets_.size() ||
        resolveDescriptorSets_[frameIndex] == VK_NULL_HANDLE) {
        throw std::out_of_range("WeightedOIT resolve frame index is invalid");
    }
    return resolveDescriptorSets_[frameIndex];
}

void VulkanWeightedOitPass::clearDescriptors() noexcept {
    if (descriptors_ != nullptr) {
        for (VkDescriptorSet set : resolveDescriptorSets_) {
            if (set != VK_NULL_HANDLE) {
                try { descriptors_->free(set); } catch (...) {}
            }
        }
    }
    resolveDescriptorSets_.clear();
}

void VulkanWeightedOitPass::cleanup() noexcept {
    clearDescriptors();
    if (device_ == VK_NULL_HANDLE) return;
    if (resolvePipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, resolvePipeline_, nullptr);
    if (accumulationPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, accumulationPipeline_, nullptr);
    if (resolvePipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, resolvePipelineLayout_, nullptr);
    if (resolveDescriptorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, resolveDescriptorLayout_, nullptr);
    device_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    accumulationPipelineLayout_ = VK_NULL_HANDLE;
    resolveDescriptorLayout_ = VK_NULL_HANDLE;
    resolvePipelineLayout_ = VK_NULL_HANDLE;
    accumulationPipeline_ = VK_NULL_HANDLE;
    resolvePipeline_ = VK_NULL_HANDLE;
}

} // namespace Iridium
