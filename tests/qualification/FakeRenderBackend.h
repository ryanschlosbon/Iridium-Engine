#pragma once

// A device-free IRenderBackend and IQualificationBackend for harness tests.
// Every call is a no-op except the ones the qualification harness makes, which
// are recorded in order.

#include "app/FrameObserver.h"
#include "assets/AssetManager.h"
#include "qualification/QualificationBackend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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
            ArmFrameCapture,
            ArmOrdinary2Validation,
            ArmDeepLayeredValidation,
            ArmDepthPyramidValidation,
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
        void prepareReflectionProbes(uint32_t,
            std::span<const EnvironmentLightingHandles>) override {}
        std::span<const ReflectionProbeCaptureCompletion>
            finalizeReflectionProbeCaptures() override { return {}; }
        std::optional<uint32_t> capturedReflectionProbeEnvironmentSlot(
            SceneEntityUuid) const noexcept override { return std::nullopt; }
        void synchronizeReflectionProbeCaptureOwners(
            std::span<const SceneEntityUuid>) override {}
        void configureReflectionProbeCaptures(
            const ProjectReflectionProbeSettings&) override {}

        FrameStatus beginFrame() override { return FrameStatus::Ready; }
        // Reports every stage boundary in order, like the Vulkan backend.
        void submitFrame(const RenderFrame& frame) override {
            if (frame.stageObserver == nullptr) return;
            for (const RenderFrameStage stage : { RenderFrameStage::DirectionalShadows,
                    RenderFrameStage::SpotShadows, RenderFrameStage::PointShadows }) {
                frame.stageObserver->onRenderFrameStage(stage);
            }
            if (frame.submitReflectionProbeCaptures)
                frame.stageObserver->onRenderFrameStage(
                    RenderFrameStage::ReflectionProbeCaptures);
            for (const RenderFrameStage stage : { RenderFrameStage::Lighting,
                    RenderFrameStage::SceneLinearComplete,
                    RenderFrameStage::OutputComplete }) {
                frame.stageObserver->onRenderFrameStage(stage);
            }
        }
        RenderFrameTelemetry frameTelemetry() const noexcept override { return {}; }
        uint64_t getShadowCasterRevision(
            const ShadowCasterSubmission&) override { return 0; }
        std::array<uint64_t, kDirectionalShadowCascadeCount>
            getDirectionalShadowCasterRevisions(const ShadowCasterSubmission&,
                const DirectionalShadowCascadePlan&) override {
            return {};
        }
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

        // Also used by FakeQualificationBackend, so render-backend and
        // qualification-backend calls share one ordered log.
        void record(BackendCall::Kind kind, uint64_t value,
            uint32_t detail = 0) {
            calls.push_back({ kind, value, detail, marker });
        }

    private:
        uint32_t nextTexture_ = 1u;
        uint32_t nextMaterial_ = 1u;
    };

    // A device-free IQualificationBackend: arm requests are recorded in the
    // FakeRenderBackend's log (with its marker); collections return nothing.
    class FakeQualificationBackend final : public IQualificationBackend {
    public:
        explicit FakeQualificationBackend(FakeRenderBackend& log) : log_(log) {}

        std::optional<QualificationBackendConfig> configured;

        IRenderBackendExtension& backendExtension() noexcept override {
            return extension_;
        }
        void configureQualification(
            const QualificationBackendConfig& config) override {
            configured = config;
        }
        void armFrameCapture(uint64_t captureId,
            FrameCapturePoint point) override {
            log_.record(BackendCall::Kind::ArmFrameCapture, captureId,
                static_cast<uint32_t>(point));
            if (produceCaptures) {
                pendingCaptures_.push_back({ captureId, 0u });
                maximumHeldCaptures = std::max(maximumHeldCaptures,
                    pendingCaptures_.size());
            }
        }
        void armOrdinary2CaptureValidation(uint64_t validationId) override {
            log_.record(BackendCall::Kind::ArmOrdinary2Validation, validationId);
        }
        void armDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality) override {
            log_.record(BackendCall::Kind::ArmDeepLayeredValidation, validationId,
                static_cast<uint32_t>(quality));
        }
        void armDepthPyramidCaptureValidation(uint64_t validationId) override {
            log_.record(BackendCall::Kind::ArmDepthPyramidValidation, validationId);
        }
        // With produceCaptures, each armed capture completes as a 2x2
        // scene-linear readback `captureLatency` collections after it was
        // armed (frames in flight), or at a waiting collection.
        std::vector<FrameCapture> collectFrameCaptures(bool waitForPending) override {
            std::vector<FrameCapture> completed;
            size_t index = 0;
            while (index < pendingCaptures_.size()) {
                PendingCapture& pending = pendingCaptures_[index];
                if (!waitForPending && ++pending.age < captureLatency) {
                    ++index;
                    continue;
                }
                completed.push_back(syntheticCapture(pending.captureId));
                pendingCaptures_.erase(pendingCaptures_.begin() +
                    static_cast<std::ptrdiff_t>(index));
            }
            // Slot order, not frame order, at a waiting collection.
            if (waitForPending) std::ranges::reverse(completed);
            return completed;
        }

        bool produceCaptures = false;
        uint32_t captureLatency = 2;
        // Most capture readbacks outstanding at once.
        size_t maximumHeldCaptures = 0;

        static FrameCapture syntheticCapture(uint64_t captureId) {
            FrameCapture capture{};
            capture.captureId = captureId;
            capture.width = 2;
            capture.height = 2;
            capture.rowPitchBytes = 2 * 4 * sizeof(float);
            capture.pixelFormat = FrameCapturePixelFormat::Rgba32Float;
            capture.colorDomain = FrameCaptureColorDomain::SceneLinearAcesCg;
            const float base = static_cast<float>(captureId);
            const std::array<float, 16> pixels{
                base, 0.0f, 0.5f, 1.0f, 1.0f, base, 0.25f, 1.0f,
                0.0f, 1.0f, base, 1.0f, 2.0f, 4.0f, 8.0f, 1.0f };
            capture.pixels.resize(sizeof(pixels));
            std::memcpy(capture.pixels.data(), pixels.data(), sizeof(pixels));
            return capture;
        }
        std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool) override { return {}; }
        std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool) override { return {}; }
        std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool) override { return {}; }
        uint32_t probeDrains = 0;
        bool drainReflectionProbeCaptures() override { ++probeDrains; return false; }

    private:
        class Extension final : public IRenderBackendExtension {
        public:
            RenderBackendApi api() const noexcept override {
                return RenderBackendApi::Vulkan;
            }
        };

        struct PendingCapture {
            uint64_t captureId = 0;
            uint32_t age = 0;
        };

        FakeRenderBackend& log_;
        Extension extension_;
        std::vector<PendingCapture> pendingCaptures_;
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
