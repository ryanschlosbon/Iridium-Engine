#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/transparency/LayeredGlass.h"

#include <limits>
#include <vulkan/vulkan.h>

namespace Iridium {

    struct VulkanLayeredGraphConfig {
        VkExtent2D ordinary2AtlasExtent{};
        VkExtent2D hero4AtlasExtent{};
        VkExtent2D cinematic8AtlasExtent{};

        [[nodiscard]] constexpr VkExtent2D atlasExtent(
            TransparencyQuality quality) const noexcept {
            switch (quality) {
            case TransparencyQuality::Ordinary2:
                return ordinary2AtlasExtent;
            case TransparencyQuality::Hero4:
                return hero4AtlasExtent;
            case TransparencyQuality::Cinematic8:
                return cinematic8AtlasExtent;
            }
            return {};
        }

        [[nodiscard]] constexpr bool enabled(
            TransparencyQuality quality) const noexcept {
            const VkExtent2D extent = atlasExtent(quality);
            return extent.width != 0u || extent.height != 0u;
        }

        [[nodiscard]] constexpr bool anyEnabled() const noexcept {
            return enabled(TransparencyQuality::Ordinary2) ||
                enabled(TransparencyQuality::Hero4) ||
                enabled(TransparencyQuality::Cinematic8);
        }
    };

    // Compatibility name retained while the live renderer still executes only
    // the Ordinary2 tier. Aggregate initialization continues to populate the
    // first (Ordinary2) extent.
    using VulkanOrdinary2GraphConfig = VulkanLayeredGraphConfig;

    // Logical bytes per frame-context for the current D32 depth + R32 identity
    // interface representation, one R32 16x16-tile termination mask per
    // interface, and one RGBA16F local-color product.
    [[nodiscard]] constexpr uint64_t layeredTierLogicalStorageBytes(
        VkExtent2D atlasExtent, TransparencyQuality quality) noexcept {
        const LayeredQualityTierContract tier =
            layeredQualityTierContract(quality);
        if (!tier.valid() || atlasExtent.width == 0u ||
            atlasExtent.height == 0u) {
            return 0u;
        }
        constexpr uint64_t InterfaceBytesPerPixel =
            sizeof(float) + sizeof(uint32_t);
        constexpr uint64_t LocalColorBytesPerPixel = 8u;
        const uint64_t bytesPerPixel =
            tier.maximumInterfaceCount * InterfaceBytesPerPixel +
            LocalColorBytesPerPixel;
        const uint64_t pixelCount =
            static_cast<uint64_t>(atlasExtent.width) * atlasExtent.height;
        if (pixelCount > (std::numeric_limits<uint64_t>::max)() /
                bytesPerPixel) {
            return (std::numeric_limits<uint64_t>::max)();
        }
        if (quality == TransparencyQuality::Ordinary2)
            return pixelCount * bytesPerPixel;
        const uint64_t tileWidth = (atlasExtent.width +
            kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize;
        const uint64_t tileHeight = (atlasExtent.height +
            kDeepLayeredEarlyTerminationTileSize - 1u) /
            kDeepLayeredEarlyTerminationTileSize;
        const uint64_t tileBytes = tileWidth * tileHeight *
            deepLayeredTerminationMaskCount(tier.maximumInterfaceCount) *
            sizeof(uint32_t);
        const uint64_t interfaceBytes = pixelCount * bytesPerPixel;
        if (interfaceBytes > (std::numeric_limits<uint64_t>::max)() -
                tileBytes) {
            return (std::numeric_limits<uint64_t>::max)();
        }
        return interfaceBytes + tileBytes;
    }

    [[nodiscard]] RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
        VkExtent2D extent, VkFormat swapchainFormat,
        VkFormat outputFormat = VK_FORMAT_B8G8R8A8_SRGB,
        bool hdr10Composition = false,
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference,
        ClusterGridConfig clusterConfig = {},
        uint32_t directionalShadowResolution = 4096,
        uint32_t spotShadowAtlasResolution = 8192,
        bool transparencyPyramids = true,
        VulkanLayeredGraphConfig layered = {});
    [[nodiscard]] RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
        VkExtent2D sceneExtent, VkExtent2D presentationExtent,
        VkFormat swapchainFormat,
        VkFormat outputFormat = VK_FORMAT_B8G8R8A8_SRGB,
        bool hdr10Composition = false,
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference,
        ClusterGridConfig clusterConfig = {},
        uint32_t directionalShadowResolution = 4096,
        uint32_t spotShadowAtlasResolution = 8192,
        bool transparencyPyramids = true,
        VulkanLayeredGraphConfig layered = {});

} // namespace Iridium
