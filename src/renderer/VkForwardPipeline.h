#pragma once
#include "renderer/VkContext.h"
#include "VkForwardRenderPass.h"
#include <vulkan/vulkan.h>
#include <vector>

class VkForwardPipeline {
public:
    VkForwardPipeline(VkContext* context, VkForwardRenderPass* renderPass, VkDescriptorSetLayout lightingSetLayout);
    ~VkForwardPipeline();

    VkPipeline getPipeline() const { return pipeline; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout; }
    VkDescriptorSetLayout getGlobalSetLayout() const { return globalSetLayout; }
    VkDescriptorSetLayout getMaterialSetLayout() const { return materialSetLayout; }

private:
    VkContext* context;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout globalSetLayout;
    VkDescriptorSetLayout materialSetLayout;
    VkDescriptorSetLayout lightingSetLayout;

    void createDescriptorSetLayouts();
    void createPipeline(VkForwardRenderPass* renderPass);
    VkShaderModule createShaderModule(const std::vector<char>& code);
};