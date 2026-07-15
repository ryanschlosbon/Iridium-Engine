#include "PipelineCache.h"
#include "VulkanVertexUtils.h" // For your getBindingDescription / getAttributeDescriptions
#include <iostream>
#include <fstream>

// A simple hash combination helper
inline void hashCombine(uint64_t& seed, uint64_t hash) {
    seed ^= hash + 0x9e3779b97f4a7c15llu + (seed << 6) + (seed >> 2);
}

namespace Iridium {

    void PipelineCache::init(VkDevice dev, VkRenderPass gBufferPass, VkRenderPass fwdPass, VkPipelineLayout layout) {
        device = dev;
        gBufferRenderPass = gBufferPass;
        forwardRenderPass = fwdPass;
        pipelineLayout = layout;
    }

    void PipelineCache::cleanup() {
        for (auto& pair : pipelineMap) {
            vkDestroyPipeline(device, pair.second, nullptr);
        }
        pipelineMap.clear();
    }

    uint64_t PipelineCache::generateHash(const PipelineStateDesc& desc) {
        uint64_t hash = 0;
        hashCombine(hash, std::hash<std::string>{}(desc.vertexShaderPath));
        hashCombine(hash, std::hash<std::string>{}(desc.fragmentShaderPath));
        hashCombine(hash, static_cast<uint64_t>(desc.blendMode));
        hashCombine(hash, static_cast<uint64_t>(desc.depthCompare));
        hashCombine(hash, desc.depthWrite ? 1 : 0);
        hashCombine(hash, desc.cullBackFace ? 1 : 0);
        return hash;
    }

    VkPipeline PipelineCache::getOrCreatePipeline(const PipelineStateDesc& desc) {
        uint64_t hash = generateHash(desc);

        if (pipelineMap.find(hash) == pipelineMap.end()) {
            if (pipelineMap.size() > 20) {
                std::cerr << "[WARNING] High Pipeline Count: " << pipelineMap.size() << ". Check for state explosion!\n";
            }
            pipelineMap[hash] = createVulkanPipeline(desc);
        }

        return pipelineMap[hash];
    }

    VkPipeline PipelineCache::createVulkanPipeline(const PipelineStateDesc& desc) {
        // 1. Shaders
        auto vertCode = readFile(std::string(PROJECT_ROOT_DIR) + desc.vertexShaderPath);
        auto fragCode = readFile(std::string(PROJECT_ROOT_DIR) + desc.fragmentShaderPath);

        VkShaderModule vertShaderModule = createShaderModule(vertCode);
        VkShaderModule fragShaderModule = createShaderModule(fragCode);

        VkPipelineShaderStageCreateInfo vertShaderStageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

        // 2. Vertex Input
        auto bindingDescription = VulkanVertexUtils::getBindingDescription();
        auto attributeDescriptions = VulkanVertexUtils::getAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Dynamic Viewport
        std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // 3. Rasterizer (Mapped from MaterialDesc!)
        VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = desc.cullBackFace ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // 4. Depth Stencil (Mapped from MaterialDesc!)
        VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;

        switch (desc.depthCompare) {
        case DepthCompare::Less: depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; break;
        case DepthCompare::LessOrEqual: depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; break;
        case DepthCompare::Greater: depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER; break;
        case DepthCompare::Always: depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS; break;
        }

        // 5. Color Blending (Mapped from MaterialDesc!)
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        if (desc.blendMode == BlendMode::AlphaBlend) {
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        else {
            colorBlendAttachment.blendEnable = VK_FALSE;
        }

        // Opaque goes to G-Buffer (2 attachments). Transparent goes to lit scene (1 attachment).
        bool isGBuffer = (desc.blendMode == BlendMode::Opaque);
        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
        if (isGBuffer) {
            blendAttachments = { colorBlendAttachment, colorBlendAttachment }; // Normal & Albedo
        }
        else {
            blendAttachments = { colorBlendAttachment }; // Lit Scene Out
        }

        VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
        colorBlending.pAttachments = blendAttachments.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = isGBuffer ? gBufferRenderPass : forwardRenderPass;
        pipelineInfo.subpass = 0;

        VkPipeline graphicsPipeline;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create dynamic graphics pipeline!");
        }

        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);

        return graphicsPipeline;
    }

    VkShaderModule PipelineCache::createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shaderModule;
        vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
        return shaderModule;
    }

    std::vector<char> PipelineCache::readFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("failed to open file: " + filename);
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    }
}