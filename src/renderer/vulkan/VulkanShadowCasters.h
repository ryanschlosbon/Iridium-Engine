#pragma once

// M7R R3c.5: what the shadow and reflection-probe feature owners share:
// caster resolution (the submission's GPU-scene primitives for one consumer,
// then its direct packets), the caster-content revision, the per-frame caster
// scratch, the direct-fallback shadow draw loop, the qualification-oracle draw
// counters and the oracle's CPU LOD selectors. Moved out of the backend
// unchanged.

#include "VulkanFeatureContext.h"
#include "VulkanExtensionHooks.h"
#include "VulkanIndirectCullerShared.h"
#include "VulkanIndirectViewCuller.h"
#include "core/BuildFeatures.h"
#include "renderer/rhi/IRenderBackend.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class VulkanResourceRegistry;

    // One backing store for the shadow kinds and the probe capture of a frame,
    // so steady frames allocate nothing.
    struct VulkanCasterScratch {
        std::vector<VulkanResolvedCaster> casters;
        // Per caster: visibility bits of the work item being recorded.
        std::vector<uint8_t> visibility;
    };

    // The submission's GPU-scene primitives that resolve for `consumerMask`,
    // then its direct packets.
    template<typename Submission, typename Visitor>
    void visitIndirectCasters(const VulkanIndirectScene& scene,
        const Submission& submission, uint32_t consumerMask, Visitor&& visitor) {
        VulkanResolvedCaster caster{};
        for (uint32_t primitiveIndex : submission.gpuScenePrimitiveIndices) {
            if (resolveIndirectCaster(scene, primitiveIndex, consumerMask, caster)) {
                caster.gpuScenePrimitiveIndex = primitiveIndex;
                visitor(caster);
            }
        }
        for (const DrawPacket& packet : submission.directPackets) {
            caster = {
                .geometry = packet.geometry,
                .material = packet.material,
                .pipeline = packet.pipeline,
                .worldTransform = packet.worldTransform,
                .boundsSphereCenterWorld = packet.boundsSphereCenterWorld,
                .boundsSphereRadiusWorld = packet.boundsSphereRadiusWorld,
                .indexCount = packet.indexCount,
                .firstIndex = packet.firstIndex,
                .gpuScenePrimitiveIndex = InvalidGpuSceneIndex,
                .owner = packet.owner,
            };
            visitor(caster);
        }
    }

    // Replaces scratch.casters with the submission's resolved casters.
    template<typename Submission>
    void resolveCasters(const VulkanIndirectScene& scene,
        const Submission& submission, uint32_t consumerMask,
        VulkanCasterScratch& scratch) {
        scratch.casters.clear();
        scratch.casters.reserve(submission.size());
        visitIndirectCasters(scene, submission, consumerMask,
            [&scratch](const VulkanResolvedCaster& caster) {
                scratch.casters.push_back(caster);
            });
    }

    // FNV-1a over the shadow casters' content (transform, handles, index
    // range, material revision and shadow-relevant state).
    [[nodiscard]] uint64_t shadowCasterRevision(const VulkanIndirectScene& scene,
        const VulkanResourceRegistry& resources,
        const ShadowCasterSubmission& casters) noexcept;
    // The same per directional cascade the caster's bounds touch.
    [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
        directionalShadowCasterRevisions(const VulkanIndirectScene& scene,
            const VulkanResourceRegistry& resources,
            const ShadowCasterSubmission& casters,
            const DirectionalShadowCascadePlan& plan) noexcept;

    // Geometry/material/pipeline payload lookups for the indirect cullers.
    [[nodiscard]] VulkanIndirectAssetResolver vulkanIndirectAssets(
        const VulkanFeatureContext& context) noexcept;

    // Oracle access for expectation-emission sites: null unless this is a
    // qualification build with an attached oracle enabled for the view.
    [[nodiscard]] inline IVulkanIndirectOracle* activeIndirectOracle(
        const VulkanExtensionHooks& hooks, VulkanIndirectOracleView view) noexcept {
        if constexpr (kQualificationBuild) {
            IVulkanIndirectOracle* oracle = hooks.indirectOracle();
            if (oracle != nullptr && oracle->enabled(view))
                return oracle;
        }
        (void)hooks;
        (void)view;
        return nullptr;
    }

    // Draw counters for the GPU-scene casters an oracle-checked shadow work
    // item drew (scratch.visibility[i] & visibilityBit).
    void recordIndirectOracleDraws(const VulkanFeatureContext& context,
        const VulkanCasterScratch& scratch, uint8_t visibilityBit,
        uint64_t& drawCounter, uint64_t& commandCounter,
        uint64_t& alphaMaskCounter);

    // The direct draws of one shadow work item: every caster whose visibility
    // has `visibilityBit`, except GPU-scene casters when the indirect path
    // drew them. Starts with no pipeline or geometry bound.
    struct VulkanShadowDirectDraws {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        const void* pipelineOwner = nullptr;
        VkPipeline (*pipeline)(const void* owner, bool alphaMasked,
            bool doubleSided) = nullptr;
        uint32_t slotWord = 0;           // CanonicalMeshPushConstants::padding[0]
        uint8_t visibilityBit = 1u;
        bool indirectValid = false;
        uint64_t* drawCounter = nullptr;
        uint64_t* alphaMaskCounter = nullptr;
    };
    void recordShadowDirectDraws(const VulkanFeatureContext& context,
        VkCommandBuffer commandBuffer, uint32_t frameIndex,
        const VulkanCasterScratch& scratch, const VulkanShadowDirectDraws& draws,
        bool& materialDescriptorsBound);

    // CPU LOD selectors for the qualification oracle's expected commands.
    struct DensityLodContext {
        float worldUnitsPerTexel = 0.0f;
        float errorTexels = 0.0f;
    };
    [[nodiscard]] IndirectLodMetric densityLodMetric(const DensityLodContext& context);
    struct PerspectiveLodContext {
        glm::mat4 worldToClip{ 1.0f };
        glm::vec2 viewportPixels{ 0.0f };
        float errorPixels = 0.0f;
    };
    [[nodiscard]] IndirectLodMetric perspectiveLodMetric(
        const PerspectiveLodContext& context);
    struct RadialLodContext {
        glm::vec3 position{ 0.0f };
        float resolution = 0.0f;
        float errorPixels = 0.0f;
    };
    [[nodiscard]] IndirectLodMetric radialLodMetric(const RadialLodContext& context);

    // Shadow and probe view settings (backend configuration).
    struct VulkanIndirectViewSettings {
        float lodErrorThreshold = 0.0f;
        uint32_t lodMaximumLevel = 15u;
        bool forceDirectGBufferReference = false;
        bool forceDirectShadowReference = false;
    };

    // What a view culler is created with: the backend's culler services and
    // the shared 3-binding indirect set layout.
    struct VulkanIndirectViewSetup {
        VulkanCullerServices services{};
        VkDescriptorSetLayout indirectLayout = VK_NULL_HANDLE;
    };

} // namespace Iridium
