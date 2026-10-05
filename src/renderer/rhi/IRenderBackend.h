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
#include "RenderFrame.h"
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
        // Between frames: switches the anti-aliasing mode (a graph topology
        // change; temporal history starts over). On failure the previous mode
        // remains active.
        [[nodiscard]] virtual bool setAntiAliasing(
            AntiAliasingMode mode, std::string& diagnostic) = 0;
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
        // After beginFrame, before the frame is extracted and submitted.
        virtual void publishGpuScene(const GpuScenePackedTables& scene) = 0;
        [[nodiscard]] virtual GpuSceneFrameSerials
            getGpuSceneFrameSerials() const noexcept = 0;
        // Grows fence-owned probe records/cluster products and publishes the
        // abstract local-environment table before beginFrame acquires a slot.
        virtual void prepareReflectionProbes(uint32_t requiredCapacity,
            std::span<const EnvironmentLightingHandles> environments) = 0;
        // Completes fence-safe runtime capture publication before a frame opens.
        // The completions are valid until the next call (backend storage).
        [[nodiscard]] virtual std::span<const ReflectionProbeCaptureCompletion>
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

        // Records the whole frame (M7R R3c.11): view and output state, the
        // three shadow kinds, probe captures, the G-buffer, deferred lighting,
        // forward and transparency, the output transform and the UI pass, in
        // that order, reporting stage boundaries to frame.stageObserver.
        // Requires an open frame; the frame's spans need only outlive the call.
        // Frame captures and capture-validation readbacks are not part of this
        // interface: the qualification harness attaches a backend extension
        // for them (M7R R2.9); the editor UI is an IEditorRenderBridge
        // extension (M7R R3c.10).
        virtual void submitFrame(const RenderFrame& frame) = 0;
        // Upload, probe-capture and clustered-lighting telemetry of the frame
        // being recorded (see RenderFrameTelemetry for when each is current).
        [[nodiscard]] virtual RenderFrameTelemetry frameTelemetry() const noexcept = 0;

        // Opaque cache key over caster geometry, transforms, pipeline state,
        // and backend-owned material revisions. It carries no Vulkan identity.
        // Valid once the frame's GPU scene is published. M7R R5c.1: a
        // monotonic revision that advances exactly when that content changes
        // (content returning to an earlier state gets a new value), evaluated
        // from change triggers rather than per-frame hashing.
        [[nodiscard]] virtual uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission& shadowCasters) = 0;
        // Cache identities for the independent conservative caster membership
        // of each directional cascade. Backend material revisions are included.
        // Call once per directional light per frame, in shadow-index order.
        [[nodiscard]] virtual std::array<uint64_t,
            kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(
                const ShadowCasterSubmission& shadowCasters,
                const DirectionalShadowCascadePlan& plan) = 0;

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
