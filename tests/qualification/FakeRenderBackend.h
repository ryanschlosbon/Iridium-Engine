#pragma once

// A device-free IRenderBackend for observer/harness tests. Every call is a no-op
// except the ones the qualification harness makes, which are recorded in order.

#include "app/FrameObserver.h"
#include "assets/AssetManager.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace IridiumTest {

    using namespace Iridium;

    struct BackendCall {
        enum class Kind : uint8_t {
            AllocateTexture,
            FreeTexture,
            AllocateMaterial,
            FreeMaterial,
            CaptureCurrentFrame,
            RequestOrdinary2Validation,
            RequestDeepLayeredValidation,
            RequestDepthPyramidValidation,
        };
        Kind kind{};
        uint64_t value = 0;     // handle index, capture or validation id
        uint32_t detail = 0;    // capture point or transparency quality
        std::string marker;     // FakeRenderBackend::marker at call time
    };

    class FakeRenderBackend final : public IRenderBackend {
    public:
        // Tests set this before each hook so calls can be attributed to it.
        std::string marker;
        std::vector<BackendCall> calls;
        RenderBackendRuntimeInfo runtimeInfo{};

        [[nodiscard]] size_t count(BackendCall::Kind kind) const {
            size_t total = 0;
            for (const BackendCall& call : calls)
                if (call.kind == kind) ++total;
            return total;
        }

        void init(GLFWwindow*, const RenderBackendConfig&) override {}
        void cleanup() override {}
        void recreateSwapchain(GLFWwindow*) override {}
        void setOutputTransport(GLFWwindow*, Color::OutputTransport) override {}
        RenderExtent getRenderExtent() const override { return { 1280u, 720u }; }
        bool resizeSceneRenderExtent(RenderExtent, std::string&) override {
            return true;
        }
        RenderBackendCapabilities getCapabilities() const override { return {}; }
        RenderBackendRuntimeInfo getRuntimeInfo() const override {
            return runtimeInfo;
        }
        FrameTopologyPreparation prepareFrameTopology(
            const FrameTopologyRequirements&) override { return {}; }

        void prepareLighting(uint32_t) override {}
        void prepareGpuScene(const GpuSceneCapacityRequirements&) override {}
        void publishGpuScene(const GpuScenePackedTables&) override {}
        GpuSceneFrameSerials getGpuSceneFrameSerials() const noexcept override {
            return {};
        }
        GpuSceneUploadTelemetry getGpuSceneUploadTelemetry() const noexcept override {
            return {};
        }
        void prepareReflectionProbes(uint32_t,
            std::span<const EnvironmentLightingHandles>) override {}
        std::vector<ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() override { return {}; }
        std::optional<uint32_t> capturedReflectionProbeEnvironmentSlot(
            SceneEntityUuid) const noexcept override { return std::nullopt; }
        void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid>) override {}
        void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings&) override {}

        FrameStatus beginFrame() override { return FrameStatus::Ready; }
        void updateCamera(const ViewTransportRecord&, ViewHistoryContext) override {}
        void setDebugView(RenderDebugView) override {}
        void setOutputSettings(float, float, float) override {}
        void setViewportGridOverlay(const ViewportGridOverlay&) override {}

        void submitDirectionalShadows(const ShadowCasterSubmission&,
            std::span<const DirectionalShadowFramePacket>) override {}
        void submitSpotShadows(const ShadowCasterSubmission&,
            std::span<const SpotShadowFramePacket>) override {}
        void submitPointShadows(const ShadowCasterSubmission&,
            std::span<const PointShadowFramePacket>) override {}
        void submitReflectionProbeCaptures(const ReflectionProbeCasterSubmission&,
            std::span<const ReflectionProbeCaptureScheduleEntry>,
            const LightingFramePacket&) override {}
        ReflectionProbeCaptureTelemetry
            getReflectionProbeCaptureTelemetry() const noexcept override {
            return {};
        }
        uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission&) const noexcept override { return 0; }
        std::array<uint64_t, kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(const ShadowCasterSubmission&,
                const DirectionalShadowCascadePlan&) const noexcept override {
            return {};
        }

        void submitOpaqueQueue(std::span<const DrawPacket>,
            std::span<const DrawPacket>, bool) override {}
        void submitLightingPass(const glm::vec3&, const glm::mat4&,
            const glm::mat4&, float, float, const LightingFramePacket&,
            const ReflectionProbeGpuFramePacket&) override {}
        LightingUploadTelemetry getLightingUploadTelemetry() const noexcept override {
            return {};
        }
        ClusteredLightingTelemetry
            getClusteredLightingTelemetry() const noexcept override { return {}; }
        void submitForwardQueues(std::span<const DrawPacket>,
            std::span<const DrawPacket>, std::span<const DrawPacket>,
            std::span<const glm::mat4>) override {}

        void captureCurrentFrame(uint64_t captureId,
            FrameCapturePoint point) override {
            record(BackendCall::Kind::CaptureCurrentFrame, captureId,
                static_cast<uint32_t>(point));
        }
        std::vector<FrameCapture> collectFrameCaptures(bool) override {
            return {};
        }
        void requestOrdinary2CaptureValidation(uint64_t validationId) override {
            record(BackendCall::Kind::RequestOrdinary2Validation, validationId);
        }
        std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool) override { return {}; }
        void requestDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality) override {
            record(BackendCall::Kind::RequestDeepLayeredValidation, validationId,
                static_cast<uint32_t>(quality));
        }
        std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool) override { return {}; }
        void requestDepthPyramidCaptureValidation(uint64_t validationId) override {
            record(BackendCall::Kind::RequestDepthPyramidValidation, validationId);
        }
        std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool) override { return {}; }

        void submitOutputPass() override {}
        void submitUIPass() override {}
        void beginUI() override {}
        void* getLitSceneTextureID() override { return nullptr; }
        void* getGlassDepthTextureID() override { return nullptr; }
        void* getEditorTextureID(TextureHandle) override { return nullptr; }
        FrameStatus endFrame() override { return FrameStatus::Ready; }

        GeometryHandle allocateGeometry(const GeometryDesc&,
            std::span<const std::byte>, std::span<const std::byte>) override {
            return {};
        }
        void freeGeometry(GeometryHandle) override {}
        GeometryArenaAllocation allocateGeometryArena(uint32_t,
            std::span<const std::byte>, const GeometryArenaData&) override {
            return {};
        }
        void freeGeometryArena(std::span<const GeometryHandle>) override {}

        TextureHandle allocateTexture(const TextureDesc&,
            std::span<const std::byte>) override {
            const TextureHandle handle = TextureHandle::fromParts(nextTexture_++, 1u);
            record(BackendCall::Kind::AllocateTexture, handle.getIndex());
            return handle;
        }
        void freeTexture(TextureHandle handle) override {
            record(BackendCall::Kind::FreeTexture, handle.getIndex());
        }
        MaterialBinding allocateCanonicalMaterial(
            const CanonicalMaterialAsset&) override {
            const MaterialHandle handle =
                MaterialHandle::fromParts(nextMaterial_++, 1u);
            record(BackendCall::Kind::AllocateMaterial, handle.getIndex());
            return MaterialBinding{ .material = handle };
        }
        void updateCanonicalMaterial(MaterialHandle,
            const PackedGpuMaterial&) override {}
        void freeMaterial(MaterialHandle handle) override {
            record(BackendCall::Kind::FreeMaterial, handle.getIndex());
        }
        void setEnvironmentLighting(const EnvironmentLightingHandles&) override {}
        void setOutputTransformLut(TextureHandle) override {}

    private:
        void record(BackendCall::Kind kind, uint64_t value,
            uint32_t detail = 0) {
            calls.push_back({ kind, value, detail, marker });
        }

        uint32_t nextTexture_ = 1u;
        uint32_t nextMaterial_ = 1u;
    };

    // Records IAppControl requests; resizes always succeed.
    class FakeAppControl final : public IAppControl {
    public:
        std::vector<Color::OutputTransport> transports;
        std::vector<RenderExtent> resizes;
        RenderExtent extent{ 1280u, 720u };

        OutputTransportSwitchResult applyOutputTransport(
            Color::OutputTransport transport) override {
            transports.push_back(transport);
            return {};
        }
        bool resizeSceneExtent(RenderExtent requested, std::string&) override {
            resizes.push_back(requested);
            extent = requested;
            return true;
        }
        RenderExtent renderExtent() const override { return extent; }
        std::shared_ptr<ModelAsset> loadCookedStartupModel() override {
            return {};
        }
        void loadCookedStartupEnvironment() override {}
        void publishStartupEnvironment(LoadedEnvironmentAsset) override {}
    };

} // namespace IridiumTest
