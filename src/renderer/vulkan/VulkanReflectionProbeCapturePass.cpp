#include "renderer/vulkan/VulkanReflectionProbeCapturePass.h"

#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanVertexUtils.h"
#include "utils/File.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed");
    }

    VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
        return alignment == 0 ? value :
            (value + alignment - 1u) & ~(alignment - 1u);
    }

    struct PrefilterPush {
        uint32_t outputSize = 0;
        uint32_t sampleCount = 0;
        uint32_t mipLevel = 0;
        uint32_t mipCount = 0;
    };

} // namespace

void VulkanReflectionProbeCapturePass::init(VkDevice device,
    VkPhysicalDevice physicalDevice, VulkanResourceAllocator& allocator,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout samplerLayout, VkDescriptorSetLayout sceneLayout) {
    if (device_ != VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE || materialLayout == VK_NULL_HANDLE ||
        samplerLayout == VK_NULL_HANDLE || sceneLayout == VK_NULL_HANDLE)
        throw std::invalid_argument(
            "Invalid reflection-probe capture-pass initialization");
    device_ = device;
    allocator_ = &allocator;
    descriptors_ = &descriptors;
    try {
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[1].format = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference color{ 0,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        const VkAttachmentReference depth{ 1,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dependencies[0].dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo renderPass{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPass.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPass.pAttachments = attachments.data();
        renderPass.subpassCount = 1;
        renderPass.pSubpasses = &subpass;
        renderPass.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPass.pDependencies = dependencies.data();
        requireSuccess(vkCreateRenderPass(device_, &renderPass, nullptr,
            &renderPass_), "vkCreateRenderPass(reflection probe capture)");

        const VkDescriptorSetLayoutBinding captureBinding{ 0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr };
        VkDescriptorSetLayoutCreateInfo captureLayout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        captureLayout.bindingCount = 1;
        captureLayout.pBindings = &captureBinding;
        requireSuccess(vkCreateDescriptorSetLayout(device_, &captureLayout,
            nullptr, &captureLayout_),
            "vkCreateDescriptorSetLayout(reflection probe capture)");
        const std::array layouts{ captureLayout_, materialLayout,
            samplerLayout, sceneLayout };
        const VkPushConstantRange meshPush{
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(CanonicalMeshPushConstants) };
        VkPipelineLayoutCreateInfo graphicsLayout{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        graphicsLayout.setLayoutCount = static_cast<uint32_t>(layouts.size());
        graphicsLayout.pSetLayouts = layouts.data();
        graphicsLayout.pushConstantRangeCount = 1;
        graphicsLayout.pPushConstantRanges = &meshPush;
        requireSuccess(vkCreatePipelineLayout(device_, &graphicsLayout,
            nullptr, &graphicsLayout_),
            "vkCreatePipelineLayout(reflection probe capture)");
        skyPipeline_ = createGraphicsPipeline(true, false, false);
        for (uint32_t index = 0; index < pipelines_.size(); ++index)
            pipelines_[index] = createGraphicsPipeline(false,
                (index & 1u) != 0, (index & 2u) != 0);

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        faceDataStride_ = alignUp(sizeof(VulkanReflectionProbeCaptureFaceData),
            properties.limits.minUniformBufferOffsetAlignment);
        for (uint32_t frame = 0; frame < faceBuffers_.size(); ++frame) {
            faceBuffers_[frame] = allocator.createBuffer(
                faceDataStride_ * MaximumFaceRecords,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::Uniform);
            faceDescriptors_[frame] = descriptors.allocate(captureLayout_);
            const VkDescriptorBufferInfo buffer{ faceBuffers_[frame].buffer,
                0, sizeof(VulkanReflectionProbeCaptureFaceData) };
            VkWriteDescriptorSet write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = faceDescriptors_[frame];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }

        const std::array filterBindings{
            VkDescriptorSetLayoutBinding{ 0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo filterLayout{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        filterLayout.bindingCount = static_cast<uint32_t>(filterBindings.size());
        filterLayout.pBindings = filterBindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_, &filterLayout,
            nullptr, &filterLayout_),
            "vkCreateDescriptorSetLayout(reflection probe prefilter)");
        const VkPushConstantRange filterPush{ VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(PrefilterPush) };
        VkPipelineLayoutCreateInfo filterPipelineLayout{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        filterPipelineLayout.setLayoutCount = 1;
        filterPipelineLayout.pSetLayouts = &filterLayout_;
        filterPipelineLayout.pushConstantRangeCount = 1;
        filterPipelineLayout.pPushConstantRanges = &filterPush;
        requireSuccess(vkCreatePipelineLayout(device_, &filterPipelineLayout,
            nullptr, &filterPipelineLayout_),
            "vkCreatePipelineLayout(reflection probe prefilter)");
        const VkShaderModule filterShader = createShaderModule(
            "assets/shaders/reflection_probe_prefilter_comp.spv");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, filterShader, "main", nullptr };
        const VkComputePipelineCreateInfo filterPipeline{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, filterPipelineLayout_, VK_NULL_HANDLE, -1 };
        const VkResult filterResult = vkCreateComputePipelines(device_,
            VK_NULL_HANDLE, 1, &filterPipeline, nullptr, &filterPipeline_);
        vkDestroyShaderModule(device_, filterShader, nullptr);
        requireSuccess(filterResult,
            "vkCreateComputePipelines(reflection probe prefilter)");

        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.minLod = 0.0f;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        requireSuccess(vkCreateSampler(device_, &sampler, nullptr, &sampler_),
            "vkCreateSampler(reflection probe capture)");
    }
    catch (...) {
        cleanup();
        throw;
    }
}

VkShaderModule VulkanReflectionProbeCapturePass::createShaderModule(
    const char* relativePath) const {
    const std::vector<char> code = readFile(
        std::string(PROJECT_ROOT_DIR) + relativePath);
    VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    create.codeSize = code.size();
    create.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule result = VK_NULL_HANDLE;
    requireSuccess(vkCreateShaderModule(device_, &create, nullptr, &result),
        "vkCreateShaderModule(reflection probe capture)");
    return result;
}

VkPipeline VulkanReflectionProbeCapturePass::createGraphicsPipeline(bool sky,
    bool, bool doubleSided) const {
    const VkShaderModule vertex = createShaderModule(sky
        ? "assets/shaders/reflection_probe_sky_vert.spv"
        : "assets/shaders/reflection_probe_capture_vert.spv");
    const VkShaderModule fragment = createShaderModule(sky
        ? "assets/shaders/reflection_probe_sky_frag.spv"
        : "assets/shaders/reflection_probe_capture_frag.spv");
    VkPipeline result = VK_NULL_HANDLE;
    try {
        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr },
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
                0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr },
        };
        const auto binding = VulkanVertexUtils::getBindingDescription();
        const auto attributes = VulkanVertexUtils::getAttributeDescriptions();
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        if (!sky) {
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount =
                static_cast<uint32_t>(attributes.size());
            vertexInput.pVertexAttributeDescriptions = attributes.data();
        }
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
        const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };
        const VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
            static_cast<uint32_t>(dynamicStates.size()), dynamicStates.data() };
        const VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0,
            1, nullptr, 1, nullptr };
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = sky || doubleSided ? VK_CULL_MODE_NONE :
            VK_CULL_MODE_BACK_BIT;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        const VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr,
            0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE };
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = sky ? VK_FALSE : VK_TRUE;
        depth.depthWriteEnable = sky ? VK_FALSE : VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        const VkPipelineColorBlendAttachmentState blend{ VK_FALSE,
            VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        const VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr,
            0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &blend };
        VkGraphicsPipelineCreateInfo pipeline{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline.stageCount = static_cast<uint32_t>(stages.size());
        pipeline.pStages = stages.data();
        pipeline.pVertexInputState = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &multisample;
        pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &colorBlend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = graphicsLayout_;
        pipeline.renderPass = renderPass_;
        requireSuccess(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
            &pipeline, nullptr, &result),
            "vkCreateGraphicsPipelines(reflection probe capture)");
    }
    catch (...) {
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    return result;
}

VkDeviceSize VulkanReflectionProbeCapturePass::dynamicOffset(
    uint32_t recordIndex) const {
    if (recordIndex >= MaximumFaceRecords)
        throw std::out_of_range("Reflection-probe capture face slot is invalid");
    return faceDataStride_ * recordIndex;
}

void VulkanReflectionProbeCapturePass::writeFace(uint32_t frameIndex,
    uint32_t recordIndex, const ReflectionProbeCaptureFace& face,
    glm::vec3 position, float nearPlane, uint32_t activeLightCount,
    bool captureSky, uint32_t resolution) {
    if (frameIndex >= faceBuffers_.size())
        throw std::out_of_range("Reflection-probe capture frame is invalid");
    VulkanReflectionProbeCaptureFaceData data{
        .worldToClip = face.worldToClip,
        .clipToWorld = glm::inverse(face.worldToClip),
        .capturePositionNear = glm::vec4(position, nearPlane),
        .metadata = { activeLightCount, captureSky ? 1u : 0u,
            resolution, face.faceIndex },
    };
    allocator_->write(faceBuffers_[frameIndex], dynamicOffset(recordIndex),
        std::as_bytes(std::span{ &data, size_t{ 1 } }));
}

void VulkanReflectionProbeCapturePass::beginFace(VkCommandBuffer commandBuffer,
    const VulkanReflectionProbeCaptureStaging& target, uint32_t faceIndex,
    uint32_t frameIndex, uint32_t recordIndex,
    VkDescriptorSet sceneDescriptor) const {
    if (faceIndex >= kReflectionProbeCaptureFaceCount ||
        frameIndex >= faceDescriptors_.size() ||
        target.framebuffers[faceIndex] == VK_NULL_HANDLE)
        throw std::out_of_range("Reflection-probe capture face is invalid");
    const std::array<VkClearValue, 2> clear{
        VkClearValue{ .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } },
        VkClearValue{ .depthStencil = { 1.0f, 0u } },
    };
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = renderPass_;
    begin.framebuffer = target.framebuffers[faceIndex];
    begin.renderArea.extent = { target.resolution, target.resolution };
    begin.clearValueCount = static_cast<uint32_t>(clear.size());
    begin.pClearValues = clear.data();
    vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{ 0.0f, 0.0f,
        static_cast<float>(target.resolution),
        static_cast<float>(target.resolution), 0.0f, 1.0f };
    const VkRect2D scissor{ { 0, 0 },
        { target.resolution, target.resolution } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        skyPipeline_);
    const uint32_t offset = static_cast<uint32_t>(dynamicOffset(recordIndex));
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphicsLayout_, 0, 1, &faceDescriptors_[frameIndex], 1, &offset);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphicsLayout_, 3, 1, &sceneDescriptor, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void VulkanReflectionProbeCapturePass::bindFaceDescriptors(
    VkCommandBuffer commandBuffer, uint32_t frameIndex, uint32_t recordIndex,
    VkDescriptorSet sceneDescriptor) const {
    const uint32_t offset = static_cast<uint32_t>(dynamicOffset(recordIndex));
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphicsLayout_, 0, 1, &faceDescriptors_[frameIndex], 1, &offset);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphicsLayout_, 3, 1, &sceneDescriptor, 0, nullptr);
}

void VulkanReflectionProbeCapturePass::endFace(
    VkCommandBuffer commandBuffer) const {
    vkCmdEndRenderPass(commandBuffer);
}

VkPipeline VulkanReflectionProbeCapturePass::pipeline(bool alphaMasked,
    bool doubleSided) const noexcept {
    return pipelines_[(doubleSided ? 2u : 0u) | (alphaMasked ? 1u : 0u)];
}

std::vector<VkDescriptorSet>
VulkanReflectionProbeCapturePass::recordPrefilter(
    VkCommandBuffer commandBuffer,
    const VulkanReflectionProbeCaptureStaging& target,
    uint32_t sampleCount) {
    if (sampleCount == 0 || target.prefilteredMipArrayViews.size() !=
        target.mipLevels)
        throw std::invalid_argument(
            "Reflection-probe prefilter request is invalid");
    VkImageMemoryBarrier toGeneral{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toGeneral.srcAccessMask = 0;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.image = target.prefilteredRadiance.image;
    toGeneral.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
        target.mipLevels, 0, kReflectionProbeCaptureFaceCount };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &toGeneral);
    std::vector<VkDescriptorSet> sets;
    sets.reserve(target.mipLevels);
    try {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            filterPipeline_);
        for (uint32_t mip = 0; mip < target.mipLevels; ++mip) {
            const VkDescriptorSet set = descriptors_->allocate(filterLayout_);
            sets.push_back(set);
            const VkDescriptorImageInfo source{ sampler_,
                target.rawRadiance.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo output{ VK_NULL_HANDLE,
                target.prefilteredMipArrayViews[mip], VK_IMAGE_LAYOUT_GENERAL };
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                &source, nullptr, nullptr };
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &output, nullptr, nullptr };
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                writes.data(), 0, nullptr);
            const uint32_t size = (std::max)(target.resolution >> mip, 1u);
            const PrefilterPush push{ size, mip == 0 ? 1u : sampleCount,
                mip, target.mipLevels };
            vkCmdBindDescriptorSets(commandBuffer,
                VK_PIPELINE_BIND_POINT_COMPUTE, filterPipelineLayout_, 0, 1,
                &set, 0, nullptr);
            vkCmdPushConstants(commandBuffer, filterPipelineLayout_,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(commandBuffer, (size + 7u) / 8u,
                (size + 7u) / 8u, kReflectionProbeCaptureFaceCount);
        }
    }
    catch (...) {
        releaseDescriptors(sets);
        throw;
    }
    VkImageMemoryBarrier toSample{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toSample.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.image = target.prefilteredRadiance.image;
    toSample.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
        target.mipLevels, 0, kReflectionProbeCaptureFaceCount };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        1, &toSample);
    return sets;
}

VulkanReflectionProbeCaptureReadback
VulkanReflectionProbeCapturePass::recordReadback(
    VkCommandBuffer commandBuffer,
    const VulkanReflectionProbeCaptureStaging& target) {
    const ReflectionProbeCaptureStorageFootprint footprint =
        reflectionProbeCaptureStorageFootprint(target.resolution);
    VulkanReflectionProbeCaptureReadback result{
        .radianceBytes = footprint.rawRadianceBytes,
        .prefilteredBytes = footprint.prefilteredRadianceBytes,
    };
    result.buffer = allocator_->createBuffer(
        result.radianceBytes + result.prefilteredBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, ProfileMemoryCategory::CaptureReadback);
    try {
        std::array<VkImageMemoryBarrier, 2> toCopy{};
        for (VkImageMemoryBarrier& barrier : toCopy) {
            barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount =
                kReflectionProbeCaptureFaceCount;
        }
        toCopy[0].image = target.rawRadiance.image;
        toCopy[0].subresourceRange.baseMipLevel = 0;
        toCopy[0].subresourceRange.levelCount = 1;
        toCopy[1].image = target.prefilteredRadiance.image;
        toCopy[1].subresourceRange.baseMipLevel = 0;
        toCopy[1].subresourceRange.levelCount = target.mipLevels;
        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(toCopy.size()), toCopy.data());

        const VkBufferImageCopy radianceRegion{
            0, 0, 0,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                kReflectionProbeCaptureFaceCount },
            { 0, 0, 0 },
            { target.resolution, target.resolution, 1 },
        };
        vkCmdCopyImageToBuffer(commandBuffer, target.rawRadiance.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, result.buffer.buffer,
            1, &radianceRegion);
        std::vector<VkBufferImageCopy> prefilteredRegions;
        prefilteredRegions.reserve(static_cast<size_t>(target.mipLevels) *
            kReflectionProbeCaptureFaceCount);
        VkDeviceSize offset = result.radianceBytes;
        // Environment products and the generic upload path use a frozen
        // layer-major ABI: a full largest-to-smallest mip chain for +X, then
        // -X, +Y, -Y, +Z, and -Z. Read back directly in that order so a baked
        // capture can be reimported without a format-specific transpose.
        for (uint32_t face = 0;
            face < kReflectionProbeCaptureFaceCount; ++face)
            for (uint32_t mip = 0; mip < target.mipLevels; ++mip) {
                const uint32_t size =
                    (std::max)(target.resolution >> mip, 1u);
                prefilteredRegions.push_back({
                    offset, 0, 0,
                    { VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1 },
                    { 0, 0, 0 }, { size, size, 1 },
                });
                offset += static_cast<VkDeviceSize>(size) * size *
                    sizeof(uint16_t) * 4u;
            }
        if (offset != result.radianceBytes + result.prefilteredBytes)
            throw std::logic_error(
                "Reflection-probe readback layout does not match its product");
        vkCmdCopyImageToBuffer(commandBuffer,
            target.prefilteredRadiance.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, result.buffer.buffer,
            static_cast<uint32_t>(prefilteredRegions.size()),
            prefilteredRegions.data());

        VkImageMemoryBarrier toSample{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toSample.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSample.image = target.prefilteredRadiance.image;
        toSample.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
            target.mipLevels, 0, kReflectionProbeCaptureFaceCount };
        VkBufferMemoryBarrier toHost{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = result.buffer.buffer;
        toHost.offset = 0;
        toHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &toHost, 1, &toSample);
        return result;
    }
    catch (...) {
        allocator_->destroy(result.buffer);
        throw;
    }
}

void VulkanReflectionProbeCapturePass::releaseDescriptors(
    std::span<const VkDescriptorSet> descriptors) noexcept {
    if (descriptors_ == nullptr) return;
    try { descriptors_->free(descriptors); }
    catch (...) {}
}

void VulkanReflectionProbeCapturePass::cleanup() noexcept {
    if (device_ == VK_NULL_HANDLE) return;
    if (descriptors_ != nullptr)
        for (VkDescriptorSet set : faceDescriptors_)
            try { descriptors_->free(set); } catch (...) {}
    if (allocator_ != nullptr)
        for (VulkanBufferResource& buffer : faceBuffers_)
            allocator_->destroy(buffer);
    if (sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, sampler_, nullptr);
    if (filterPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, filterPipeline_, nullptr);
    if (filterPipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, filterPipelineLayout_, nullptr);
    if (filterLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, filterLayout_, nullptr);
    for (VkPipeline pipeline : pipelines_)
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, pipeline, nullptr);
    if (skyPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, skyPipeline_, nullptr);
    if (graphicsLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, graphicsLayout_, nullptr);
    if (captureLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, captureLayout_, nullptr);
    if (renderPass_ != VK_NULL_HANDLE)
        vkDestroyRenderPass(device_, renderPass_, nullptr);
    *this = {};
}

} // namespace Iridium
