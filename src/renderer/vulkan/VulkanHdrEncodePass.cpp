#include "renderer/vulkan/VulkanHdrEncodePass.h"

#include "renderer/vulkan/VulkanFrameTargets.h"
#include "utils/File.h"

#include <array>
#include <stdexcept>
#include <string>

namespace Iridium {
    namespace {
        VkShaderModule createModule(VkDevice device, const std::vector<char>& code) {
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = code.size();
            info.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule result = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &result) != VK_SUCCESS)
                throw std::runtime_error("Failed to create HDR encode shader module.");
            return result;
        }
    }

    VulkanHdrEncodePass::~VulkanHdrEncodePass() { cleanup(); }

    void VulkanHdrEncodePass::init(VkContext& context,
        DescriptorAllocator& allocator, VkFormat swapchainFormat) {
        device_ = context.getDevice();
        allocator_ = &allocator;
        VkAttachmentDescription attachment{};
        attachment.format = swapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderInfo.attachmentCount = 1;
        renderInfo.pAttachments = &attachment;
        renderInfo.subpassCount = 1;
        renderInfo.pSubpasses = &subpass;
        renderInfo.dependencyCount = 1;
        renderInfo.pDependencies = &dependency;
        if (vkCreateRenderPass(device_, &renderInfo, nullptr, &renderPass_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create HDR encode render pass.");

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
            &descriptorSetLayout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create HDR encode descriptor layout.");
        VkPushConstantRange push{ VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(float) * 2 };
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
            &pipelineLayout_) != VK_SUCCESS)
            throw std::runtime_error("Failed to create HDR encode pipeline layout.");

        const auto vertexCode = readFile(std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/output_vert.spv");
        const auto fragmentCode = readFile(std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/hdr_encode_frag.spv");
        VkShaderModule vertex = createModule(device_, vertexCode);
        VkShaderModule fragment = createModule(device_, fragmentCode);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1; viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo blending{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blending.attachmentCount = 1; blending.pAttachments = &blend;
        constexpr std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2; pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewport;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blending;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_; pipelineInfo.renderPass = renderPass_;
        const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, fragment, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        if (result != VK_SUCCESS)
            throw std::runtime_error("Failed to create HDR encode pipeline.");
    }

    void VulkanHdrEncodePass::rebuild(const VulkanFrameTargets& targets,
        const std::vector<VkImageView>& swapchainViews, VkExtent2D extent) {
        clearTargets();
        for (const auto& target : targets.targets()) {
            VkDescriptorSet set = allocator_->allocate(descriptorSetLayout_);
            descriptorSets_.push_back(set);
            VkDescriptorImageInfo image{ targets.sampler(), target.uiComposition.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        }
        for (VkImageView view : swapchainViews) {
            VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            info.renderPass = renderPass_; info.attachmentCount = 1;
            info.pAttachments = &view; info.width = extent.width;
            info.height = extent.height; info.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS)
                throw std::runtime_error("Failed to create HDR present framebuffer.");
            framebuffers_.push_back(framebuffer);
        }
    }

    void VulkanHdrEncodePass::clearTargets() {
        if (!descriptorSets_.empty() && allocator_) allocator_->free(descriptorSets_);
        descriptorSets_.clear();
        for (VkFramebuffer framebuffer : framebuffers_)
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        framebuffers_.clear();
    }

    void VulkanHdrEncodePass::record(VkCommandBuffer commandBuffer,
        uint32_t frameIndex, uint32_t imageIndex, VkExtent2D extent,
        float paperWhiteNits, float peakNits) const {
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = renderPass_; begin.framebuffer = framebuffers_.at(imageIndex);
        begin.renderArea.extent = extent;
        VkClearValue clear{ { { 0, 0, 0, 1 } } };
        begin.clearValueCount = 1; begin.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkDescriptorSet set = descriptorSets_.at(frameIndex);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout_, 0, 1, &set, 0, nullptr);
        const float push[2] = { paperWhiteNits, peakNits };
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        const VkViewport viewport{ 0, 0, static_cast<float>(extent.width),
            static_cast<float>(extent.height), 0, 1 };
        const VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
    }

    void VulkanHdrEncodePass::cleanup() {
        if (device_ != VK_NULL_HANDLE) {
            clearTargets();
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        }
        descriptorSets_.clear(); framebuffers_.clear(); device_ = VK_NULL_HANDLE;
        allocator_ = nullptr; renderPass_ = VK_NULL_HANDLE;
        descriptorSetLayout_ = VK_NULL_HANDLE; pipelineLayout_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
    }
}
