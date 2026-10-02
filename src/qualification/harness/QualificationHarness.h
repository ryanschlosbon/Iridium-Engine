#pragma once

// M7R R2.6: the application-side qualification harness. It owns everything the
// Application used to do only for qualification runs: deterministic benchmark
// content and scene construction, validator preconditions, resize/lifecycle/
// transport/residency validators, per-frame capture and readback-validation
// requests, end-of-run IRIDIUM_* reports, the capture artifact and the CPU-profile
// run report. It is attached through IFrameObserver (app/FrameObserver.h).
//
// Backend capture/validation calls still go through IRenderBackend; R2.9 moves
// them to the Vulkan qualification extension.

#include "app/FrameObserver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmarks/BenchmarkManifest.h"
#include "core/types/FrameCapture.h"
#include "capture/CaptureArtifact.h"
#include "core/types/RenderHandles.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"

namespace Iridium {

    struct SystemProfile;

    // The run policy the harness requests for a configuration: a --benchmark run
    // is deterministic, fullscreen and owns its startup content.
    [[nodiscard]] AppRunPolicy qualificationRunPolicy(
        const ApplicationConfig& config);

    // Factory used by main.cpp. Calling it only from a discarded
    // `if constexpr (kQualificationBuild)` branch keeps an OFF build from
    // needing this library at link time.
    [[nodiscard]] std::unique_ptr<IFrameObserver> createQualificationHarness(
        const ApplicationConfig& config);

    class QualificationHarness final : public IFrameObserver {
    public:
        explicit QualificationHarness(const ApplicationConfig& config);

        [[nodiscard]] AppRunPolicy runPolicy() const override { return policy_; }
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
        void checkPreconditions(AppStartupContext& context);
        void recordTopologyBaselines(AppStartupContext& context);
        void constructBenchmarkScene(AppStartupContext& context);
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

        // Shutdown (QualificationReport.cpp, RunReportExport.cpp).
        void reportRunMetrics(const AppShutdownContext& context) const;
        void collectEndOfRunValidations(AppShutdownContext& context);
        void releaseProbeResources(AppShutdownContext& context);
        [[nodiscard]] std::optional<CaptureArtifactPaths> exportCaptureArtifact(
            const AppShutdownContext& context,
            const SystemProfile& systemProfile) const;
        void exportCpuProfile(const AppShutdownContext& context,
            const SystemProfile& systemProfile,
            const std::optional<CaptureArtifactPaths>& captureArtifact) const;

        AppRunPolicy policy_{};

        // Benchmark content.
        std::optional<BenchmarkFixture> benchmark_;
        std::string benchmarkManifestPath_;
        std::string benchmarkManifestSha256_;
        std::vector<BenchmarkInstanceState> benchmarkInstances_;
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

        // Captures.
        FrameRequests frame_{};
        std::optional<FrameCapture> completedCapture_;
        std::optional<uint64_t> capturedApplicationFrameIndex_;
    };

} // namespace Iridium
