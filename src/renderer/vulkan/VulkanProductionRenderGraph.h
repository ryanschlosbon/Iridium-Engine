#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/lighting/ClusteredLighting.h"

#include <vulkan/vulkan.h>

namespace Iridium {

    [[nodiscard]] RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
        VkExtent2D extent, VkFormat swapchainFormat,
        VkFormat outputFormat = VK_FORMAT_B8G8R8A8_SRGB,
        bool hdr10Composition = false,
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference,
        ClusterGridConfig clusterConfig = {},
        uint32_t directionalShadowResolution = 4096,
        uint32_t spotShadowAtlasResolution = 8192);
    [[nodiscard]] RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
        VkExtent2D sceneExtent, VkExtent2D presentationExtent,
        VkFormat swapchainFormat,
        VkFormat outputFormat = VK_FORMAT_B8G8R8A8_SRGB,
        bool hdr10Composition = false,
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference,
        ClusterGridConfig clusterConfig = {},
        uint32_t directionalShadowResolution = 4096,
        uint32_t spotShadowAtlasResolution = 8192);

} // namespace Iridium
