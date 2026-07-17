#include "VkGraphicsPipeline.h"
#include "renderer/rhi/Mesh.h"
#include "VulkanVertexUtils.h"
#include <stdexcept>
#include <iostream>
#include <array>

VkGraphicsPipeline::VkGraphicsPipeline(VkContext* context, VkSwapchain* swapchain, VkRenderPassWrapper* renderPass,
    VkPipelineLayout pipelineLayout)
	: context(context), pipelineLayout(pipelineLayout) {
    
    wireframePipeline = createPipeline(swapchain, renderPass, true, false); // wireframe
    outlinePipeline = createPipeline(swapchain, renderPass, false, true); // outline
}

VkGraphicsPipeline::~VkGraphicsPipeline() {
    vkDestroyPipeline(context->getDevice(), wireframePipeline, nullptr);
    vkDestroyPipeline(context->getDevice(), outlinePipeline, nullptr);
}

VkShaderModule VkGraphicsPipeline::createShaderModule(const std::vector<char>& code) {
	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = code.size();
	createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
	VkShaderModule shaderModule;
	if (vkCreateShaderModule(context->getDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
		throw std::runtime_error("failed to create shader module!");
	}
	return shaderModule;
}

VkPipeline VkGraphicsPipeline::createPipeline(VkSwapchain* swapchain, VkRenderPassWrapper* renderPass, 
    bool isWireframe, bool isOutline) {
    // -------------------------------------------------------------
    // 1. SHADER LOADING
    // -------------------------------------------------------------
    auto vertCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_vert.spv");
    auto fragCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_frag.spv");

    if (isOutline) {
        // This MUST be your standard 3D mesh vertex shader, not the select shader
        vertCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_vert.spv");
        fragCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/mask_frag.spv");
    }
    else {
        vertCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_vert.spv");
        fragCode = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/shader_frag.spv");
    }

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };

    // -------------------------------------------------------------
    // 2. FIXED FUNCTION STATES
    // -------------------------------------------------------------

    // Vertex Input
    auto bindingDescription = Iridium::VulkanVertexUtils::getBindingDescription();
    auto attributeDescriptions = Iridium::VulkanVertexUtils::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // Input Assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport & Scissor (Dynamic)
    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterizer (The Toggle Logic)
    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;

    rasterizer.lineWidth = 1.0f;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    if (isOutline) {
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; // Keep this (Flip normals)

        // CHANGED: Check isWireframe to decide between FILL (Solid) or LINE (Cage)
        if (isWireframe) {
            rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
        }
        else {
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        }
    }
    else if (isWireframe) {
        rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    }
    else {
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth Stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = isOutline ? VK_FALSE : VK_TRUE;
    depthStencil.depthWriteEnable = isOutline ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = isOutline ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color Blending - Protect the G-Buffer!
    VkPipelineColorBlendAttachmentState normalBlendAttachment{};
    normalBlendAttachment.blendEnable = VK_FALSE;
    // Mask pass does NOT touch the normal buffer at all.
    normalBlendAttachment.colorWriteMask = isOutline ? 0 :
        (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

    VkPipelineColorBlendAttachmentState albedoBlendAttachment{};
    albedoBlendAttachment.blendEnable = VK_FALSE;
    // Mask pass ONLY writes to the Alpha channel. Leaves RGB (car color) perfectly intact!
    albedoBlendAttachment.colorWriteMask = isOutline ? VK_COLOR_COMPONENT_A_BIT :
        (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

    VkPipelineColorBlendAttachmentState emissiveBlendAttachment{};
    emissiveBlendAttachment.blendEnable = VK_FALSE;
    emissiveBlendAttachment.colorWriteMask = isOutline ? 0 :
        (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

    std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments = {
        normalBlendAttachment, albedoBlendAttachment, emissiveBlendAttachment
    };

    VkPipelineColorBlendStateCreateInfo colorBlending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    colorBlending.pAttachments = blendAttachments.data();

    // Dynamic State
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // -------------------------------------------------------------
    // 3. CREATE PIPELINE
    // -------------------------------------------------------------
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

    // The layout is owned by VulkanMeshLayouts and borrowed by this fixed wrapper.
    pipelineInfo.layout = pipelineLayout;

    pipelineInfo.renderPass = renderPass->getRenderPass();
    pipelineInfo.subpass = 0;
    pipelineInfo.pDynamicState = &dynamicState;

    VkPipeline newPipeline;
    if (vkCreateGraphicsPipelines(context->getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &newPipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }

    vkDestroyShaderModule(context->getDevice(), fragModule, nullptr);
    vkDestroyShaderModule(context->getDevice(), vertModule, nullptr);

    return newPipeline;
}
