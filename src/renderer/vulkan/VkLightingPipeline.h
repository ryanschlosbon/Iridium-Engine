#pragma once

#include "VkContext.h"
#include "VkRenderPass.h"
#include "renderer/rhi/GBufferLayout.h"
#include "utils/File.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

// Push constant for the camera position
struct LightingPushConstants {
    glm::vec4 viewPos; // Use vec4 for strict 16-byte Vulkan alignment
    glm::mat4 invView; 
    glm::mat4 invProj;
    glm::ivec4 debugView;
};

static_assert(sizeof(LightingPushConstants) == 160);

class VkLightingPipeline {
public:
    // Notice we don't need the swapchain here, just the render pass it will draw to!
    VkLightingPipeline(VkContext* context, VkRenderPass renderPass,
        Iridium::GBufferLayout gBufferLayout);
    ~VkLightingPipeline();

    VkPipeline getPipeline() const { return pipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }
    VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptorSetLayout; }

private:
    VkContext* context;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout descriptorSetLayout;

    VkShaderModule createShaderModule(const std::vector<char>& code);
    void createDescriptorSetLayout();
    void createPipeline(VkRenderPass renderPass, Iridium::GBufferLayout gBufferLayout);
};
