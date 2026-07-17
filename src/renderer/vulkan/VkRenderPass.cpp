#include "VkRenderPass.h"
#include <stdexcept>
#include <array>

VkRenderPassWrapper::VkRenderPassWrapper(VkContext* context, VkSwapchain* swapchain) : context(context) {
	createRenderPass(swapchain->getImageFormat());
}

VkRenderPassWrapper::~VkRenderPassWrapper() {
	vkDestroyRenderPass(context->getDevice(), renderPass, nullptr);
}

void VkRenderPassWrapper::createRenderPass(VkFormat swapChainImageFormat) {
    // =========================================================
    // 0: NORMAL ATTACHMENT (Packed into 32-bit: R=Nx, G=Ny, B=Rough, A=Metal)
    // =========================================================
    VkAttachmentDescription normalAttachment{};
    normalAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    normalAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    normalAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    normalAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    normalAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    normalAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference normalAttachmentRef{};
    normalAttachmentRef.attachment = 0; // <--- SHIFTED DOWN TO 0
    normalAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // =========================================================
    // 1: ALBEDO/COLOR ATTACHMENT (Standard 32-bit: RGB=Color, A=Emissive)
    // =========================================================
    VkAttachmentDescription albedoAttachment{};
    albedoAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    albedoAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    albedoAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    albedoAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    albedoAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    albedoAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference albedoAttachmentRef{};
    albedoAttachmentRef.attachment = 1; // <--- SHIFTED DOWN TO 1
    albedoAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // 2: EMISSIVE ATTACHMENT (RGB may contain HDR values)
    VkAttachmentDescription emissiveAttachment{};
    emissiveAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    emissiveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    emissiveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    emissiveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    emissiveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    emissiveAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference emissiveAttachmentRef{};
    emissiveAttachmentRef.attachment = 2;
    emissiveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // =========================================================
    // 3: DEPTH ATTACHMENT (Used for Depth Testing AND Position Reconstruction)
    // =========================================================
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 3;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // --- SUBPASS SETUP ---
    std::array<VkAttachmentReference, 3> colorAttachments = {
        normalAttachmentRef, albedoAttachmentRef, emissiveAttachmentRef
    };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    subpass.pColorAttachments = colorAttachments.data();
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    // --- SYNCHRONIZATION ---
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    // --- CREATE ---
    std::array<VkAttachmentDescription, 4> attachments = {
        normalAttachment, albedoAttachment, emissiveAttachment, depthAttachment
    };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(context->getDevice(), &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create deferred render pass!");
    }
}
