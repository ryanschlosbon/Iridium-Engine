#include "VulkanSpotShadowAtlas.h"

#include "DescriptorAllocator.h"
#include "VulkanUploadContext.h"
#include "VulkanVertexUtils.h"
#include "utils/File.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

} // namespace

void VulkanSpotShadowAtlas::init(VkDevice device,
    VulkanResourceAllocator& allocator, VulkanUploadContext& uploads,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout samplerLayout, uint32_t resolution) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE || resolution == 0 ||
        materialLayout == VK_NULL_HANDLE || samplerLayout == VK_NULL_HANDLE)
        throw std::invalid_argument("Invalid spot shadow atlas initialization");
    device_ = device;
    allocator_ = &allocator;
    descriptors_ = &descriptors;
    resolution_ = resolution;
    try {
        image_ = allocator.createImage2D({ resolution, resolution },
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT, ProfileMemoryCategory::ShadowLocal);

        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.compareEnable = VK_FALSE;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        requireSuccess(vkCreateSampler(device_, &sampler, nullptr, &sampler_),
            "vkCreateSampler(spot shadow)");

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_D32_SFLOAT;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
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
        dependencies[0].dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        VkRenderPassCreateInfo renderPass{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPass.attachmentCount = 1;
        renderPass.pAttachments = &attachment;
        renderPass.subpassCount = 1;
        renderPass.pSubpasses = &subpass;
        renderPass.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPass.pDependencies = dependencies.data();
        requireSuccess(vkCreateRenderPass(device_, &renderPass, nullptr,
            &renderPass_), "vkCreateRenderPass(spot shadow)");

        VkFramebufferCreateInfo framebuffer{
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer.renderPass = renderPass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &image_.view;
        framebuffer.width = resolution;
        framebuffer.height = resolution;
        framebuffer.layers = 1;
        requireSuccess(vkCreateFramebuffer(device_, &framebuffer, nullptr,
            &framebuffer_), "vkCreateFramebuffer(spot shadow)");

        const VkDescriptorSetLayoutBinding shadowBinding{ 0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo setLayout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = 1;
        setLayout.pBindings = &shadowBinding;
        requireSuccess(vkCreateDescriptorSetLayout(device_, &setLayout, nullptr,
            &renderSetLayout_), "vkCreateDescriptorSetLayout(spot shadow)");

        const std::array<VkDescriptorSetLayout, 3> layouts{
            renderSetLayout_, materialLayout, samplerLayout };
        const VkPushConstantRange push{
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(CanonicalMeshPushConstants) };
        VkPipelineLayoutCreateInfo pipelineLayout{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayout.setLayoutCount = static_cast<uint32_t>(layouts.size());
        pipelineLayout.pSetLayouts = layouts.data();
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        requireSuccess(vkCreatePipelineLayout(device_, &pipelineLayout, nullptr,
            &pipelineLayout_), "vkCreatePipelineLayout(spot shadow)");
        for (uint32_t index = 0; index < pipelines_.size(); ++index)
            pipelines_[index] = createPipeline((index & 1u) != 0,
                (index & 2u) != 0);

        for (uint32_t frame = 0; frame < frameBuffers_.size(); ++frame) {
            frameBuffers_[frame] = allocator.createBuffer(
                sizeof(VulkanSpotShadowData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::Uniform);
            renderSets_[frame] = descriptors.allocate(renderSetLayout_);
            const VkDescriptorBufferInfo buffer{ frameBuffers_[frame].buffer,
                0, sizeof(VulkanSpotShadowData) };
            VkWriteDescriptorSet write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
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

void VulkanSpotShadowAtlas::cleanup() noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    for (VkPipeline& pipeline : pipelines_) {
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptors_ != nullptr)
        for (VkDescriptorSet set : renderSets_)
            if (set != VK_NULL_HANDLE) descriptors_->free(set);
    if (renderSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, renderSetLayout_, nullptr);
    if (framebuffer_ != VK_NULL_HANDLE)
        vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, sampler_, nullptr);
    if (allocator_ != nullptr) {
        for (VulkanBufferResource& buffer : frameBuffers_)
            allocator_->destroy(buffer);
        allocator_->destroy(image_);
    }
    frameBuffers_ = {};
    renderSets_ = {};
    image_ = {};
    sampler_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    framebuffer_ = VK_NULL_HANDLE;
    renderSetLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    allocator_ = nullptr;
    resolution_ = 0;
    device_ = VK_NULL_HANDLE;
}

void VulkanSpotShadowAtlas::updateFrame(uint32_t frameIndex,
    std::span<const SpotShadowFramePacket> packets) {
    if (frameIndex >= frameBuffers_.size() ||
        packets.size() > kSpotShadowEntryCapacity)
        throw std::out_of_range("Spot shadow frame data is invalid");
    VulkanSpotShadowData data{};
    std::array<bool, kSpotShadowEntryCapacity> occupied{};
    uint32_t activeCount = 0;
    for (const SpotShadowFramePacket& packet : packets) {
        if (packet.shadowDataSlot >= kSpotShadowEntryCapacity ||
            occupied[packet.shadowDataSlot] || packet.tileSize == 0 ||
            packet.guardTexels * 2u >= packet.tileSize ||
            packet.atlasX + packet.tileSize > resolution_ ||
            packet.atlasY + packet.tileSize > resolution_)
            throw std::invalid_argument("Spot shadow packet is outside the atlas");
        occupied[packet.shadowDataSlot] = true;
        VulkanSpotShadowEntry& entry = data.entries[packet.shadowDataSlot];
        entry.worldToShadowClip = packet.worldToShadowClip;
        const float inverseAtlas = 1.0f / static_cast<float>(resolution_);
        const float innerSize = static_cast<float>(
            packet.tileSize - packet.guardTexels * 2u);
        entry.atlasScaleBias = {
            innerSize * inverseAtlas, innerSize * inverseAtlas,
            static_cast<float>(packet.atlasX + packet.guardTexels) * inverseAtlas,
            static_cast<float>(packet.atlasY + packet.guardTexels) * inverseAtlas };
        entry.metadata = { packet.lightSlot, packet.sampleable ? 1u : 0u,
            packet.tileSize, packet.staleAgeFrames };
        entry.projectionParameters = { packet.nearPlane, packet.farPlane,
            packet.sourceRadiusMeters, 0.0f };
        entry.filterMetadata = {
            packet.filterProfile.blockerSearchSamples,
            packet.filterProfile.filterSamples,
            packet.filterProfile.contactHardening ? 1u : 0u,
            static_cast<uint32_t>((std::max)(0.0f,
                packet.filterProfile.maximumPenumbraTexels) * 256.0f),
        };
        activeCount += packet.sampleable ? 1u : 0u;
    }
    data.metadata = { resolution_, activeCount, 0u, 0u };
    allocator_->write(frameBuffers_[frameIndex], 0,
        std::as_bytes(std::span{ &data, size_t{ 1 } }));
}

void VulkanSpotShadowAtlas::beginTile(VkCommandBuffer commandBuffer,
    const SpotShadowFramePacket& packet) const {
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = renderPass_;
    begin.framebuffer = framebuffer_;
    begin.renderArea.extent = { resolution_, resolution_ };
    vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkClearAttachment attachment{ VK_IMAGE_ASPECT_DEPTH_BIT, 0,
        { .depthStencil = { 1.0f, 0u } } };
    const VkClearRect clear{ { { static_cast<int32_t>(packet.atlasX),
        static_cast<int32_t>(packet.atlasY) },
        { packet.tileSize, packet.tileSize } }, 0, 1 };
    vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clear);
    const float x = static_cast<float>(packet.atlasX + packet.guardTexels);
    const float y = static_cast<float>(packet.atlasY + packet.guardTexels);
    const uint32_t inner = packet.tileSize - packet.guardTexels * 2u;
    const VkViewport viewport{ x, y, static_cast<float>(inner),
        static_cast<float>(inner), 0.0f, 1.0f };
    const VkRect2D scissor{ { static_cast<int32_t>(x),
        static_cast<int32_t>(y) }, { inner, inner } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdSetDepthBias(commandBuffer, 0.5f, 0.0f, 1.0f);
}

void VulkanSpotShadowAtlas::endTile(VkCommandBuffer commandBuffer) const {
    vkCmdEndRenderPass(commandBuffer);
}

VkPipeline VulkanSpotShadowAtlas::pipeline(bool alphaMasked,
    bool doubleSided) const noexcept {
    return pipelines_[(doubleSided ? 2u : 0u) | (alphaMasked ? 1u : 0u)];
}

VkDescriptorSet VulkanSpotShadowAtlas::renderDescriptor(
    uint32_t frameIndex) const {
    if (frameIndex >= renderSets_.size())
        throw std::out_of_range("Spot shadow descriptor frame is invalid");
    return renderSets_[frameIndex];
}

VkDescriptorImageInfo VulkanSpotShadowAtlas::sampleImage() const noexcept {
    return { sampler_, image_.view,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
}

VkDescriptorBufferInfo VulkanSpotShadowAtlas::sampleBuffer(
    uint32_t frameIndex) const noexcept {
    if (frameIndex >= frameBuffers_.size()) return {};
    return { frameBuffers_[frameIndex].buffer, 0,
        sizeof(VulkanSpotShadowData) };
}

VkShaderModule VulkanSpotShadowAtlas::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &create, nullptr, &module),
        "vkCreateShaderModule(spot shadow)");
    return module;
}

VkPipeline VulkanSpotShadowAtlas::createPipeline(bool alphaMasked,
    bool doubleSided) {
    VkShaderModule vertex = createShaderModule(
        "assets/shaders/spot_shadow_vert.spv");
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
            "vkCreateGraphicsPipelines(spot shadow)");
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
