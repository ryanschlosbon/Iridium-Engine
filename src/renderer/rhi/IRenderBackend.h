#pragma once
#include "DrawPacket.h"
#include "PipelineTypes.h"
#include "TextureTypes.h"
#include "RhiResourceTypes.h"
#include "RenderBackendConfig.h"
#include "RenderDebugView.h"
#include "FrameCapture.h"
#include "Ordinary2CaptureValidation.h"
#include "LightingTypes.h"
#include "Mesh.h"
#include "ReflectionProbeTypes.h"
#include "ReflectionProbeSettings.h"
#include "renderer/lighting/ReflectionProbeCapture.h"
#include "ShadowTypes.h"
#include "RenderBackendRuntimeInfo.h"
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

        virtual void updateCamera(const ViewTransportRecord& view) = 0;
        virtual void setDebugView(RenderDebugView view) = 0;
        virtual void setOutputSettings(float manualExposureEv,
            float paperWhiteNits, float peakNits) = 0;

        // Persistent cached directional shadow storage is updated before any
        // opaque/forward consumer reads it. An empty packet disables sampling.
        virtual void submitDirectionalShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const DirectionalShadowFramePacket> shadows) = 0;
        virtual void submitSpotShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const SpotShadowFramePacket> shadows) = 0;
        virtual void submitPointShadows(
            std::span<const DrawPacket> shadowCasters,
            std::span<const PointShadowFramePacket> shadows) = 0;
        virtual void submitReflectionProbeCaptures(
            std::span<const DrawPacket> opaqueCasters,
            std::span<const DrawPacket> complexOpaqueCasters,
            std::span<const ReflectionProbeCaptureScheduleEntry> captures,
            const LightingFramePacket& lights) = 0;
        [[nodiscard]] virtual ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept = 0;
        // Opaque cache key over caster geometry, transforms, pipeline state,
        // and backend-owned material revisions. It carries no Vulkan identity.
        [[nodiscard]] virtual uint64_t getShadowCasterRevision(
            std::span<const DrawPacket> shadowCasters) const noexcept = 0;

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
            std::span<const DrawPacket> compatibilityTransparentQueue) = 0;

        // Records an asynchronous readback of the post-transparency scene image.
        // The call itself does not wait for GPU completion. Completed captures may
        // be drained without waiting, or the explicit end-of-run drain may wait.
        virtual void captureCurrentFrame(uint64_t captureId,
            FrameCapturePoint point) = 0;
        [[nodiscard]] virtual std::vector<FrameCapture> collectFrameCaptures(
            bool waitForPending) = 0;

        // Explicit diagnostic only. The request is consumed by the next live
        // Ordinary2 capture/local composition with at least one prepared draw;
        // normal frames do not allocate a readback buffer or record transfers.
        virtual void requestOrdinary2CaptureValidation(
            uint64_t validationId) = 0;
        [[nodiscard]] virtual std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool waitForPending) = 0;
        virtual void requestDeepLayeredCaptureValidation(
            uint64_t validationId, TransparencyQuality quality) = 0;
        [[nodiscard]] virtual std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool waitForPending) = 0;

        // Pass 4: Maps scene-linear color into the selected display output.
        virtual void submitOutputPass() = 0;

        // Pass 5: Draws the ImGui editor and final UI elements to the swapchain.
        virtual void submitUIPass() = 0;

        virtual void beginUI() = 0;

        // ImGui uses 'void*' for its abstract texture IDs
        virtual void* getLitSceneTextureID() = 0;
        virtual void* getGlassDepthTextureID() = 0;
        virtual void* getEditorTextureID(TextureHandle texture) = 0;

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
        virtual void setEnvironmentLightingSettings(
            const EnvironmentLightingSettings&) {}
        virtual void setOutputTransformLut(TextureHandle lutHandle) = 0;
    };

} // namespace Iridium
