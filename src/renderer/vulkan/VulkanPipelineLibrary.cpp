#include "VulkanPipelineLibrary.h"

#include "VulkanVertexUtils.h"
#include "utils/File.h"

#include <array>
#include <stdexcept>

namespace Iridium {
    namespace {
        VkPrimitiveTopology toVulkanTopology(PrimitiveTopology topology) {
            switch (topology) {
            case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
            throw std::invalid_argument("unsupported primitive topology");
        }

        VkPolygonMode toVulkanPolygonMode(PolygonMode polygonMode) {
            switch (polygonMode) {
            case PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
            case PolygonMode::Line: return VK_POLYGON_MODE_LINE;
            }
            throw std::invalid_argument("unsupported polygon mode");
        }

        VkCullModeFlags toVulkanCullMode(CullMode cullMode) {
            switch (cullMode) {
            case CullMode::None: return VK_CULL_MODE_NONE;
            case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
            }
            throw std::invalid_argument("unsupported cull mode");
        }

        VkFrontFace toVulkanFrontFace(FrontFace frontFace) {
            switch (frontFace) {
            case FrontFace::Clockwise: return VK_FRONT_FACE_CLOCKWISE;
            case FrontFace::CounterClockwise: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
            }
            throw std::invalid_argument("unsupported front face");
        }

        VkCompareOp toVulkanDepthCompare(DepthCompare depthCompare) {
            switch (depthCompare) {
            case DepthCompare::Less: return VK_COMPARE_OP_LESS;
            case DepthCompare::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case DepthCompare::Greater: return VK_COMPARE_OP_GREATER;
            case DepthCompare::Always: return VK_COMPARE_OP_ALWAYS;
            }
            throw std::invalid_argument("unsupported depth compare");
        }

        VkColorComponentFlags toVulkanColorWriteMask(uint8_t mask) noexcept {
            VkColorComponentFlags result = 0;
            if ((mask & ColorWriteR) != 0) result |= VK_COLOR_COMPONENT_R_BIT;
            if ((mask & ColorWriteG) != 0) result |= VK_COLOR_COMPONENT_G_BIT;
            if ((mask & ColorWriteB) != 0) result |= VK_COLOR_COMPONENT_B_BIT;
            if ((mask & ColorWriteA) != 0) result |= VK_COLOR_COMPONENT_A_BIT;
            return result;
        }

        void configureBlendAttachment(VkPipelineColorBlendAttachmentState& attachment, BlendMode blendMode,
            VkColorComponentFlags colorWriteMask) {
            attachment.colorWriteMask = colorWriteMask;
            switch (blendMode) {
            case BlendMode::Opaque:
                attachment.blendEnable = VK_FALSE;
                return;
            case BlendMode::AlphaBlend:
                attachment.blendEnable = VK_TRUE;
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.colorBlendOp = VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.alphaBlendOp = VK_BLEND_OP_ADD;
                return;
            case BlendMode::Additive:
                attachment.blendEnable = VK_TRUE;
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.colorBlendOp = VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.alphaBlendOp = VK_BLEND_OP_ADD;
                return;
            }
            throw std::invalid_argument("unsupported blend mode");
        }

        void validateProgramPassPair(const PipelineStateDesc& desc) {
            const bool valid = (desc.shaderProgram == ShaderProgram::PbrGBuffer
                    && desc.renderPass == RenderPassClass::GBuffer)
                || (desc.shaderProgram == ShaderProgram::PbrForward
                    && desc.renderPass == RenderPassClass::Forward);
            if (!valid) {
                throw std::invalid_argument("shader program and render pass class must use matching PBR targets");
            }
        }
    }

    void VulkanPipelineLibrary::init(VkDevice device, VulkanPipelineTarget gBufferTarget,
        VulkanPipelineTarget forwardTarget) {
        if (device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanPipelineLibrary is already initialized");
        }
        if (device == VK_NULL_HANDLE
            || gBufferTarget.renderPass == VK_NULL_HANDLE
            || gBufferTarget.pipelineLayout == VK_NULL_HANDLE
            || gBufferTarget.colorAttachmentCount != 3
            || forwardTarget.renderPass == VK_NULL_HANDLE
            || forwardTarget.pipelineLayout == VK_NULL_HANDLE
            || forwardTarget.colorAttachmentCount != 1) {
            throw std::invalid_argument("VulkanPipelineLibrary requires valid targets and device");
        }

        device_ = device;
        gBufferTarget_ = gBufferTarget;
        forwardTarget_ = forwardTarget;
    }

    void VulkanPipelineLibrary::cleanup() noexcept {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }

        for (const auto& [_, handle] : pipelineMap_) {
            if (VulkanPipelineRecord* record = pipelineRecords_.get(handle)) {
                if (record->pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device_, record->pipeline, nullptr);
                    record->pipeline = VK_NULL_HANDLE;
                }
            }
            pipelineRecords_.free(handle);
        }
        pipelineMap_.clear();
        gBufferTarget_ = {};
        forwardTarget_ = {};
        device_ = VK_NULL_HANDLE;
    }

    PipelineHandle VulkanPipelineLibrary::getOrCreatePipeline(const PipelineStateDesc& desc) {
        if (device_ == VK_NULL_HANDLE) {
            throw std::logic_error("VulkanPipelineLibrary must be initialized before creating pipelines");
        }
        validateProgramPassPair(desc);

        if (const auto it = pipelineMap_.find(desc); it != pipelineMap_.end()) {
            return it->second;
        }

        const VulkanPipelineTarget& target = getTarget(desc.renderPass);
        VulkanPipelineRecord record{};
        record.pipeline = createPipeline(desc, target);
        record.pipelineLayout = target.pipelineLayout;
        record.renderPass = desc.renderPass;

        PipelineHandle handle = pipelineRecords_.allocate(record);
        try {
            pipelineMap_.emplace(desc, handle);
        } catch (...) {
            vkDestroyPipeline(device_, record.pipeline, nullptr);
            pipelineRecords_.free(handle);
            throw;
        }
        return handle;
    }

    const VulkanPipelineRecord* VulkanPipelineLibrary::get(PipelineHandle handle) const noexcept {
        return pipelineRecords_.get(handle);
    }

    VkPipeline VulkanPipelineLibrary::createPipeline(const PipelineStateDesc& desc,
        const VulkanPipelineTarget& target) {
        const char* fragmentShaderPath = nullptr;
        switch (desc.shaderProgram) {
        case ShaderProgram::PbrGBuffer:
            fragmentShaderPath = "assets/shaders/shader_frag.spv";
            break;
        case ShaderProgram::PbrForward:
            fragmentShaderPath = "assets/shaders/forward_frag.spv";
            break;
        }

        const std::vector<char> vertexShaderCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_vert.spv");
        const std::vector<char> fragmentShaderCode = readFile(std::string(PROJECT_ROOT_DIR) + fragmentShaderPath);
        VkShaderModule vertexShader = VK_NULL_HANDLE;
        VkShaderModule fragmentShader = VK_NULL_HANDLE;

        try {
            vertexShader = createShaderModule(vertexShaderCode);
            fragmentShader = createShaderModule(fragmentShaderCode);

            const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {{
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", nullptr },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", nullptr },
            }};

            const auto bindingDescription = VulkanVertexUtils::getBindingDescription();
            const auto attributeDescriptions = VulkanVertexUtils::getAttributeDescriptions();
            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &bindingDescription;
            vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
            vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            inputAssembly.topology = toVulkanTopology(desc.topology);
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            const std::array<VkDynamicState, 2> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
            };
            VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = toVulkanPolygonMode(desc.polygonMode);
            rasterizer.cullMode = toVulkanCullMode(desc.cullMode);
            rasterizer.frontFace = toVulkanFrontFace(desc.frontFace);
            rasterizer.depthBiasEnable = VK_FALSE;
            rasterizer.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            multisampling.sampleShadingEnable = VK_FALSE;

            VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
            depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
            depthStencil.depthCompareOp = toVulkanDepthCompare(desc.depthCompare);
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.stencilTestEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState blendAttachment{};
            configureBlendAttachment(blendAttachment, desc.blendMode, toVulkanColorWriteMask(desc.colorWriteMask));
            std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments = {
                blendAttachment, blendAttachment, blendAttachment };
            VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.attachmentCount = target.colorAttachmentCount;
            colorBlending.pAttachments = blendAttachments.data();

            VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
            pipelineInfo.pStages = shaderStages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = target.pipelineLayout;
            pipelineInfo.renderPass = target.renderPass;
            pipelineInfo.subpass = 0;

            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
                throw std::runtime_error("failed to create Vulkan graphics pipeline");
            }

            vkDestroyShaderModule(device_, fragmentShader, nullptr);
            vkDestroyShaderModule(device_, vertexShader, nullptr);
            return pipeline;
        } catch (...) {
            if (fragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, fragmentShader, nullptr);
            }
            if (vertexShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, vertexShader, nullptr);
            }
            throw;
        }
    }

    VkShaderModule VulkanPipelineLibrary::createShaderModule(const std::vector<char>& code) const {
        VkShaderModuleCreateInfo createInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("failed to create Vulkan shader module");
        }
        return shaderModule;
    }

    const VulkanPipelineTarget& VulkanPipelineLibrary::getTarget(RenderPassClass renderPass) const {
        switch (renderPass) {
        case RenderPassClass::GBuffer: return gBufferTarget_;
        case RenderPassClass::Forward: return forwardTarget_;
        }
        throw std::invalid_argument("unsupported render pass class");
    }

} // namespace Iridium
