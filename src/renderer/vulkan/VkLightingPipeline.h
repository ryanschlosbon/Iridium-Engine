#pragma once

#include "VkContext.h"
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
    // M7R R4a: dynamic rendering into one colour attachment of `colorFormat`.
    VkLightingPipeline(VkContext* context, VkPipelineCache pipelineCache,
        VkFormat colorFormat, Iridium::GBufferLayout gBufferLayout);
    ~VkLightingPipeline();

    VkPipeline getPipeline() const { return pipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }
    VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptorSetLayout; }

private:
    VkContext* context;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout descriptorSetLayout;

    VkShaderModule createShaderModule(const std::vector<char>& code);
    void createDescriptorSetLayout();
    void createPipeline(VkFormat colorFormat, Iridium::GBufferLayout gBufferLayout);
};
