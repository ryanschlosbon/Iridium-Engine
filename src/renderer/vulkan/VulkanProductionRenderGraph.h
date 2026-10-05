#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/vulkan/VulkanProductionGraphIds.h"
#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/ShadowSettings.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/transparency/LayeredGlass.h"

#include <array>
#include <limits>
#include <vulkan/vulkan.h>

namespace Iridium {

    struct VulkanLayeredGraphConfig {
        VkExtent2D ordinary2AtlasExtent{};
        VkExtent2D hero4AtlasExtent{};
        VkExtent2D cinematic8AtlasExtent{};
        bool weightedOit = false;

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

    // Extension hook passes (M7R R2.7). A hook's only graph effect is its
    // declared usages and lifetimes (skipPass records nothing), so declaring
    // exactly these whenever their consumer is active keeps the graph
    // identical. Defaults reproduce the pre-R2.7 graph. "final-capture-hook"
    // is not optional: retained editor views also copy output through it.
    struct VulkanGraphHooks {
        // "depth.occlusion-pyramid.validation-readback-hook" (with depthPyramid).
        bool depthPyramidValidation = true;
        // "transparent.layered[.<tier>].validation-readback-hook" per
        // resident layered tier.
        bool layeredValidation = true;
        // Scene depth as a transfer source of the VSM request readback so the
        // depth qualification oracle can copy it.
        bool virtualShadowDepthSnapshot = true;
        // "scene-color-capture-hook" (M7R R3b.5): scene-linear capture copies
        // read scene.color as a transfer source after the last scene writer;
        // output-transform's begin returns it to SampledRead.
        bool sceneColorCapture = true;
        // M7R R4b.5 (design finding 5): the editor bridge samples
        // depth.opaque in the UI pass (glass-depth view), so the UI pass
        // declares that read and depth stays live (unaliased) until then.
        // Not a hook pass; the attached editor bridge requests it.
        bool editorDepthSample = false;

        [[nodiscard]] static constexpr VulkanGraphHooks none() noexcept {
            return { false, false, false, false, false };
        }
        [[nodiscard]] constexpr VulkanGraphHooks operator|(
            const VulkanGraphHooks& other) const noexcept {
            return { depthPyramidValidation || other.depthPyramidValidation,
                layeredValidation || other.layeredValidation,
                virtualShadowDepthSnapshot || other.virtualShadowDepthSnapshot,
                sceneColorCapture || other.sceneColorCapture,
                editorDepthSample || other.editorDepthSample };
        }
    };

    // Optional production-graph features. Defaults reproduce the default graph.
    struct VulkanProductionGraphFeatures {
        // M7.6 scene-depth pyramid (Hi-Z history/occlusion).
        bool depthPyramid = false;
        // M7.8 virtual-shadow working-set bytes; zero omits the VSM passes.
        uint64_t virtualShadowWorkingSetBytes = 0;
        // Copies the 64-byte cluster diagnostics for CPU telemetry; only needed
        // when frame counters are collected.
        bool clusterTelemetryReadback = true;
        VulkanGraphHooks hooks{};
        // Cube capacity of the 256/512/1024 point-shadow pools; the imported
        // pool images declare capacity x 6 layers (R3b.6 binds them).
        std::array<uint32_t, 3> pointShadowPoolCapacities{
            kPointShadowPool256Capacity, kPointShadowPool512Capacity,
            kPointShadowPool1024Capacity };
        // M7R R4b.4 (--render-graph-aliasing): compile with
        // CompileOptions::transientAliasing, so the executor places the
        // aliasing-eligible transient images in shared alias heaps.
        bool transientAliasing = false;
        // M9.2: native TAA. Declares temporal.taa.resolve and the taa.history
        // pair; the post chain (bloom, output) then reads the resolved colour.
        bool temporalAntiAliasing = false;
        // M9.5: auto-exposure. Declares the "exposure" History buffer pair
        // (SurviveCut) and post.exposure.{histogram,adapt} after the resolve;
        // TAA and the output transform read the adapted state.
        bool autoExposure = false;
    };

    [[nodiscard]] RenderGraph::CompiledGraph buildVulkanProductionRenderGraph(
        VkExtent2D extent, VkFormat swapchainFormat,
        VkFormat outputFormat = VK_FORMAT_B8G8R8A8_SRGB,
        bool hdr10Composition = false,
        GBufferLayout gBufferLayout = GBufferLayout::CanonicalReference,
        ClusterGridConfig clusterConfig = {},
        uint32_t directionalShadowResolution = 4096,
        uint32_t spotShadowAtlasResolution = 8192,
        bool transparencyPyramids = true,
        VulkanLayeredGraphConfig layered = {},
        VulkanProductionGraphFeatures features = {});
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
        VulkanLayeredGraphConfig layered = {},
        VulkanProductionGraphFeatures features = {});

    class VulkanRenderGraphExecutor;

    // The ids of the production passes and resources in the executor's bound
    // plan (R3b.4); undeclared ones are invalid. Call after every rebuild.
    [[nodiscard]] VulkanProductionGraphIds resolveVulkanProductionGraphIds(
        const VulkanRenderGraphExecutor& graph);

} // namespace Iridium
