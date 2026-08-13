#include "renderer/vulkan/VulkanOutputPass.h"

#include "renderer/vulkan/VulkanFrameTargets.h"
#include "utils/File.h"

#include <array>
#include <stdexcept>
#include <string>

namespace Iridium {

    namespace {

        struct OutputPushConstants {
            float manualExposureEv = 0.0f;
            uint32_t outputOperator = 0;
            uint32_t outputTransport = 0;
            float paperWhiteNits = 203.0f;
            float peakNits = 1000.0f;
            uint32_t selectionActive = 0;
        };
        static_assert(sizeof(OutputPushConstants) == 24);

        [[nodiscard]] VkShaderModule createShaderModule(VkDevice device,
            const std::vector<char>& code) {
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = code.size();
            info.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create output shader module.");
            }
            return module;
        }

    } // namespace

    VulkanOutputPass::~VulkanOutputPass() {
        cleanup();
    }

    void VulkanOutputPass::init(VkContext& context,
        ::DescriptorAllocator& allocator, VkFormat outputFormat) {
        if (device_ != VK_NULL_HANDLE || outputFormat == VK_FORMAT_UNDEFINED) {
            throw std::logic_error("Output pass initialized incorrectly.");
        }
        device_ = context.getDevice();
        allocator_ = &allocator;

        try {
            VkAttachmentDescription attachment{};
            attachment.format = outputFormat;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference attachmentRef{ 0,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &attachmentRef;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo renderPassInfo{
                VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &attachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;
            if (vkCreateRenderPass(device_, &renderPassInfo, nullptr,
                &renderPass_) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create output render pass.");
            }

            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            for (uint32_t index = 0; index < bindings.size(); ++index) {
                bindings[index].binding = index;
                bindings[index].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[index].descriptorCount = 1;
                bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo setInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            setInfo.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(device_, &setInfo, nullptr,
                &descriptorSetLayout_) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create output descriptor layout.");
            }

            VkPipelineLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &descriptorSetLayout_;
            const VkPushConstantRange pushConstant{
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OutputPushConstants) };
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &pushConstant;
            if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                &pipelineLayout_) != VK_SUCCESS) {
                throw std::runtime_error("Failed to create output pipeline layout.");
            }

            const auto vertexCode = readFile(std::string(PROJECT_ROOT_DIR) +
                "assets/shaders/output_vert.spv");
            const auto fragmentCode = readFile(std::string(PROJECT_ROOT_DIR) +
                "assets/shaders/output_frag.spv");
            const VkShaderModule vertex = createShaderModule(device_, vertexCode);
            VkShaderModule fragment = VK_NULL_HANDLE;
            try {
                fragment = createShaderModule(device_, fragmentCode);
                VkPipelineShaderStageCreateInfo stages[2]{};
                stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = vertex;
                stages[0].pName = "main";
                stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = fragment;
                stages[1].pName = "main";
                VkPipelineVertexInputStateCreateInfo vertexInput{
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
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
                raster.cullMode = VK_CULL_MODE_NONE;
                raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
                raster.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo multisample{
                    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
                multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                    VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                    VK_COLOR_COMPONENT_A_BIT;
                VkPipelineColorBlendStateCreateInfo blending{
                    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
                blending.attachmentCount = 1;
                blending.pAttachments = &blend;
                constexpr std::array dynamicStates{
                    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
                VkPipelineDynamicStateCreateInfo dynamic{
                    VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
                dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
                dynamic.pDynamicStates = dynamicStates.data();
                VkGraphicsPipelineCreateInfo pipelineInfo{
                    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                pipelineInfo.stageCount = 2;
                pipelineInfo.pStages = stages;
                pipelineInfo.pVertexInputState = &vertexInput;
                pipelineInfo.pInputAssemblyState = &assembly;
                pipelineInfo.pViewportState = &viewport;
                pipelineInfo.pRasterizationState = &raster;
                pipelineInfo.pMultisampleState = &multisample;
                pipelineInfo.pColorBlendState = &blending;
                pipelineInfo.pDynamicState = &dynamic;
                pipelineInfo.layout = pipelineLayout_;
                pipelineInfo.renderPass = renderPass_;
                if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                    &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS) {
                    throw std::runtime_error("Failed to create output pipeline.");
                }
            }
            catch (...) {
                if (fragment != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(device_, fragment, nullptr);
                }
                vkDestroyShaderModule(device_, vertex, nullptr);
                throw;
            }
            vkDestroyShaderModule(device_, fragment, nullptr);
            vkDestroyShaderModule(device_, vertex, nullptr);
        }
        catch (...) {
            cleanup();
            throw;
        }
    }

    void VulkanOutputPass::rebuildDescriptors(
        const VulkanFrameTargets& frameTargets, VkImageView lutView,
        VkSampler lutSampler) {
        clearDescriptors();
        descriptorSets_.reserve(frameTargets.size());
        try {
            for (size_t index = 0; index < frameTargets.size(); ++index) {
                const VkDescriptorSet set = allocator_->allocate(descriptorSetLayout_);
                descriptorSets_.push_back(set);
                const VkDescriptorImageInfo image{ frameTargets.sampler(),
                    frameTargets.get(index).litScene.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo lut{ lutSampler, lutView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo selectionMask{
                    frameTargets.sampler(),
                    frameTargets.get(index).emissive.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                std::array<VkWriteDescriptorSet, 3> writes{};
                writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[0].dstSet = set;
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].pImageInfo = &image;
                writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[1].dstSet = set;
                writes[1].dstBinding = 2;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &selectionMask;
                uint32_t writeCount = 2;
                if (lutView != VK_NULL_HANDLE && lutSampler != VK_NULL_HANDLE) {
                    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[2].dstSet = set;
                    writes[2].dstBinding = 1;
                    writes[2].descriptorCount = 1;
                    writes[2].descriptorType =
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[2].pImageInfo = &lut;
                    writeCount = 3;
                }
                vkUpdateDescriptorSets(device_, writeCount, writes.data(), 0, nullptr);
            }
        }
        catch (...) {
            clearDescriptors();
            throw;
        }
    }

    void VulkanOutputPass::clearDescriptors() {
        if (allocator_ != nullptr && !descriptorSets_.empty()) {
            allocator_->free(std::span<const VkDescriptorSet>(descriptorSets_));
        }
        descriptorSets_.clear();
    }

    void VulkanOutputPass::record(VkCommandBuffer commandBuffer, uint32_t frameIndex,
        VkFramebuffer framebuffer, VkExtent2D extent,
        float manualExposureEv, uint32_t outputOperator,
        uint32_t outputTransport, float paperWhiteNits,
        float peakNits, bool selectionActive) const {
        if (frameIndex >= descriptorSets_.size()) {
            throw std::out_of_range("Output descriptor frame index is out of range.");
        }
        VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        begin.renderPass = renderPass_;
        begin.framebuffer = framebuffer;
        begin.renderArea.extent = extent;
        const VkClearValue clear{ { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout_, 0, 1, &descriptorSets_[frameIndex], 0, nullptr);
        const OutputPushConstants push{ manualExposureEv, outputOperator,
            outputTransport, paperWhiteNits, peakNits,
            selectionActive ? 1u : 0u };
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        const VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(extent.width),
            static_cast<float>(extent.height), 0.0f, 1.0f };
        const VkRect2D scissor{ { 0, 0 }, extent };
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
    }

    void VulkanOutputPass::cleanup() {
        clearDescriptors();
        if (device_ != VK_NULL_HANDLE) {
            if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipelineLayout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            }
            if (descriptorSetLayout_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            }
            if (renderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, renderPass_, nullptr);
            }
        }
        pipeline_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        descriptorSetLayout_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        allocator_ = nullptr;
        device_ = VK_NULL_HANDLE;
    }

} // namespace Iridium
