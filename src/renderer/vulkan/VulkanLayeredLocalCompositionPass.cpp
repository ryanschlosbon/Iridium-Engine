#include "renderer/vulkan/VulkanLayeredLocalCompositionPass.h"

#include "renderer/rhi/Mesh.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanVertexUtils.h"
#include "utils/File.h"

#include <algorithm>
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

void VulkanLayeredLocalCompositionPass::init(VkDevice device,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout globalLayout,
    VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout samplerLayout,
    VkDescriptorSetLayout sceneLayout) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        globalLayout == VK_NULL_HANDLE || materialLayout == VK_NULL_HANDLE ||
        samplerLayout == VK_NULL_HANDLE || sceneLayout == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "Invalid layered local-composition initialization");
    }
    device_ = device;
    descriptors_ = &descriptors;
    try {
        VkAttachmentDescription color{};
        color.format = VulkanSceneColorFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference colorReference{
            0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1u;
        subpass.pColorAttachments = &colorReference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0u;
        dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderPassInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPassInfo.attachmentCount = 1u;
        renderPassInfo.pAttachments = &color;
        renderPassInfo.subpassCount = 1u;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1u;
        renderPassInfo.pDependencies = &dependency;
        requireSuccess(vkCreateRenderPass(device_, &renderPassInfo, nullptr,
            &renderPass_),
            "vkCreateRenderPass(layered local composition)");

        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (uint32_t binding = 0u; binding < bindings.size(); ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[binding].descriptorCount = 1u;
            bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo interfaceLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        interfaceLayoutInfo.bindingCount =
            static_cast<uint32_t>(bindings.size());
        interfaceLayoutInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_,
            &interfaceLayoutInfo, nullptr, &interfaceLayout_),
            "vkCreateDescriptorSetLayout(layered local composition)");

        std::array<VkDescriptorSetLayoutBinding, 2> deepBindings{};
        for (uint32_t binding = 0u; binding < deepBindings.size(); ++binding) {
            deepBindings[binding].binding = binding;
            deepBindings[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            deepBindings[binding].descriptorCount =
                kMaximumLayeredInterfaceCount;
            deepBindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo deepInterfaceLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        deepInterfaceLayoutInfo.bindingCount =
            static_cast<uint32_t>(deepBindings.size());
        deepInterfaceLayoutInfo.pBindings = deepBindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_,
            &deepInterfaceLayoutInfo, nullptr, &deepInterfaceLayout_),
            "vkCreateDescriptorSetLayout(deep layered local composition)");

        const std::array<VkDescriptorSetLayout, 5> setLayouts{
            globalLayout, materialLayout, samplerLayout, sceneLayout,
            interfaceLayout_ };
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
            "vkCreatePipelineLayout(layered local composition)");
        const std::array<VkDescriptorSetLayout, 5> deepSetLayouts{
            globalLayout, materialLayout, samplerLayout, sceneLayout,
            deepInterfaceLayout_ };
        pipelineLayoutInfo.pSetLayouts = deepSetLayouts.data();
        requireSuccess(vkCreatePipelineLayout(device_, &pipelineLayoutInfo,
            nullptr, &deepPipelineLayout_),
            "vkCreatePipelineLayout(deep layered local composition)");
        pipeline_ = createPipeline(
            "assets/shaders/layered_ordinary2_material_indexed_frag.spv",
            pipelineLayout_, false);
        deepPipeline_ = createPipeline(
            "assets/shaders/layered_deep_material_indexed_frag.spv",
            deepPipelineLayout_, true);
        deepResidualPipeline_ = createPipeline(
            "assets/shaders/layered_deep_residual_material_indexed_frag.spv",
            deepPipelineLayout_, true);
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkPipeline VulkanLayeredLocalCompositionPass::createPipeline(
    const char* fragmentPath, VkPipelineLayout layout,
    bool premultipliedBlend) const {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        vertex = createShaderModule(
            "assets/shaders/canonical_material_vert.spv");
        fragment = createShaderModule(fragmentPath);
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
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        if (premultipliedBlend) {
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
        }
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
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = layout;
        pipelineInfo.renderPass = renderPass_;
        VkPipeline pipeline = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1u,
            &pipelineInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines(layered local composition)");
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

VkShaderModule VulkanLayeredLocalCompositionPass::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &info, nullptr, &module),
        "vkCreateShaderModule(layered local composition)");
    return module;
}

void VulkanLayeredLocalCompositionPass::rebuildDescriptors(
    const VulkanFrameTargets& frameTargets) {
    if (device_ == VK_NULL_HANDLE || descriptors_ == nullptr)
        throw std::logic_error(
            "Layered local-composition pass is not initialized");
    clearDescriptors();
    descriptorSets_.resize(frameTargets.size());
    try {
        for (uint32_t frame = 0u; frame < frameTargets.size(); ++frame) {
            const VulkanFrameContextTargets& target = frameTargets.get(frame);
            FrameDescriptorSets& frameSets = descriptorSets_[frame];
            const bool anyOrdinary2 = target.layeredEntryDepth.isValid() ||
                target.layeredEntryIdentity.isValid() ||
                target.layeredExitDepth.isValid() ||
                target.layeredExitIdentity.isValid();
            if (anyOrdinary2) {
                if (!target.layeredEntryDepth.isValid() ||
                    !target.layeredEntryIdentity.isValid() ||
                    !target.layeredExitDepth.isValid() ||
                    !target.layeredExitIdentity.isValid()) {
                    throw std::logic_error(
                        "Incomplete Ordinary2 local-composition targets");
                }
                frameSets.ordinary2 = descriptors_->allocate(interfaceLayout_);
                const std::array<VkDescriptorImageInfo, 4> images{{
                    { frameTargets.sampler(), target.layeredEntryDepth.view,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
                    { frameTargets.integerSampler(),
                        target.layeredEntryIdentity.view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                    { frameTargets.sampler(), target.layeredExitDepth.view,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
                    { frameTargets.integerSampler(),
                        target.layeredExitIdentity.view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                }};
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (uint32_t binding = 0u; binding < writes.size(); ++binding) {
                    writes[binding] = {
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                        frameSets.ordinary2, binding, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        &images[binding], nullptr, nullptr };
                }
                vkUpdateDescriptorSets(device_,
                    static_cast<uint32_t>(writes.size()), writes.data(),
                    0u, nullptr);
            }

            const auto buildDeepSet = [&](const auto& tier,
                    VkDescriptorSet& set, uint32_t& publishedCount,
                    const char* tierName) {
                if (tier.interfaceCount == 0u) return;
                if (tier.interfaceCount > kMaximumLayeredInterfaceCount)
                    throw std::logic_error(std::string(
                        "Deep local-composition interface capacity exceeded for ") +
                        tierName);
                for (uint32_t index = 0u; index < tier.interfaceCount; ++index) {
                    if (!tier.interfaceDepth[index].isValid() ||
                        !tier.interfaceIdentity[index].isValid()) {
                        throw std::logic_error(std::string(
                            "Incomplete deep local-composition targets for ") +
                            tierName);
                    }
                }
                set = descriptors_->allocate(deepInterfaceLayout_);
                std::array<VkDescriptorImageInfo,
                    kMaximumLayeredInterfaceCount> depths{};
                std::array<VkDescriptorImageInfo,
                    kMaximumLayeredInterfaceCount> identities{};
                for (uint32_t index = 0u;
                    index < kMaximumLayeredInterfaceCount; ++index) {
                    const uint32_t source = (std::min)(index,
                        tier.interfaceCount - 1u);
                    depths[index] = { frameTargets.sampler(),
                        tier.interfaceDepth[source].view,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                    identities[index] = { frameTargets.integerSampler(),
                        tier.interfaceIdentity[source].view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                }
                const std::array<VkWriteDescriptorSet, 2> writes{{
                    { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                        0u, 0u, kMaximumLayeredInterfaceCount,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        depths.data(), nullptr, nullptr },
                    { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                        1u, 0u, kMaximumLayeredInterfaceCount,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        identities.data(), nullptr, nullptr },
                }};
                vkUpdateDescriptorSets(device_,
                    static_cast<uint32_t>(writes.size()), writes.data(),
                    0u, nullptr);
                publishedCount = tier.interfaceCount;
            };
            buildDeepSet(target.hero4, frameSets.hero4,
                frameSets.hero4InterfaceCount, "Hero4");
            buildDeepSet(target.cinematic8, frameSets.cinematic8,
                frameSets.cinematic8InterfaceCount, "Cinematic8");
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

VkDescriptorSet VulkanLayeredLocalCompositionPass::descriptorSet(
    uint32_t frameIndex) const {
    if (frameIndex >= descriptorSets_.size())
        throw std::out_of_range(
            "Layered local-composition frame index is invalid");
    return descriptorSets_[frameIndex].ordinary2;
}

VkDescriptorSet VulkanLayeredLocalCompositionPass::deepDescriptorSet(
    uint32_t frameIndex, TransparencyQuality quality) const {
    if (frameIndex >= descriptorSets_.size())
        throw std::out_of_range(
            "Deep local-composition frame index is invalid");
    const FrameDescriptorSets& sets = descriptorSets_[frameIndex];
    if (quality == TransparencyQuality::Hero4) return sets.hero4;
    if (quality == TransparencyQuality::Cinematic8) return sets.cinematic8;
    throw std::invalid_argument(
        "Deep local-composition requires Hero4 or Cinematic8");
}

uint32_t VulkanLayeredLocalCompositionPass::deepDescriptorInterfaceCount(
    uint32_t frameIndex, TransparencyQuality quality) const {
    if (frameIndex >= descriptorSets_.size()) return 0u;
    const FrameDescriptorSets& sets = descriptorSets_[frameIndex];
    if (quality == TransparencyQuality::Hero4)
        return sets.hero4InterfaceCount;
    if (quality == TransparencyQuality::Cinematic8)
        return sets.cinematic8InterfaceCount;
    return 0u;
}

void VulkanLayeredLocalCompositionPass::clearDescriptors() noexcept {
    if (descriptors_ != nullptr) {
        for (const FrameDescriptorSets& sets : descriptorSets_) {
            for (VkDescriptorSet set : {
                    sets.ordinary2, sets.hero4, sets.cinematic8 }) {
                if (set != VK_NULL_HANDLE)
                    try { descriptors_->free(set); } catch (...) {}
            }
        }
    }
    descriptorSets_.clear();
}

void VulkanLayeredLocalCompositionPass::cleanup() noexcept {
    clearDescriptors();
    if (device_ == VK_NULL_HANDLE)
        return;
    if (deepResidualPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, deepResidualPipeline_, nullptr);
    if (deepPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, deepPipeline_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (deepPipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, deepPipelineLayout_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (deepInterfaceLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, deepInterfaceLayout_, nullptr);
    if (interfaceLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, interfaceLayout_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, renderPass_, nullptr);
    device_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    renderPass_ = VK_NULL_HANDLE;
    interfaceLayout_ = VK_NULL_HANDLE;
    deepInterfaceLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    deepPipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    deepPipeline_ = VK_NULL_HANDLE;
    deepResidualPipeline_ = VK_NULL_HANDLE;
}

} // namespace Iridium
