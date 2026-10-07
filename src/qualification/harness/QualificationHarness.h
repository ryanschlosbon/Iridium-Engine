#pragma once

// M7R R2.6: the application-side qualification harness. It owns everything the
// Application used to do only for qualification runs: deterministic benchmark
// content and scene construction, validator preconditions, resize/lifecycle/
// transport/residency validators, per-frame capture and readback-validation
// requests, end-of-run IRIDIUM_* reports, the capture artifact and the CPU-profile
// run report. It is attached through IFrameObserver (app/FrameObserver.h) and
// supplies the backend extension that records captures and readbacks
// (IQualificationBackend, implemented by VulkanQualificationExtension).

#include "app/FrameObserver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "benchmarks/BenchmarkManifest.h"
#include "qualification/harness/AllocationTrace.h"
#include "core/types/FrameCapture.h"
#include "capture/CaptureArtifact.h"
#include "core/types/RenderHandles.h"
#include "qualification/QualificationBackend.h"
#include "qualification/QualificationOptions.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"

namespace Iridium {

    struct SystemProfile;
    struct ScriptedChangeRun;
    // Defined with ScriptedChangeRun (ScriptedChangeEvents.cpp).
    struct ScriptedChangeRunDeleter {
        void operator()(ScriptedChangeRun* run) const noexcept;
    };
    // M7R R5b.3 starvation test load and probe (StarvationLoad.h).
    class BackgroundCookLoad;
    class FrameTaskProbe;
    struct StarvationLoadDeleter {
        void operator()(BackgroundCookLoad* load) const noexcept;
        void operator()(FrameTaskProbe* probe) const noexcept;
    };

    // The run policy the harness requests: a --benchmark run is deterministic,
    // fullscreen and owns its startup content; reference routes, the OIT order
    // seed and the resident LOD floor reach the Application as routing.
    [[nodiscard]] AppRunPolicy qualificationRunPolicy(
        const QualificationOptions& options);
    // The oracle/startup-validator selection handed to the backend extension.
    [[nodiscard]] QualificationBackendConfig qualificationBackendConfig(
        const QualificationOptions& options);

    // Factory used by main.cpp (IRIDIUM_QUALIFICATION=ON builds only): the
    // harness with a VulkanQualificationExtension.
    [[nodiscard]] std::unique_ptr<IFrameObserver> createQualificationHarness(
        const QualificationOptions& options);

    class QualificationHarness final : public IFrameObserver {
    public:
        QualificationHarness(QualificationOptions options,
            std::unique_ptr<IQualificationBackend> backend);

        [[nodiscard]] AppRunPolicy runPolicy() const override { return policy_; }
        [[nodiscard]] std::span<IRenderBackendExtension* const>
            backendExtensions() override { return extensions_; }
        void onStartup(StartupPhase phase, AppStartupContext& context) override;
        void onFrameBegin(FrameBeginPhase phase,
            AppFrameContext& context) override;
        void onFrameSubmit(FrameSubmitPoint point,
            AppFrameContext& context) override;
        void onFrameEnd(AppFrameContext& context) override;
        void onShutdown(ShutdownPhase phase,
            AppShutdownContext& context) override;

    private:
        struct ResizeValidationState {
            RenderExtent originalExtent{};
            uint64_t initialRenderGraphRebuildCount = 0;
            uint32_t requests = 0;
            uint32_t successes = 0;
            uint32_t failures = 0;
            std::string lastDiagnostic;
        };
        struct DeepLayeredLifecycleValidationState {
            enum class Phase : uint8_t {
                Initial,
                WaitingForRetirement,
                WaitingForReactivation,
                Complete,
            };
            Phase phase = Phase::Initial;
            uint32_t retirements = 0;
            uint32_t reactivations = 0;
            uint32_t visibilityChanges = 0;
            uint64_t firstMeasuredFrame = 0;
            uint64_t completionMeasuredFrame = 0;
        };
        struct BenchmarkInstanceState {
            Entity entity = NULL_ENTITY;
            glm::vec3 basePosition{ 0.0f };
            // Composition fixtures: this frame's evaluated pose is a teleport
            // (BenchmarkEntityPose::teleported). Recorded for the M9
            // per-instance history reset; nothing consumes it yet.
            bool teleportedThisFrame = false;
        };
        // One armed frame capture: its frames and, once its frame was
        // submitted, that frame's jitter (M9 G6c).
        struct CaptureFrameRecord {
            uint64_t captureId = 0;
            uint64_t applicationFrameIndex = 0;
            std::optional<CaptureTemporalJitter> jitter;
        };
        // Requests decided at PostSceneUpdate and issued inside the open frame. A
        // frame lost to a swapchain recreate in beginFrame loses them, as before.
        struct FrameRequests {
            std::optional<uint64_t> captureId;
            bool ordinary2Validation = false;
            bool deepLayeredValidation = false;
            bool depthPyramidValidation = false;
        };

        // Startup (QualificationHarness.cpp, BenchmarkScene.cpp).
        void allocateTableScaleProbes(AppStartupContext& context);
        void loadBenchmarkContent(AppStartupContext& context);
        void loadBenchmarkModels(AppStartupContext& context,
            std::shared_ptr<ModelAsset> startupModel);
        void prepareBenchmarkModelTopology(AppStartupContext& context);
        [[nodiscard]] const std::shared_ptr<ModelAsset>& benchmarkSourceModel(
            const std::filesystem::path& sourceAsset,
            const AppStartupContext& context) const;
        void checkPreconditions(AppStartupContext& context);
        void recordTopologyBaselines(AppStartupContext& context);
        void constructBenchmarkScene(AppStartupContext& context);
        void constructBenchmarkComposition(AppStartupContext& context);
        void constructGeneratedLights(AppStartupContext& context);
        void constructProbeValidationEntities(AppStartupContext& context);
        void allocateResidencyChurnProbe(AppStartupContext& context);

        // Frames (FrameValidators.cpp, BenchmarkScene.cpp).
        void updateBenchmarkState(AppFrameContext& context);
        void updateOrdinary2ResizeValidation(AppFrameContext& context,
            uint64_t measuredFrameIndex);
        void updateWeightedOitResizeValidation(AppFrameContext& context,
            uint64_t measuredFrameIndex);
        void updateDepthPyramidResizeValidation(AppFrameContext& context,
            uint64_t measuredFrameIndex);
        [[nodiscard]] bool updateDeepLayeredLifecycleValidation(
            AppFrameContext& context, uint64_t measuredFrameIndex);
        void updateTextureResidencyChurn(AppFrameContext& context);
        void updateOutputTransportValidation(AppFrameContext& context);

        // Scripted mid-run changes (ScriptedChangeEvents.cpp). Every entry
        // point is a no-op without --qualification-scripted-changes.
        void startStarvationLoad(AppStartupContext& context);
        void finishStarvationLoad();

        void loadScriptedChanges(AppStartupContext& context);
        void prepareScriptedChanges(AppStartupContext& context);
        void updateScriptedChanges(AppFrameContext& context);
        void finishScriptedChanges(AppShutdownContext& context);
        void releaseScriptedChangeResources(AppShutdownContext& context);
        void appendScriptedChangeRecords(const AppShutdownContext& context) const;
        [[nodiscard]] uint64_t applyScriptedChange(size_t eventIndex,
            AppFrameContext& context);

        // Shutdown (QualificationReport.cpp, RunReportExport.cpp).
        void reportRunMetrics(const AppShutdownContext& context) const;
        void collectEndOfRunValidations(AppShutdownContext& context);
        void releaseProbeResources(AppShutdownContext& context);
        // Every artifact of the run, in capture order: the --capture-frame
        // capture, or the committed --capture-frames sequence.
        [[nodiscard]] std::vector<CaptureArtifactPaths> exportCaptureArtifacts(
            const AppShutdownContext& context,
            const SystemProfile& systemProfile);
        [[nodiscard]] CaptureArtifactMetadata runCaptureMetadata(
            const AppShutdownContext& context,
            const SystemProfile& systemProfile) const;
        // The naming fields of the artifact stem (shared by the streamed
        // images and their sidecars).
        void setCaptureNaming(CaptureArtifactMetadata& metadata,
            const ApplicationConfig& config, uint64_t measuredFrameIndex) const;
        void setCaptureFrame(CaptureArtifactMetadata& metadata,
            uint64_t captureId) const;
        void exportCpuProfile(const AppShutdownContext& context,
            const SystemProfile& systemProfile,
            const std::vector<CaptureArtifactPaths>& captureArtifacts) const;

        // Capture sequences (M9 G6c): write every completed readback's image
        // now (pixels are not retained); sidecars are committed at Finalize.
        void streamCompletedCaptures(const ApplicationConfig& config,
            bool waitForPending);
        void verifyCaptureSequence() const;
        void discardStreamedCaptures() noexcept;

        const QualificationOptions options_;
        AppRunPolicy policy_{};
        std::unique_ptr<BackgroundCookLoad, StarvationLoadDeleter> backgroundCook_;
        std::unique_ptr<FrameTaskProbe, StarvationLoadDeleter> frameTaskProbe_;
        // M7R R5c.8 --qualification-allocation-trace.
        std::unique_ptr<AllocationTrace> allocationTrace_;
        std::unique_ptr<IQualificationBackend> backend_;
        std::array<IRenderBackendExtension*, 1> extensions_{};

        // Benchmark content.
        std::optional<BenchmarkFixture> benchmark_;
        std::string benchmarkManifestPath_;
        std::string benchmarkManifestSha256_;
        std::vector<BenchmarkInstanceState> benchmarkInstances_;
        // M7C P1 multi-model fixtures: the model backing each fixture source
        // (benchmarkFixtureSourceAssets order; the first is the startup
        // model). Empty for single-model fixtures.
        std::vector<std::pair<std::filesystem::path, std::shared_ptr<ModelAsset>>>
            benchmarkSourceModels_;
        uint32_t generatedLightCount_ = 0;

        // Backend facts retained for the run report.
        RenderBackendCapabilities capabilities_{};
        RenderBackendRuntimeInfo runtimeInfo_{};

        // Validators.
        ResizeValidationState ordinary2ResizeValidation_;
        ResizeValidationState weightedOitResizeValidation_;
        ResizeValidationState depthPyramidResizeValidation_;
        DeepLayeredLifecycleValidationState deepLayeredLifecycleValidation_;
        uint32_t outputTransportValidationStep_ = 0;

        // Probe resources.
        TextureHandle residencyProbeTexture_{};
        TextureHandle residencyReplacementTexture_{};
        uint32_t residencyRetiredIndex_ = UINT32_MAX;
        std::vector<std::byte> residencyProbePixels_;
        std::vector<TextureHandle> textureScaleProbeTextures_;
        TextureHandle materialScaleProbeTexture_{};
        std::vector<MaterialHandle> materialScaleProbeMaterials_;

        // Probe validation entities (--validate-reflection-probes).
        Entity environmentProbeEntity_ = NULL_ENTITY;
        Entity captureProbeEntity_ = NULL_ENTITY;

        // Scripted mid-run changes; null without the flag.
        std::unique_ptr<ScriptedChangeRun, ScriptedChangeRunDeleter> scripted_;

        // Captures.
        FrameRequests frame_{};
        std::optional<FrameCapture> completedCapture_;
        std::vector<CaptureFrameRecord> captureFrames_;
        // --capture-frames: images written, sidecars pending, capture order.
        std::vector<PendingCaptureImage> streamedCaptures_;
    };

} // namespace Iridium
