#pragma once

// M7R R3c.8: deferred lighting as a feature owner. Owns the lighting render
// pass and pipeline, the lighting set (VulkanSceneDescriptors: G-buffer and
// scene targets, light records, clusters, environment, the directional/spot/
// point shadow maps with their frame data, and the reflection-probe records,
// clusters and environment table) and the environment products (the active
// image-based lighting, its settings and the neutral cube/BRDF-LUT fallback).
// Registers "lighting"; gpu.lighting.deferred opens inside the render pass,
// as before. The forward, layered and WeightedOIT consumers read the lighting
// set through sceneSet(frame). The probe-clustering sets are rebound with the
// lighting set's probe buffers (bindReflectionProbeBuffers), in the old order.

#include "renderer/rhi/GBufferLayout.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/RenderDebugView.h"

#include "VkLightingPipeline.h"
#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanSceneDescriptors.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace Iridium {

    class VulkanClusterLightingFeature;
    class VulkanLocalShadowFeature;
    class VulkanReflectionProbeFeature;
    class VulkanShadowFeature;

    class VulkanDeferredLightingFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;

        // The owners whose products the lighting set binds.
        struct Sources {
            VulkanClusterLightingFeature* clusters = nullptr;
            VulkanShadowFeature* shadows = nullptr;
            VulkanLocalShadowFeature* localShadows = nullptr;
            VulkanReflectionProbeFeature* probes = nullptr;
        };

        // Per-frame inputs (submitLightingPass), valid until the drain.
        struct FrameInputs {
            glm::vec3 cameraPosition{ 0.0f };
            glm::mat4 view{ 1.0f };
            glm::mat4 projection{ 1.0f };
            RenderDebugView debugView = RenderDebugView::Final;
        };

        VulkanDeferredLightingFeature() = default;
        VulkanDeferredLightingFeature(const VulkanDeferredLightingFeature&) = delete;
        VulkanDeferredLightingFeature& operator=(const VulkanDeferredLightingFeature&) = delete;

        void configure(GBufferLayout layout, const Sources& sources) noexcept {
            gBufferLayout_ = layout;
            sources_ = sources;
        }

        // IVulkanFeature. create() builds the pipeline (the
        // lighting set layout is needed by the mesh layouts right after).
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        [[nodiscard]] VkDescriptorSetLayout setLayout() const noexcept {
            return pipeline_->getDescriptorSetLayout();
        }

        // Environment products. createNeutralEnvironment runs once at init
        // (before the initial upload flush); setEnvironment validates and
        // selects the active products, which each slot binds after its fence.
        void createNeutralEnvironment();
        void setEnvironment(const EnvironmentLightingHandles& environment);
        [[nodiscard]] const EnvironmentLightingHandles& environment() const noexcept {
            return environment_;
        }
        void setEnvironmentSettings(const EnvironmentLightingSettings& settings);
        [[nodiscard]] const EnvironmentLightingSettings& environmentSettings() const noexcept {
            return environmentSettings_;
        }
        // beginFrame: rebinds the slot's environment when it changed.
        void bindFrameEnvironment(uint32_t frame);

        // The lighting set. rebuildSceneSet creates it after the frame targets
        // and binds every product (lights, clusters, environment, shadows,
        // probes) in the original order; releaseSceneSet frees it before the
        // targets are released.
        void rebuildSceneSet();
        void releaseSceneSet() { scene_.cleanup(); }
        [[nodiscard]] bool sceneSetReady() const noexcept { return scene_.size() != 0; }
        [[nodiscard]] VkDescriptorSet sceneSet(uint32_t frame) const {
            return scene_.get(frame);
        }
        // Light-record/cluster replacement (prepareLighting), probe-buffer
        // replacement and environment-table changes (the probe owner).
        // M7R R4c: `frame` != UINT32_MAX rebinds one retired (or idle) slot.
        void bindLightBuffers(uint32_t frame = UINT32_MAX);
        void bindReflectionProbeBuffers(uint32_t frame = UINT32_MAX);
        void bindReflectionProbeEnvironments(uint32_t frame = UINT32_MAX);

        // Drain point: "lighting".
        void record(const FrameInputs& inputs);

    private:
        static void executeLighting(void* owner, VulkanPassContext& context);
        void recordLighting(VulkanPassContext& context);
        void bindEnvironmentProducts(uint32_t frame = UINT32_MAX);
        void bindShadows();

        const VulkanFeatureContext* context_ = nullptr;
        GBufferLayout gBufferLayout_ = GBufferLayout::CanonicalReference;
        Sources sources_{};
        std::unique_ptr<VkLightingPipeline> pipeline_;
        VulkanSceneDescriptors scene_;
        RenderGraph::PassId lightingPass_{};

        EnvironmentLightingHandles environment_;
        EnvironmentLightingSettings environmentSettings_;
        TextureHandle neutralCube_;
        TextureHandle neutralBrdfLut_;
        std::array<EnvironmentLightingHandles, FrameCount> frameEnvironments_{};

        // Staged for the frame's callback (see VulkanFeatureContext.h).
        FrameInputs staged_{};
    };

} // namespace Iridium
