#pragma once
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <string>
#include <vector>
#include "renderer/rhi/PipelineTypes.h"

namespace Iridium {

    class PipelineCache {
    public:
        void init(VkDevice device, VkRenderPass gBufferPass, VkRenderPass forwardPass, VkPipelineLayout layout);
        void cleanup();

        // The core abstraction: Hand it a state, get a compiled pipeline back!
        VkPipeline getOrCreatePipeline(const PipelineStateDesc& desc);

    private:
        VkDevice device;
        VkRenderPass gBufferRenderPass;
        VkRenderPass forwardRenderPass;
        VkPipelineLayout pipelineLayout;

        std::unordered_map<uint64_t, VkPipeline> pipelineMap;

        uint64_t generateHash(const PipelineStateDesc& desc);
        VkPipeline createVulkanPipeline(const PipelineStateDesc& desc);

        VkShaderModule createShaderModule(const std::vector<char>& code);
        std::vector<char> readFile(const std::string& filename);
    };

}