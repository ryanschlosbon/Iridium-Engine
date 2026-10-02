#include "renderer/vulkan/VulkanLayeredInterfaceCapturePass.h"

#include "renderer/transparency/LayeredGlass.h"
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

void VulkanLayeredInterfaceCapturePass::init(VkDevice device,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout globalLayout,
    VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout samplerLayout) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        globalLayout == VK_NULL_HANDLE || materialLayout == VK_NULL_HANDLE ||
        samplerLayout == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "Invalid layered-interface capture-pass initialization");
    }
    device_ = device;
    descriptors_ = &descriptors;
    try {
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = VK_FORMAT_R32_UINT;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Every interface is fully cleared on its capture. Undefined initial
        // layout allows the render pass to discard either the prior frame's
        // sampled state or a never-written target without contradicting the
        // graph transition history.
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1].format = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        const VkAttachmentReference color{
            0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        const VkAttachmentReference depth{
            1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1u;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0u;
        dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0u;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPassInfo.attachmentCount =
            static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1u;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1u;
        renderPassInfo.pDependencies = &dependency;
        requireSuccess(vkCreateRenderPass(device_, &renderPassInfo, nullptr,
            &renderPass_), "vkCreateRenderPass(layered interface capture)");

        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[binding].descriptorCount = 1u;
            bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo captureLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        captureLayoutInfo.bindingCount =
            static_cast<uint32_t>(bindings.size());
        captureLayoutInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_, &captureLayoutInfo,
            nullptr, &captureLayout_),
            "vkCreateDescriptorSetLayout(layered interface capture)");

        const std::array<VkDescriptorSetLayout, 4> setLayouts{
            globalLayout, materialLayout, samplerLayout, captureLayout_ };
        const VkPushConstantRange pushRange{
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0u, sizeof(LayeredInterfaceCapturePushConstants) };
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount =
            static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        requireSuccess(vkCreatePipelineLayout(device_, &pipelineLayoutInfo,
            nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(layered interface capture)");

        // Prewarm the one cull-free pipeline. Entry/exit and mirrored semantics
        // are push-constant branches, so topology preparation cannot trigger
        // any lazy graphics-pipeline creation.
        pipeline_ = createPipeline();

        const std::array<VkDescriptorSetLayoutBinding, 2>
            terminationBindings{{
                { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            }};
        VkDescriptorSetLayoutCreateInfo terminationLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        terminationLayoutInfo.bindingCount = static_cast<uint32_t>(
            terminationBindings.size());
        terminationLayoutInfo.pBindings = terminationBindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_,
            &terminationLayoutInfo, nullptr, &tileTerminationLayout_),
            "vkCreateDescriptorSetLayout(layered tile termination)");
        VkPipelineLayoutCreateInfo terminationPipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        terminationPipelineLayoutInfo.setLayoutCount = 1u;
        terminationPipelineLayoutInfo.pSetLayouts = &tileTerminationLayout_;
        requireSuccess(vkCreatePipelineLayout(device_,
            &terminationPipelineLayoutInfo, nullptr,
            &tileTerminationPipelineLayout_),
            "vkCreatePipelineLayout(layered tile termination)");
        VkShaderModule terminationShader = createShaderModule(
            "assets/shaders/layered_tile_termination_comp.spv");
        const VkPipelineShaderStageCreateInfo terminationStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u,
            VK_SHADER_STAGE_COMPUTE_BIT, terminationShader, "main", nullptr };
        const VkComputePipelineCreateInfo terminationPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0u,
            terminationStage, tileTerminationPipelineLayout_,
            VK_NULL_HANDLE, -1 };
        const VkResult terminationResult = vkCreateComputePipelines(device_,
            VK_NULL_HANDLE, 1u, &terminationPipelineInfo, nullptr,
            &tileTerminationPipeline_);
        vkDestroyShaderModule(device_, terminationShader, nullptr);
        requireSuccess(terminationResult,
            "vkCreateComputePipelines(layered tile termination)");
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkPipeline VulkanLayeredInterfaceCapturePass::createPipeline() const {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        vertex = createShaderModule("assets/shaders/canonical_material_vert.spv");
        fragment = createShaderModule(
            "assets/shaders/layered_interface_capture_frag.spv");
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
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
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
        pipelineInfo.renderPass = renderPass_;
        VkPipeline pipeline = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1u,
            &pipelineInfo, nullptr, &pipeline),
            "vkCreateGraphicsPipelines(layered interface capture)");
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

VkShaderModule VulkanLayeredInterfaceCapturePass::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &info, nullptr, &module),
        "vkCreateShaderModule(layered interface capture)");
    return module;
}

void VulkanLayeredInterfaceCapturePass::rebuildDescriptors(
    const VulkanFrameTargets& frameTargets) {
    if (device_ == VK_NULL_HANDLE || descriptors_ == nullptr)
        throw std::logic_error(
            "Layered-interface capture pass is not initialized");
    clearDescriptors();
    descriptorSets_.resize(frameTargets.size());
    try {
        for (uint32_t frame = 0; frame < frameTargets.size(); ++frame) {
            const VulkanFrameContextTargets& target = frameTargets.get(frame);
            const VkDescriptorImageInfo opaque{
                frameTargets.sampler(), target.depth.view,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            const auto buildTier = [&](auto& sets, auto* terminationSets,
                    uint32_t& publishedCount, uint32_t interfaceCount,
                    auto depthAt, auto identityAt, auto tileAt,
                    const char* tierName) {
                if (interfaceCount == 0u) return;
                if (interfaceCount > sets.size())
                    throw std::logic_error(std::string(
                        "Layered-interface descriptor capacity exceeded for ") +
                        tierName);
                for (uint32_t interfaceIndex = 0u;
                    interfaceIndex < interfaceCount; ++interfaceIndex) {
                    if (!depthAt(interfaceIndex).isValid() ||
                        !identityAt(interfaceIndex).isValid() ||
                        (terminationSets != nullptr &&
                            deepLayeredTerminationInterface(interfaceIndex,
                                interfaceCount) &&
                            !tileAt(interfaceIndex).isValid())) {
                        throw std::logic_error(std::string(
                            "Incomplete layered-interface targets for ") +
                            tierName);
                    }
                }
                for (uint32_t interfaceIndex = 0u;
                    interfaceIndex < interfaceCount; ++interfaceIndex) {
                    sets[interfaceIndex] =
                        descriptors_->allocate(captureLayout_);
                    const VulkanImageResource& previousDepth =
                        interfaceIndex == 0u ? target.depth :
                            depthAt(interfaceIndex - 1u);
                    const VulkanImageResource& previousIdentity =
                        interfaceIndex == 0u ? target.materialFlags :
                            identityAt(interfaceIndex - 1u);
                    // The shader does not fetch the termination binding for
                    // interfaces 0/1, but Vulkan still requires every
                    // statically reachable sampled descriptor to advertise
                    // the image's current layout. Reusing materialFlags for
                    // interface 1 is unsafe when a full-scene Cinematic8
                    // target aliases that retired graph slot with the current
                    // identity attachment. Its already-sampled prior identity
                    // is a format-compatible, layout-correct placeholder.
                    const VulkanImageResource& previousTile =
                        interfaceIndex < 2u ? previousIdentity :
                            tileAt(deepLayeredPriorTerminationInterface(
                                interfaceIndex));
                    const VkDescriptorImageInfo priorDepth{
                        frameTargets.sampler(), previousDepth.view,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                    const VkDescriptorImageInfo priorIdentity{
                        frameTargets.integerSampler(), previousIdentity.view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    const VkDescriptorImageInfo priorTile{
                        frameTargets.integerSampler(), previousTile.view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    std::array<VkWriteDescriptorSet, 4> writes{};
                    const std::array<const VkDescriptorImageInfo*, 4> images{
                        &opaque, &priorDepth, &priorIdentity, &priorTile };
                    for (uint32_t binding = 0u; binding < writes.size();
                        ++binding) {
                        writes[binding] = {
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                            sets[interfaceIndex], binding, 0u, 1u,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            images[binding], nullptr, nullptr };
                    }
                    vkUpdateDescriptorSets(device_,
                        static_cast<uint32_t>(writes.size()), writes.data(),
                        0u, nullptr);
                    if (terminationSets != nullptr &&
                        deepLayeredTerminationInterface(interfaceIndex,
                            interfaceCount)) {
                        (*terminationSets)[interfaceIndex] =
                            descriptors_->allocate(tileTerminationLayout_);
                        const VkDescriptorImageInfo identity{
                            frameTargets.integerSampler(),
                            identityAt(interfaceIndex).view,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                        const VkDescriptorImageInfo tile{
                            VK_NULL_HANDLE, tileAt(interfaceIndex).view,
                            VK_IMAGE_LAYOUT_GENERAL };
                        const std::array<VkDescriptorImageInfo, 2>
                            terminationImages{ identity, tile };
                        std::array<VkWriteDescriptorSet, 2>
                            terminationWrites{};
                        for (uint32_t binding = 0u;
                            binding < terminationWrites.size(); ++binding) {
                            terminationWrites[binding] = {
                                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                nullptr,
                                (*terminationSets)[interfaceIndex], binding,
                                0u, 1u,
                                binding == 0u
                                    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                &terminationImages[binding], nullptr,
                                nullptr };
                        }
                        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(
                            terminationWrites.size()),
                            terminationWrites.data(), 0u, nullptr);
                    }
                }
                publishedCount = interfaceCount;
            };

            FrameDescriptorSets& descriptors = descriptorSets_[frame];
            const bool anyOrdinary2 = target.layeredEntryDepth.isValid() ||
                target.layeredEntryIdentity.isValid() ||
                target.layeredExitDepth.isValid() ||
                target.layeredExitIdentity.isValid();
            buildTier(descriptors.ordinary2,
                static_cast<std::array<VkDescriptorSet, 2>*>(nullptr),
                descriptors.ordinary2Count,
                anyOrdinary2 ? 2u : 0u,
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return interfaceIndex == 0u
                        ? target.layeredEntryDepth
                        : target.layeredExitDepth;
                },
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return interfaceIndex == 0u
                        ? target.layeredEntryIdentity
                        : target.layeredExitIdentity;
                },
                [&](uint32_t) -> const VulkanImageResource& {
                    return target.materialFlags;
                }, "Ordinary2");
            buildTier(descriptors.hero4, &descriptors.hero4Termination,
                descriptors.hero4Count,
                target.hero4.interfaceCount,
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.hero4.interfaceDepth[interfaceIndex];
                },
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.hero4.interfaceIdentity[interfaceIndex];
                },
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.hero4.tileTermination[interfaceIndex];
                }, "Hero4");
            buildTier(descriptors.cinematic8,
                &descriptors.cinematic8Termination,
                descriptors.cinematic8Count,
                target.cinematic8.interfaceCount,
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.cinematic8.interfaceDepth[interfaceIndex];
                },
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.cinematic8.interfaceIdentity[interfaceIndex];
                },
                [&](uint32_t interfaceIndex) -> const VulkanImageResource& {
                    return target.cinematic8.tileTermination[interfaceIndex];
                }, "Cinematic8");
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

VkDescriptorSet VulkanLayeredInterfaceCapturePass::descriptorSet(
    uint32_t frameIndex, bool exitCapture) const {
    return descriptorSet(frameIndex, TransparencyQuality::Ordinary2,
        exitCapture ? 1u : 0u);
}

VkDescriptorSet VulkanLayeredInterfaceCapturePass::descriptorSet(
    uint32_t frameIndex, TransparencyQuality quality,
    uint32_t interfaceIndex) const {
    if (frameIndex >= descriptorSets_.size())
        throw std::out_of_range(
            "Layered-interface capture frame index is invalid");
    const FrameDescriptorSets& frame = descriptorSets_[frameIndex];
    switch (quality) {
    case TransparencyQuality::Ordinary2:
        if (interfaceIndex < frame.ordinary2Count)
            return frame.ordinary2[interfaceIndex];
        break;
    case TransparencyQuality::Hero4:
        if (interfaceIndex < frame.hero4Count)
            return frame.hero4[interfaceIndex];
        break;
    case TransparencyQuality::Cinematic8:
        if (interfaceIndex < frame.cinematic8Count)
            return frame.cinematic8[interfaceIndex];
        break;
    }
    throw std::out_of_range(
        "Layered-interface descriptor index is not resident");
}

uint32_t VulkanLayeredInterfaceCapturePass::descriptorInterfaceCount(
    uint32_t frameIndex, TransparencyQuality quality) const {
    if (frameIndex >= descriptorSets_.size())
        throw std::out_of_range(
            "Layered-interface capture frame index is invalid");
    const FrameDescriptorSets& frame = descriptorSets_[frameIndex];
    switch (quality) {
    case TransparencyQuality::Ordinary2: return frame.ordinary2Count;
    case TransparencyQuality::Hero4: return frame.hero4Count;
    case TransparencyQuality::Cinematic8: return frame.cinematic8Count;
    }
    return 0u;
}

void VulkanLayeredInterfaceCapturePass::recordTileTermination(
    VkCommandBuffer commandBuffer, uint32_t frameIndex,
    TransparencyQuality quality, uint32_t interfaceIndex,
    VkExtent2D atlasExtent) const {
    if (commandBuffer == VK_NULL_HANDLE ||
        tileTerminationPipeline_ == VK_NULL_HANDLE ||
        frameIndex >= descriptorSets_.size() || atlasExtent.width == 0u ||
        atlasExtent.height == 0u) {
        throw std::invalid_argument(
            "Layered tile-termination record request is invalid");
    }
    const FrameDescriptorSets& frame = descriptorSets_[frameIndex];
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (quality == TransparencyQuality::Hero4 &&
        interfaceIndex < frame.hero4Count) {
        set = frame.hero4Termination[interfaceIndex];
    }
    else if (quality == TransparencyQuality::Cinematic8 &&
        interfaceIndex < frame.cinematic8Count) {
        set = frame.cinematic8Termination[interfaceIndex];
    }
    if (set == VK_NULL_HANDLE)
        throw std::out_of_range(
            "Layered tile-termination descriptor is unavailable");
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        tileTerminationPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        tileTerminationPipelineLayout_, 0u, 1u, &set, 0u, nullptr);
    vkCmdDispatch(commandBuffer,
        (atlasExtent.width + kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize,
        (atlasExtent.height + kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize,
        1u);
}

void VulkanLayeredInterfaceCapturePass::clearDescriptors() noexcept {
    if (descriptors_ != nullptr) {
        const auto freeSets = [&](const auto& sets) {
            for (VkDescriptorSet set : sets)
                if (set != VK_NULL_HANDLE)
                    try { descriptors_->free(set); } catch (...) {}
        };
        for (const FrameDescriptorSets& frame : descriptorSets_) {
            freeSets(frame.ordinary2);
            freeSets(frame.hero4);
            freeSets(frame.cinematic8);
            freeSets(frame.hero4Termination);
            freeSets(frame.cinematic8Termination);
        }
    }
    descriptorSets_.clear();
}

void VulkanLayeredInterfaceCapturePass::cleanup() noexcept {
    clearDescriptors();
    if (device_ == VK_NULL_HANDLE)
        return;
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (tileTerminationPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, tileTerminationPipeline_, nullptr);
    if (tileTerminationPipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, tileTerminationPipelineLayout_,
            nullptr);
    if (tileTerminationLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, tileTerminationLayout_,
            nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (captureLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, captureLayout_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, renderPass_, nullptr);
    device_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    renderPass_ = VK_NULL_HANDLE;
    captureLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    tileTerminationPipeline_ = VK_NULL_HANDLE;
    tileTerminationPipelineLayout_ = VK_NULL_HANDLE;
    tileTerminationLayout_ = VK_NULL_HANDLE;
}

} // namespace Iridium
