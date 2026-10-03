#pragma once
#include "DrawPacket.h"
#include "PipelineTypes.h"
#include "core/types/TextureTypes.h"
#include "RhiResourceTypes.h"
#include "RenderBackendConfig.h"
#include "RenderDebugView.h"
#include "LightingTypes.h"
#include "Mesh.h"
#include "ReflectionProbeTypes.h"
#include "ReflectionProbeSettings.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "ShadowTypes.h"
#include "RenderBackendRuntimeInfo.h"
#include "ViewportGridOverlay.h"
#include "GpuScene.h"
#include "GeometryArena.h"
#include "DepthPyramid.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// Forward declare GLFWwindow so we don't need to include the heavy GLFW header
// inside our clean abstraction layer.
struct GLFWwindow;

namespace Iridium {

    // Shadow consumers address persistent scene geometry through the same
    // frame-local dense primitive indices used by GPU-scene indirect work.
    // Packets remain only as an explicit compatibility path for producers
    // that could not enter the persistent publication.
    struct ShadowCasterSubmission {
        std::span<const uint32_t> gpuScenePrimitiveIndices;
        std::span<const DrawPacket> directPackets;
        uint64_t membershipRevision = 0;

        [[nodiscard]] constexpr size_t size() const noexcept {
            return gpuScenePrimitiveIndices.size() + directPackets.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0u;
        }
    };

    // Reflection captures are an independent visibility consumer. Dense
    // references come from GpuSceneConsumerProbe rather than either main-view
    // queue; packets remain only for publication fallback.
    struct ReflectionProbeCasterSubmission {
        std::span<const uint32_t> gpuScenePrimitiveIndices;
        std::span<const DrawPacket> directPackets;
        uint64_t membershipRevision = 0;

        [[nodiscard]] constexpr size_t size() const noexcept {
            return gpuScenePrimitiveIndices.size() + directPackets.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0u;
        }
    };

    struct EnvironmentLightingHandles {
        TextureHandle radiance;
        TextureHandle irradiance;
        TextureHandle prefilteredSpecular;
        TextureHandle brdfLut;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return radiance.isValid() && irradiance.isValid() &&
                prefilteredSpecular.isValid() && brdfLut.isValid();
        }

        friend constexpr bool operator==(const EnvironmentLightingHandles&,
            const EnvironmentLightingHandles&) = default;
    };

    struct EnvironmentLightingSettings {
        float lightingIntensity = 1.0f;
        float backgroundIntensity = 1.0f;
        float rotationRadians = 0.0f;
        bool visibleToCamera = true;
        bool affectsLighting = true;
    };

    // Describes optional, content-dependent graph products that may be made
    // resident before the first frame. The contract is backend neutral: a
    // backend may satisfy a request by changing topology, reserving resources,
    // or doing nothing when those products are already available.
    struct FrameTopologyRequirements {
        bool refractionPyramids = false;
        bool ordinary2LayeredInterfaces = false;
        bool hero4LayeredInterfaces = false;
        bool cinematic8LayeredInterfaces = false;
        bool weightedOit = false;
    };

    struct FrameTopologyPreparation {
        bool requested = false;
        bool changed = false;
        uint64_t durationNanoseconds = 0;
    };

    class IRenderBackend {
    public:
        // A virtual destructor is MANDATORY for C++ interfaces to ensure child classes 
        // (like VulkanVertexBackend) correctly fire their own destructors.
        virtual ~IRenderBackend() = default;

        // ==============================================================================
        // 1. SYSTEM LIFECYCLE
        // ==============================================================================
        virtual void init(GLFWwindow* window, const RenderBackendConfig& config) = 0;
        virtual void cleanup() = 0;
        virtual void recreateSwapchain(GLFWwindow* window) = 0;
        // Changes presentation transport at a frame boundary while preserving
        // scene, material, geometry, and texture residency.
        virtual void setOutputTransport(GLFWwindow* window,
            Color::OutputTransport requestedTransport) = 0;
        [[nodiscard]] virtual RenderExtent getRenderExtent() const = 0;
        // Changes only scene/offscreen targets. Presentation remains owned by
        // the swapchain. On failure the previous extent must remain active.
        [[nodiscard]] virtual bool resizeSceneRenderExtent(
            RenderExtent extent, std::string& diagnostic) = 0;
        [[nodiscard]] virtual RenderBackendCapabilities getCapabilities() const = 0;
        [[nodiscard]] virtual RenderBackendRuntimeInfo getRuntimeInfo() const = 0;
        // May only be called between frames. Startup callers use this to move
        // predictable content-driven topology work out of the first interactive
        // frame without forcing optional products into every empty scene.
        [[nodiscard]] virtual FrameTopologyPreparation prepareFrameTopology(
            const FrameTopologyRequirements& requirements) = 0;

        // ==============================================================================
        // 2. THE FRAME PIPELINE
        // ==============================================================================

        // Performs fence-safe capacity growth before beginFrame acquires a slot.
        virtual void prepareLighting(uint32_t requiredCapacity) = 0;
        // Grows fence-owned persistent scene tables before a frame slot is
        // acquired. Publication then updates only revision-mismatched ranges
        // in the acquired context.
        virtual void prepareGpuScene(
            const GpuSceneCapacityRequirements& requirements) = 0;
        virtual void publishGpuScene(const GpuScenePackedTables& scene) = 0;
        [[nodiscard]] virtual GpuSceneFrameSerials
            getGpuSceneFrameSerials() const noexcept = 0;
        [[nodiscard]] virtual GpuSceneUploadTelemetry
            getGpuSceneUploadTelemetry() const noexcept = 0;
        // Grows fence-owned probe records/cluster products and publishes the
        // abstract local-environment table before beginFrame acquires a slot.
        virtual void prepareReflectionProbes(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments) = 0;
        // Completes fence-safe runtime capture publication before a frame opens.
        [[nodiscard]] virtual std::vector<ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() = 0;
        [[nodiscard]] virtual std::optional<uint32_t>
            capturedReflectionProbeEnvironmentSlot(
                SceneEntityUuid owner) const noexcept = 0;
        virtual void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid> owners) = 0;
        virtual void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings& settings) = 0;

        // Prepares swapchains, acquires the next image, and resets command buffers
        virtual FrameStatus beginFrame() = 0;

        virtual void updateCamera(const ViewTransportRecord& view,
            ViewHistoryContext history = {}) = 0;
        virtual void setDebugView(RenderDebugView view) = 0;
        virtual void setOutputSettings(float manualExposureEv,
            float paperWhiteNits, float peakNits) = 0;
        virtual void setViewportGridOverlay(
            const ViewportGridOverlay& overlay) = 0;

        // Persistent cached directional shadow storage is updated before any
        // opaque/forward consumer reads it. An empty packet disables sampling.
        virtual void submitDirectionalShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows) = 0;
        virtual void submitSpotShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const SpotShadowFramePacket> shadows) = 0;
        virtual void submitPointShadows(
            const ShadowCasterSubmission& shadowCasters,
            std::span<const PointShadowFramePacket> shadows) = 0;
        virtual void submitReflectionProbeCaptures(
            const ReflectionProbeCasterSubmission& probeCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights) = 0;
        [[nodiscard]] virtual ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept = 0;
        // Opaque cache key over caster geometry, transforms, pipeline state,
        // and backend-owned material revisions. It carries no Vulkan identity.
        [[nodiscard]] virtual uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission& shadowCasters) const noexcept = 0;
        // Cache identities for the independent conservative caster membership
        // of each directional cascade. Backend material revisions are included.
        [[nodiscard]] virtual std::array<uint64_t,
            kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(
                const ShadowCasterSubmission& shadowCasters,
                const DirectionalShadowCascadePlan& plan) const noexcept = 0;

        // Freezes the complete depth-writing content identity before main-view
        // compaction. Both queues contribute to the depth pyramid even though
        // complex-forward raster is submitted later.
        virtual void prepareDepthPyramidHistory(
            std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> opaqueForwardQueue) {}

        // Pass 1: Draws opaque meshes to the G-Buffer (Normal, Albedo, Depth)
        virtual void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) = 0;
        
        // Pass 2: Evaluates the G-Buffer using the HDRI and outputs the lit scene
        virtual void submitLightingPass(const glm::vec3& cameraPos,
            const glm::mat4& view, const glm::mat4& proj,
            float nearPlane, float farPlane,
            const LightingFramePacket& lights,
            const ReflectionProbeGpuFramePacket& reflectionProbes) = 0;
        [[nodiscard]] virtual LightingUploadTelemetry
            getLightingUploadTelemetry() const noexcept = 0;
        [[nodiscard]] virtual ClusteredLightingTelemetry
            getClusteredLightingTelemetry() const noexcept = 0;

        // Pass 3: Draws opaque complex closures with depth writes, classified
        // sorted surfaces with read-only depth and premultiplied blending, then
        // retained compatibility transparency through the bounded glass path.
        virtual void submitForwardQueues(
            std::span<const DrawPacket> opaqueForwardQueue,
            std::span<const DrawPacket> sortedSurfaceQueue,
            std::span<const DrawPacket> compatibilityTransparentQueue,
            std::span<const glm::mat4> instanceTransforms = {}) = 0;

        // Frame captures and capture-validation readbacks are not part of this
        // interface: the qualification harness attaches a backend extension
        // for them (M7R R2.9).

        // Pass 4: Maps scene-linear color into the selected display output.
        virtual void submitOutputPass() = 0;

        // Pass 5: The UI pass: clears the presentation target, records the
        // attached editor bridge's UI (IEditorRenderBridge, M7R R3c.10) and
        // presents. The editor's texture ids and retained views are bridge
        // services, not part of this interface.
        virtual void submitUIPass() = 0;

        // Submits the command buffers to the GPU and presents to the monitor
        virtual FrameStatus endFrame() = 0;

        // ==============================================================================
        // 3. RESOURCE MANAGEMENT (The Vault Doors)
        // ==============================================================================
        // Notice how none of these use VkBuffer, VkImage, or VkDescriptorSet.
        // We pass pure data in, and we get a lightweight Handle back.

        // --- GEOMETRY ---
        virtual GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes) = 0;
        virtual void freeGeometry(GeometryHandle handle) = 0;
        // Allocates one shared vertex stream and split index streams. Each
        // primitive handle carries its base-vertex binding internally so the
        // established DrawPacket ABI remains unchanged.
        virtual GeometryArenaAllocation allocateGeometryArena(
            uint32_t vertexStride,
            std::span<const std::byte> vertexBytes,
            const GeometryArenaData& arena) = 0;
        virtual void freeGeometryArena(
            std::span<const GeometryHandle> primitiveGeometry) = 0;

        // --- TEXTURES ---
        virtual TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes) = 0;
        virtual void freeTexture(TextureHandle handle) = 0;

        // --- MATERIALS ---
        // The backend consumes only compiled canonical material records.
        virtual MaterialBinding allocateCanonicalMaterial(
            const CanonicalMaterialAsset& desc) = 0;
        virtual void updateCanonicalMaterial(MaterialHandle handle,
            const PackedGpuMaterial& material) = 0;
        virtual void freeMaterial(MaterialHandle handle) = 0;

        virtual void setEnvironmentLighting(
            const EnvironmentLightingHandles& environment) = 0;
        [[nodiscard]] virtual EnvironmentLightingHandles getEnvironmentLighting() const { return {}; }
        virtual void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings&) {}
        virtual void setOutputTransformLut(TextureHandle lutHandle) = 0;
    };

} // namespace Iridium
