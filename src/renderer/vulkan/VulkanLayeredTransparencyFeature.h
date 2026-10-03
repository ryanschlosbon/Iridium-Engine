#pragma once

// M7R R3c.9: layered glass as a feature owner. Owns the Ordinary2 tier and
// the Hero4/Cinematic8 deep tiers: atlas residency and extents (the topology
// that declares their passes), the per-frame request collection, atlas and
// capture-draw plans, the resolved-packet indices the compatibility forward
// pass skips, and the interface-capture, local-composition and scene-resolve
// passes. Registers "transparent.layered.{entry,exit}.capture",
// "transparent.layered.local-compose", "transparent.layered.compose-hook"
// (the Ordinary2 scene resolve), the deep tiers' per-interface
// ".capture"/".terminate-tiles" and ".local-compose" passes and the deep
// compose hook (the deep scene resolve); every gpu.transparency.layered.*
// range opens before its pass's barriers, as before. The validation readback
// hooks between them stay with VulkanHookPasses; this owner supplies their
// payloads.

#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "renderer/transparency/Ordinary2Atlas.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"

#include "VulkanBackendExtension.h"
#include "VulkanFeatureContext.h"
#include "VulkanLayeredInterfaceCapturePass.h"
#include "VulkanLayeredLocalCompositionPass.h"
#include "VulkanLayeredSceneResolvePass.h"
#include "VulkanProductionGraphIds.h"
#include "VulkanRenderGraphExecutor.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>

namespace Iridium {

    class VulkanLayeredTransparencyFeature final : public IVulkanFeature {
    public:
        // Per-frame inputs (submitForwardQueues), valid until the last drain.
        struct FrameInputs {
            std::span<const DrawPacket> compatibilityTransparentQueue{};
            // The frame's command buffer (extension notifications).
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkDescriptorSet globalSet = VK_NULL_HANDLE;
            VkDescriptorSet sceneSet = VK_NULL_HANDLE;
            RenderDebugView debugView = RenderDebugView::Final;
        };

        VulkanLayeredTransparencyFeature() = default;
        VulkanLayeredTransparencyFeature(const VulkanLayeredTransparencyFeature&) = delete;
        VulkanLayeredTransparencyFeature& operator=(const VulkanLayeredTransparencyFeature&) = delete;

        // Before create(): the lighting set layout (local composition lights
        // the captured interfaces) and the scene render pass the resolve
        // composites into (the transparent forward pass).
        void configure(VkDescriptorSetLayout lightingSetLayout,
            VkRenderPass sceneRenderPass) noexcept {
            lightingSetLayout_ = lightingSetLayout;
            sceneRenderPass_ = sceneRenderPass;
        }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Render passes for the frame targets; descriptors over them.
        [[nodiscard]] VkRenderPass interfaceCaptureRenderPass() const noexcept {
            return interfaceCapture_.renderPass();
        }
        [[nodiscard]] VkRenderPass localCompositionRenderPass() const noexcept {
            return localComposition_.renderPass();
        }
        void rebuildDescriptors();
        void clearDescriptors() noexcept;

        // Atlas residency and extents (topology changes only).
        [[nodiscard]] TransparencyPyramidResidency& ordinary2Residency() noexcept {
            return ordinary2Residency_;
        }
        [[nodiscard]] const TransparencyPyramidResidency& ordinary2Residency() const noexcept {
            return ordinary2Residency_;
        }
        [[nodiscard]] TransparencyPyramidResidency& hero4Residency() noexcept {
            return hero4Residency_;
        }
        [[nodiscard]] const TransparencyPyramidResidency& hero4Residency() const noexcept {
            return hero4Residency_;
        }
        [[nodiscard]] TransparencyPyramidResidency& cinematic8Residency() noexcept {
            return cinematic8Residency_;
        }
        [[nodiscard]] const TransparencyPyramidResidency& cinematic8Residency() const noexcept {
            return cinematic8Residency_;
        }
        [[nodiscard]] VkExtent2D& ordinary2Extent() noexcept { return ordinary2Extent_; }
        [[nodiscard]] const VkExtent2D& ordinary2Extent() const noexcept { return ordinary2Extent_; }
        [[nodiscard]] VkExtent2D& hero4Extent() noexcept { return hero4Extent_; }
        [[nodiscard]] const VkExtent2D& hero4Extent() const noexcept { return hero4Extent_; }
        [[nodiscard]] VkExtent2D& cinematic8Extent() noexcept { return cinematic8Extent_; }
        [[nodiscard]] const VkExtent2D& cinematic8Extent() const noexcept {
            return cinematic8Extent_;
        }
        // The resident tiers (non-empty atlas extents).
        [[nodiscard]] bool ordinary2Active() const noexcept {
            return ordinary2Extent_.width != 0u && ordinary2Extent_.height != 0u;
        }
        [[nodiscard]] bool deepActive(TransparencyQuality quality) const noexcept;

        // Per frame. beginFrame invalidates the view projection, which the
        // lighting submission publishes; observe records the tiers' demand;
        // prepare builds the frame's plans (inside cpu.render.record.forward)
        // and stages the inputs.
        void beginFrame() noexcept { viewProjectionValid_ = false; }
        void setViewProjection(const glm::mat4& viewProjection) noexcept {
            viewProjection_ = viewProjection;
            viewProjectionValid_ = true;
        }
        void observe(std::span<const DrawPacket> compatibilityTransparentQueue);
        void prepare(const FrameInputs& inputs);
        // A packet the frame's layered tiers resolve (the compatibility
        // forward pass skips it).
        [[nodiscard]] bool isPacketResolved(uint32_t packetIndex) const noexcept;

        // Drain points, in recording order.
        void recordOrdinary2Captures();
        void recordOrdinary2LocalComposition();
        [[nodiscard]] VulkanOrdinary2HookPayload ordinary2HookPayload() const noexcept;
        void recordOrdinary2SceneResolve();
        void recordDeepCaptures(TransparencyQuality quality);
        void recordDeepLocalComposition(TransparencyQuality quality);
        [[nodiscard]] VulkanDeepLayeredHookPayload deepHookPayload(
            TransparencyQuality quality) const;
        void recordDeepSceneResolve();

    private:
        static constexpr uint32_t MaximumInterfaces = VulkanDeepLayeredGraphIds::MaximumInterfaces;

        struct Ordinary2Slot {
            VulkanLayeredTransparencyFeature* self = nullptr;
            bool exit = false;
        };
        struct DeepSlot {
            VulkanLayeredTransparencyFeature* self = nullptr;
            TransparencyQuality quality = TransparencyQuality::Hero4;
            uint32_t interfaceIndex = 0;
        };
        struct DeepTier {
            VulkanDeepLayeredGraphIds ids{};
            std::array<DeepSlot, MaximumInterfaces> interfaces{};
            DeepSlot tier{};
        };

        [[nodiscard]] static uint32_t tierSlot(TransparencyQuality quality);
        [[nodiscard]] bool tierHasDraws(TransparencyQuality quality) const noexcept {
            return tierHasDraws_[tierSlot(quality)];
        }
        void prepareOrdinary2ResolvedPacketIndices(std::span<const Ordinary2CaptureDraw> draws);
        void prepareDeepResolvedPacketIndices(std::span<const LayeredCaptureDraw> draws);
        [[nodiscard]] bool isOrdinary2PacketResolved(uint32_t packetIndex) const noexcept;
        [[nodiscard]] bool isDeepPacketResolved(uint32_t packetIndex) const noexcept;

        static bool ordinary2PassActive(void* owner, const VulkanFrameRecordContext& frame);
        static bool ordinary2CaptureActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeOrdinary2Capture(void* owner, VulkanPassContext& context);
        static void executeOrdinary2LocalComposition(void* owner, VulkanPassContext& context);
        static void executeOrdinary2SceneResolve(void* owner, VulkanPassContext& context);
        static bool deepTierActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeDeepCapture(void* owner, VulkanPassContext& context);
        static void executeDeepTermination(void* owner, VulkanPassContext& context);
        static void executeDeepLocalComposition(void* owner, VulkanPassContext& context);
        static bool deepResolveActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeDeepSceneResolve(void* owner, VulkanPassContext& context);

        // R4a: these record inside the pass's dynamic-rendering scope.
        void drawOrdinary2Capture(VulkanPassContext& context, bool exit);
        void drawOrdinary2LocalComposition(VulkanPassContext& context);
        void drawOrdinary2SceneResolve(VkCommandBuffer commandBuffer, uint32_t frame);
        void drawDeepCapture(VulkanPassContext& context, TransparencyQuality quality,
            uint32_t interfaceIndex);
        void drawDeepLocalComposition(VulkanPassContext& context,
            TransparencyQuality quality);
        void drawDeepSceneResolve(VkCommandBuffer commandBuffer, uint32_t frame);

        const VulkanFeatureContext* context_ = nullptr;
        VkDescriptorSetLayout lightingSetLayout_ = VK_NULL_HANDLE;
        VkRenderPass sceneRenderPass_ = VK_NULL_HANDLE;
        VulkanLayeredInterfaceCapturePass interfaceCapture_;
        VulkanLayeredLocalCompositionPass localComposition_;
        VulkanLayeredSceneResolvePass sceneResolve_;

        TransparencyPyramidResidency ordinary2Residency_;
        TransparencyPyramidResidency hero4Residency_;
        TransparencyPyramidResidency cinematic8Residency_;
        VkExtent2D ordinary2Extent_{};
        VkExtent2D hero4Extent_{};
        VkExtent2D cinematic8Extent_{};

        Ordinary2RequestCollector ordinary2RequestCollector_;
        Ordinary2AtlasPlan ordinary2AtlasPlan_;
        Ordinary2CaptureDrawPlan ordinary2CaptureDrawPlan_;
        LayeredRequestCollector deepRequestCollector_;
        LayeredAtlasPlan deepAtlasPlan_;
        LayeredCaptureDrawPlan deepCaptureDrawPlan_;
        std::array<Ordinary2CaptureDraw, kOrdinary2MaximumWorkCount> ordinary2ResolvedDraws_{};
        uint32_t ordinary2ResolvedPacketCount_ = 0u;
        std::array<LayeredCaptureDraw, kOrdinary2MaximumWorkCount> deepResolvedDraws_{};
        uint32_t deepResolvedPacketCount_ = 0u;
        glm::mat4 viewProjection_{ 1.0f };
        bool viewProjectionValid_ = false;

        std::array<Ordinary2Slot, 2> ordinary2Captures_{};
        RenderGraph::PassId ordinary2EntryPass_{};
        RenderGraph::PassId ordinary2ExitPass_{};
        RenderGraph::PassId ordinary2LocalComposePass_{};
        RenderGraph::PassId ordinary2ResolvePass_{};
        std::array<DeepTier, 2> deepTiers_{};
        RenderGraph::PassId deepResolvePass_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        FrameInputs staged_{};
        std::span<const Ordinary2CaptureDraw> stagedCaptureDraws_{};
        std::span<const LayeredCaptureDraw> stagedDeepDraws_{};
        std::array<bool, 2> tierHasDraws_{};
        std::array<uint32_t, kLayeredQualityTierCount> resolveDrawCounts_{};
        bool resolveRecorded_ = false;
    };

} // namespace Iridium
