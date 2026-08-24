#include "renderer/vulkan/VulkanLayeredSceneResolvePass.h"

#include "renderer/rhi/Mesh.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanVertexUtils.h"
#include "utils/File.h"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

} // namespace

void VulkanLayeredSceneResolvePass::init(VkDevice device,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout globalLayout,
    VkRenderPass sceneRenderPass) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        globalLayout == VK_NULL_HANDLE || sceneRenderPass == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "Invalid layered scene-resolve initialization");
    }
    device_ = device;
    descriptors_ = &descriptors;
    try {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        }};
        VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        descriptorLayoutInfo.bindingCount =
            static_cast<uint32_t>(bindings.size());
        descriptorLayoutInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_,
            &descriptorLayoutInfo, nullptr, &localColorLayout_),
            "vkCreateDescriptorSetLayout(layered scene resolve)");

        const std::array<VkDescriptorSetLayout, 2> setLayouts{
            globalLayout, localColorLayout_ };
        const VkPushConstantRange pushRange{
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0u, sizeof(CanonicalMeshPushConstants) };
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount =
            static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        requireSuccess(vkCreatePipelineLayout(device_, &pipelineLayoutInfo,
            nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(layered scene resolve)");
        pipeline_ = createPipeline(sceneRenderPass);
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkPipeline VulkanLayeredSceneResolvePass::createPipeline(
    VkRenderPass sceneRenderPass) const {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        vertex = createShaderModule(
            "assets/shaders/canonical_material_vert.spv");
        fragment = createShaderModule(
            "assets/shaders/layered_scene_resolve_frag.spv");
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr },
        }};
        const auto binding = VulkanVertexUtils::getBindingDescription();
        const auto attributes = VulkanVertexUtils::getAttributeDescriptions();
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1u;
        vertexInput.pVertexBindingDescriptions = &binding;
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
        dynamicState.dynamicStateCount =
            static_cast<uint32_t>(dynamicStates.size());
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
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1u;
        colorBlend.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = sceneRenderPass;
        VkPipeline pipeline = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1u,
            &pipelineInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines(layered scene resolve)");
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        return pipeline;
    }
    catch (...) {
        if (fragment != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, fragment, nullptr);
        if (vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
}

VkShaderModule VulkanLayeredSceneResolvePass::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &info, nullptr, &module),
        "vkCreateShaderModule(layered scene resolve)");
    return module;
}

void VulkanLayeredSceneResolvePass::rebuildDescriptors(
    const VulkanFrameTargets& frameTargets) {
    if (device_ == VK_NULL_HANDLE || descriptors_ == nullptr)
        throw std::logic_error("Layered scene-resolve pass is not initialized");
    clearDescriptors();
    descriptorSets_.resize(frameTargets.size(), VK_NULL_HANDLE);
    hero4DescriptorSets_.resize(frameTargets.size(), VK_NULL_HANDLE);
    cinematic8DescriptorSets_.resize(frameTargets.size(), VK_NULL_HANDLE);
    try {
        const auto buildSet = [&](VkDescriptorSet& set,
                const VulkanImageResource& localColor,
                const VulkanImageResource& entryIdentity) {
            if (!localColor.isValid() || !entryIdentity.isValid()) return;
            set = descriptors_->allocate(localColorLayout_);
            const std::array<VkDescriptorImageInfo, 2> images{{
                { frameTargets.sampler(), localColor.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                { frameTargets.integerSampler(), entryIdentity.view,
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
            vkUpdateDescriptorSets(device_,
                static_cast<uint32_t>(writes.size()), writes.data(),
                0u, nullptr);
        };
        for (uint32_t frame = 0u; frame < frameTargets.size(); ++frame) {
            const VulkanFrameContextTargets& target = frameTargets.get(frame);
            buildSet(descriptorSets_[frame], target.layeredLocalColor,
                target.layeredEntryIdentity);
            if (target.hero4.active()) {
                buildSet(hero4DescriptorSets_[frame], target.hero4.localColor,
                    target.hero4.interfaceIdentity[0]);
            }
            if (target.cinematic8.active()) {
                buildSet(cinematic8DescriptorSets_[frame],
                    target.cinematic8.localColor,
                    target.cinematic8.interfaceIdentity[0]);
            }
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

VkDescriptorSet VulkanLayeredSceneResolvePass::descriptorSet(
    uint32_t frameIndex, TransparencyQuality quality) const {
    const std::vector<VkDescriptorSet>* sets = &descriptorSets_;
    if (quality == TransparencyQuality::Hero4)
        sets = &hero4DescriptorSets_;
    else if (quality == TransparencyQuality::Cinematic8)
        sets = &cinematic8DescriptorSets_;
    else if (quality != TransparencyQuality::Ordinary2)
        throw std::invalid_argument(
            "Layered scene-resolve quality is invalid");
    if (frameIndex >= sets->size() || (*sets)[frameIndex] == VK_NULL_HANDLE)
        throw std::out_of_range("Layered scene-resolve frame index is invalid");
    return (*sets)[frameIndex];
}

size_t VulkanLayeredSceneResolvePass::descriptorFrameCount(
    TransparencyQuality quality) const noexcept {
    if (quality == TransparencyQuality::Hero4)
        return hero4DescriptorSets_.size();
    if (quality == TransparencyQuality::Cinematic8)
        return cinematic8DescriptorSets_.size();
    return quality == TransparencyQuality::Ordinary2
        ? descriptorSets_.size() : 0u;
}

void VulkanLayeredSceneResolvePass::clearDescriptors() noexcept {
    if (descriptors_ != nullptr) {
        const auto freeSets = [&](std::vector<VkDescriptorSet>& sets) {
            for (VkDescriptorSet set : sets)
                if (set != VK_NULL_HANDLE)
                    try { descriptors_->free(set); } catch (...) {}
            sets.clear();
        };
        freeSets(descriptorSets_);
        freeSets(hero4DescriptorSets_);
        freeSets(cinematic8DescriptorSets_);
    }
    else {
        descriptorSets_.clear();
        hero4DescriptorSets_.clear();
        cinematic8DescriptorSets_.clear();
    }
}

void VulkanLayeredSceneResolvePass::cleanup() noexcept {
    clearDescriptors();
    if (device_ == VK_NULL_HANDLE)
        return;
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (localColorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, localColorLayout_, nullptr);
    device_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    localColorLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
}

} // namespace Iridium
