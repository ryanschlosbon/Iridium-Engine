#include "VulkanDirectionalShadowMap.h"

#include "DescriptorAllocator.h"
#include "VulkanUploadContext.h"
#include "VulkanVertexUtils.h"
#include "utils/File.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <span>
#include <stdexcept>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

} // namespace

void VulkanDirectionalShadowMap::init(VkDevice device,
    VulkanResourceAllocator& allocator, VulkanUploadContext& uploads,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout samplerLayout, uint32_t resolution) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE || resolution == 0 ||
        materialLayout == VK_NULL_HANDLE || samplerLayout == VK_NULL_HANDLE)
        throw std::invalid_argument("Invalid directional shadow initialization");
    device_ = device;
    allocator_ = &allocator;
    descriptors_ = &descriptors;
    resolution_ = resolution;

    try {
        image_ = allocator.createImage2D({ resolution, resolution },
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            ProfileMemoryCategory::ShadowDirectional, 1,
            kDirectionalShadowLayerCount, 0,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY);

        for (uint32_t index = 0; index < kDirectionalShadowLayerCount; ++index) {
            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = image_.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = image_.format;
            view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, index, 1 };
            requireSuccess(vkCreateImageView(device_, &view, nullptr,
                &layerViews_[index]), "vkCreateImageView(shadow layer)");
        }

        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.compareEnable = VK_FALSE;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler.minLod = 0.0f;
        sampler.maxLod = 0.0f;
        requireSuccess(vkCreateSampler(device_, &sampler, nullptr, &sampler_),
            "vkCreateSampler(directional shadow)");

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_D32_SFLOAT;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        const VkAttachmentReference depthReference{
            0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthReference;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        VkRenderPassCreateInfo renderPass{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPass.attachmentCount = 1;
        renderPass.pAttachments = &attachment;
        renderPass.subpassCount = 1;
        renderPass.pSubpasses = &subpass;
        renderPass.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPass.pDependencies = dependencies.data();
        requireSuccess(vkCreateRenderPass(device_, &renderPass, nullptr,
            &renderPass_), "vkCreateRenderPass(directional shadow)");

        for (uint32_t index = 0; index < kDirectionalShadowLayerCount; ++index) {
            VkFramebufferCreateInfo framebuffer{
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebuffer.renderPass = renderPass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &layerViews_[index];
            framebuffer.width = resolution;
            framebuffer.height = resolution;
            framebuffer.layers = 1;
            requireSuccess(vkCreateFramebuffer(device_, &framebuffer, nullptr,
                &framebuffers_[index]), "vkCreateFramebuffer(directional shadow)");
        }

        VkDescriptorSetLayoutBinding shadowBinding{};
        shadowBinding.binding = 0;
        shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        shadowBinding.descriptorCount = 1;
        shadowBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo setLayout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = 1;
        setLayout.pBindings = &shadowBinding;
        requireSuccess(vkCreateDescriptorSetLayout(device_, &setLayout, nullptr,
            &renderSetLayout_), "vkCreateDescriptorSetLayout(directional shadow)");

        const std::array<VkDescriptorSetLayout, 3> layouts{
            renderSetLayout_, materialLayout, samplerLayout };
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size = sizeof(CanonicalMeshPushConstants);
        VkPipelineLayoutCreateInfo pipelineLayout{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayout.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pipelineLayout.pSetLayouts = layouts.data();
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        requireSuccess(vkCreatePipelineLayout(device_, &pipelineLayout, nullptr,
            &pipelineLayout_), "vkCreatePipelineLayout(directional shadow)");

        for (uint32_t index = 0; index < pipelines_.size(); ++index)
            pipelines_[index] = createPipeline((index & 1u) != 0,
                (index & 2u) != 0);

        for (uint32_t frame = 0; frame < frameBuffers_.size(); ++frame) {
            frameBuffers_[frame] = allocator.createBuffer(
                sizeof(VulkanDirectionalShadowData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::Uniform);
            renderSets_[frame] = descriptors.allocate(renderSetLayout_);
            const VkDescriptorBufferInfo buffer{
                frameBuffers_[frame].buffer, 0,
                sizeof(VulkanDirectionalShadowData) };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = renderSets_[frame];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }

        uploads.enqueueTransition(image_, ResourceState::ShaderResource);
    } catch (...) {
        cleanup();
        throw;
    }
}

void VulkanDirectionalShadowMap::cleanup() noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    for (VkPipeline& pipeline : pipelines_) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptors_ != nullptr) {
        for (VkDescriptorSet set : renderSets_)
            if (set != VK_NULL_HANDLE) descriptors_->free(set);
    }
    renderSets_ = {};
    if (renderSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, renderSetLayout_, nullptr);
    for (VkFramebuffer& framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
    for (VkImageView& view : layerViews_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (allocator_ != nullptr) {
        for (auto& buffer : frameBuffers_) allocator_->destroy(buffer);
        allocator_->destroy(image_);
    }
    pipelineLayout_ = VK_NULL_HANDLE;
    renderSetLayout_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    allocator_ = nullptr;
    resolution_ = 0;
    device_ = VK_NULL_HANDLE;
}

void VulkanDirectionalShadowMap::updateFrame(uint32_t frameIndex,
    std::span<const DirectionalShadowFramePacket> packets) {
    if (frameIndex >= frameBuffers_.size())
        throw std::out_of_range("Directional shadow frame index is invalid");
    VulkanDirectionalShadowData data{};
    std::array<bool, kDirectionalShadowLightCapacity> occupied{};
    for (const DirectionalShadowFramePacket& packet : packets) {
        if (packet.shadowIndex >= kDirectionalShadowLightCapacity ||
            occupied[packet.shadowIndex])
            throw std::invalid_argument(
                "Directional shadow packet storage index is invalid");
        occupied[packet.shadowIndex] = true;
        const uint32_t firstLayer = packet.shadowIndex *
            kDirectionalShadowCascadeCount;
        for (uint32_t index = 0; index < kDirectionalShadowCascadeCount; ++index)
            data.worldToShadowClip[firstLayer + index] =
                packet.plan.cascades[index].worldToShadowClip;
        data.splitFar[packet.shadowIndex] = glm::vec4(packet.plan.splitFar[0],
            packet.plan.splitFar[1], packet.plan.splitFar[2],
            packet.plan.splitFar[3]);
        data.texelWorldSize[packet.shadowIndex] = glm::vec4(
            packet.plan.cascades[0].worldUnitsPerTexel,
            packet.plan.cascades[1].worldUnitsPerTexel,
            packet.plan.cascades[2].worldUnitsPerTexel,
            packet.plan.cascades[3].worldUnitsPerTexel);
        data.depthSpanMeters[packet.shadowIndex] = glm::vec4(
            packet.plan.cascades[0].depthSpanMeters,
            packet.plan.cascades[1].depthSpanMeters,
            packet.plan.cascades[2].depthSpanMeters,
            packet.plan.cascades[3].depthSpanMeters);
        const float angularRadiusRadians = glm::radians(
            packet.sourceAngularDiameterDegrees * 0.5f);
        data.filterParameters[packet.shadowIndex] = glm::vec4(
            std::tan(angularRadiusRadians),
            packet.filterProfile.maximumPenumbraTexels, 0.0f, 0.0f);
        data.filterMetadata[packet.shadowIndex] = glm::uvec4(
            packet.filterProfile.blockerSearchSamples,
            packet.filterProfile.filterSamples,
            packet.filterProfile.contactHardening ? 1u : 0u, 0u);
        data.metadata[packet.shadowIndex] = glm::uvec4(
            packet.selection.lightSlot, packet.sampleableMask, firstLayer,
            packet.sampleableMask != 0u);
    }
    allocator_->write(frameBuffers_[frameIndex], 0,
        std::as_bytes(std::span{ &data, size_t{ 1 } }));
}

void VulkanDirectionalShadowMap::beginCascade(VkCommandBuffer commandBuffer,
    uint32_t shadowIndex, uint32_t cascadeIndex) const {
    if (shadowIndex >= kDirectionalShadowLightCapacity ||
        cascadeIndex >= kDirectionalShadowCascadeCount)
        throw std::out_of_range("Directional shadow cascade index is invalid");
    const uint32_t layer = shadowIndex * kDirectionalShadowCascadeCount +
        cascadeIndex;
    const VkClearValue clear{ .depthStencil = { 1.0f, 0 } };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = renderPass_;
    begin.framebuffer = framebuffers_[layer];
    begin.renderArea.extent = { resolution_, resolution_ };
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{ 0.0f, 0.0f,
        static_cast<float>(resolution_), static_cast<float>(resolution_),
        0.0f, 1.0f };
    const VkRect2D scissor{ { 0, 0 }, { resolution_, resolution_ } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdSetDepthBias(commandBuffer, 0.5f, 0.0f, 1.0f);
}

void VulkanDirectionalShadowMap::endCascade(VkCommandBuffer commandBuffer) const {
    vkCmdEndRenderPass(commandBuffer);
}

VkPipeline VulkanDirectionalShadowMap::pipeline(bool alphaMasked,
    bool doubleSided) const noexcept {
    return pipelines_[(doubleSided ? 2u : 0u) | (alphaMasked ? 1u : 0u)];
}

VkDescriptorSet VulkanDirectionalShadowMap::renderDescriptor(
    uint32_t frameIndex) const {
    if (frameIndex >= renderSets_.size())
        throw std::out_of_range("Directional shadow descriptor frame is invalid");
    return renderSets_[frameIndex];
}

VkDescriptorImageInfo VulkanDirectionalShadowMap::sampleImage() const noexcept {
    return { sampler_, image_.view,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
}

VkDescriptorBufferInfo VulkanDirectionalShadowMap::sampleBuffer(
    uint32_t frameIndex) const noexcept {
    if (frameIndex >= frameBuffers_.size()) return {};
    return { frameBuffers_[frameIndex].buffer, 0,
        sizeof(VulkanDirectionalShadowData) };
}

VkShaderModule VulkanDirectionalShadowMap::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &create, nullptr, &module),
        "vkCreateShaderModule(directional shadow)");
    return module;
}

VkPipeline VulkanDirectionalShadowMap::createPipeline(bool alphaMasked,
    bool doubleSided) {
    VkShaderModule vertex = createShaderModule(
        "assets/shaders/directional_shadow_vert.spv");
    VkShaderModule fragment = VK_NULL_HANDLE;
    try {
        if (alphaMasked)
            fragment = createShaderModule(
                "assets/shaders/directional_shadow_mask_frag.spv");
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr };
        if (alphaMasked)
            stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr };

        const auto binding = VulkanVertexUtils::getBindingDescription();
        const std::array<VkVertexInputAttributeDescription, 4> attributes{{
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) },
            { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color) },
            { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv0) },
            { 5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv1) },
        }};
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        // Match the canonical raster convention. Vulkan's framebuffer-space
        // orientation makes the imported glTF front faces clockwise here.
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.depthBiasEnable = VK_TRUE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        const std::array<VkDynamicState, 3> dynamicValues{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS };
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicValues.size());
        dynamic.pDynamicStates = dynamicValues.data();
        VkGraphicsPipelineCreateInfo create{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        create.stageCount = alphaMasked ? 2u : 1u;
        create.pStages = stages.data();
        create.pVertexInputState = &vertexInput;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pDepthStencilState = &depth;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = pipelineLayout_;
        create.renderPass = renderPass_;
        VkPipeline result = VK_NULL_HANDLE;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
            &create, nullptr, &result),
            "vkCreateGraphicsPipelines(directional shadow)");
        if (fragment != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        return result;
    } catch (...) {
        if (fragment != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
}

} // namespace Iridium
