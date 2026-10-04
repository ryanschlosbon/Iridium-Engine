#include "qualification/harness/QualificationHarness.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "assets/AssetManager.h"
#include "platform/SystemProfile.h"
#include "profiling/CpuProfiler.h"
#include "qualification/harness/HarnessDetail.h"
#include "qualification/harness/StarvationLoad.h"
#include "qualification/vulkan/VulkanQualificationExtension.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/TransparencyQualityOverride.h"

namespace Iridium {

    AppRunPolicy qualificationRunPolicy(const QualificationOptions& options) {
        const bool benchmark = !options.benchmarkId.empty();
        return AppRunPolicy{
            .deterministicContent = benchmark,
            .fullscreenScenePresentation = benchmark,
            .colorValidationOverlay = benchmark &&
                options.benchmarkId == "color_volume_transparency_v1",
            .ownsStartupContent = benchmark,
            .routing = {
                .forceDirectGBufferReference =
                    options.forceDirectGBufferReference,
                .forceDirectShadowReference = options.forceDirectShadowReference,
                .forceDirectProbeCaptureReference =
                    options.forceDirectProbeCaptureReference,
                .weightedOitOrderSeed = options.weightedOitOrderSeed,
                .gpuLodMinimumResidentLevel = options.gpuLodMinimumResidentLevel,
                .verifyGpuSceneObservations = options.extractionVerifier,
            },
        };
    }

    QualificationBackendConfig qualificationBackendConfig(
        const QualificationOptions& options) {
        return QualificationBackendConfig{
            .shadowIndirectOracle = options.shadowIndirectQualificationOracle,
            .gpuLodOracle = options.gpuLodQualificationOracle,
            .probeLodOracle = options.probeLodQualificationOracle,
            .depthOcclusionOracle = options.depthOcclusionQualificationOracle,
            .virtualShadowDepthOracle =
                options.virtualShadowDepthQualificationOracle,
            .indirectStreamDigest = options.indirectStreamDigest,
            .casterRevisionOracle = options.casterRevisionOracle,
            .aliasPoison = options.aliasPoison,
            .validateProbeCaptureTargets = options.validateReflectionProbes,
        };
    }

    std::unique_ptr<IFrameObserver> createQualificationHarness(
        const QualificationOptions& options) {
        return std::make_unique<QualificationHarness>(options,
            std::make_unique<VulkanQualificationExtension>());
    }

    QualificationHarness::QualificationHarness(QualificationOptions options,
        std::unique_ptr<IQualificationBackend> backend)
        : options_(std::move(options)),
          policy_(qualificationRunPolicy(options_)),
          backend_(std::move(backend)) {
        if (!backend_)
            throw std::invalid_argument(
                "The qualification harness requires a backend extension");
        extensions_[0] = &backend_->backendExtension();
    }

    void QualificationHarness::onStartup(StartupPhase phase,
        AppStartupContext& context) {
        switch (phase) {
        case StartupPhase::Configure:
            // Oracles and graph hooks are fixed before the backend exists.
            backend_->configureQualification(
                qualificationBackendConfig(options_));
            loadScriptedChanges(context);
            return;
        case StartupPhase::BackendReady:
            capabilities_ = context.capabilities;
            std::cout << "IRIDIUM_TRANSPARENCY_EXECUTION {\"mode\":\""
                << transparencyExecutionModeName(
                    context.transparencyExecutionMode)
                << "\",\"developer_override\":false}\n";
            allocateTableScaleProbes(context);
            return;
        case StartupPhase::ContentLoad:
            loadBenchmarkContent(context);
            return;
        case StartupPhase::ScenePrerequisites:
            checkPreconditions(context);
            return;
        case StartupPhase::TopologyReady:
            recordTopologyBaselines(context);
            return;
        case StartupPhase::SceneConstruction:
            if (benchmark_) constructBenchmarkScene(context);
            constructGeneratedLights(context);
            return;
        case StartupPhase::SceneComplete:
            constructProbeValidationEntities(context);
            return;
        case StartupPhase::Ready:
            allocateResidencyChurnProbe(context);
            prepareScriptedChanges(context);
            startStarvationLoad(context);
            return;
        }
    }

    void StarvationLoadDeleter::operator()(BackgroundCookLoad* load) const noexcept {
        delete load;
    }

    void StarvationLoadDeleter::operator()(FrameTaskProbe* probe) const noexcept {
        delete probe;
    }

    void QualificationHarness::startStarvationLoad(AppStartupContext& context) {
        if (options_.backgroundCookSource.empty() && !options_.frameTaskProbe) {
            return;
        }
        // The engine's task system; the harness runs on its main thread.
        Tasks::TaskSystem* const tasks = Tasks::TaskSystem::forCurrentThread();
        if (tasks == nullptr) {
            throw std::runtime_error(
                "The starvation test needs the engine task system");
        }
        if (options_.frameTaskProbe) {
            const uint64_t frames = context.config.frameLimit != 0
                ? context.config.frameLimit : 100'000u;
            frameTaskProbe_.reset(new FrameTaskProbe(*tasks,
                static_cast<size_t>(frames)));
        }
        if (!options_.backgroundCookSource.empty()) {
            backgroundCook_.reset(new BackgroundCookLoad(*tasks,
                std::filesystem::path(PROJECT_ROOT_DIR) / "assets",
                options_.backgroundCookSource));
            backgroundCook_->start();
        }
    }

    void QualificationHarness::finishStarvationLoad() {
        if (backgroundCook_) {
            backgroundCook_->stop();
            std::cout << "IRIDIUM_BACKGROUND_COOK "
                << backgroundCook_->reportJson() << '\n';
            backgroundCook_.reset();
        }
        if (frameTaskProbe_) {
            std::cout << "IRIDIUM_FRAME_TASK_PROBE "
                << frameTaskProbe_->reportJson() << '\n';
            frameTaskProbe_.reset();
        }
    }

    void QualificationHarness::allocateTableScaleProbes(
        AppStartupContext& context) {
        IRenderBackend& backend = *context.backend;
        if (options_.validateTextureTableScale != 0) {
            constexpr std::array<std::byte, 4>
                texturePixel{
                    std::byte{ 0x3f },
                    std::byte{ 0x7f },
                    std::byte{ 0xbf },
                    std::byte{ 0xff },
                };
            const TextureDesc probe{
                .width = 1,
                .height = 1,
                .format = TextureFormat::RGBA8_UNorm,
            };
            textureScaleProbeTextures_.reserve(
                options_.validateTextureTableScale);
            for (uint32_t index = 0;
                index < options_.validateTextureTableScale; ++index) {
                textureScaleProbeTextures_.push_back(
                    backend.allocateTexture(probe, texturePixel));
            }
            std::cout
                << "IRIDIUM_TEXTURE_TABLE_SCALE "
                << "{\"resident_views\":"
                << textureScaleProbeTextures_.size()
                << ",\"resident_samplers\":"
                << textureScaleProbeTextures_.size()
                << ",\"indexed\":true}\n";
        }
        if (options_.validateMaterialTableScale != 0) {
            constexpr std::array<std::byte, 4>
                whitePixel{
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                    std::byte{ 0xff },
                };
            materialScaleProbeTexture_ = backend.allocateTexture(
                TextureDesc{
                    .width = 1,
                    .height = 1,
                    .format = TextureFormat::RGBA8_UNorm,
                },
                whitePixel);
            CanonicalMaterialAsset probe{};
            probe.name = "m3.7-material-scale-probe";
            probe.packed.closureClass = static_cast<uint32_t>(
                MaterialClosureClass::StandardDeferred);
            probe.packed.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed.metallicRoughnessIorSpecular = {
                0.0f, 1.0f, 1.5f, 1.0f };
            probe.packed.specularColorNormalScale = {
                1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed.diffuseFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed.specularGlossinessFactorGloss = {
                1.0f, 1.0f, 1.0f, 1.0f };
            probe.packed.emissiveFactorStrength = {
                0.0f, 0.0f, 0.0f, 1.0f };
            probe.packed.surfaceParameters = { 1.0f, 0.5f, 0.0f, 0.0f };
            probe.textures.fill(materialScaleProbeTexture_);
            probe.packed.textureIndices.fill(
                materialScaleProbeTexture_.getIndex());
            materialScaleProbeMaterials_.reserve(
                options_.validateMaterialTableScale);
            for (uint32_t index = 0;
                index < options_.validateMaterialTableScale; ++index) {
                materialScaleProbeMaterials_.push_back(
                    backend.allocateCanonicalMaterial(probe).material);
            }
            std::cout
                << "IRIDIUM_MATERIAL_TABLE_SCALE "
                << "{\"resident_records\":"
                << materialScaleProbeMaterials_.size()
                << ",\"indexed\":true}\n";
        }
    }

    void QualificationHarness::checkPreconditions(AppStartupContext& context) {
        const ApplicationConfig& config = context.config;
        const std::shared_ptr<ModelAsset>& mainModel = context.mainModel;
        const RenderExtent renderExtent = context.renderExtent;
        if (options_.captureFrameIndex) {
            if (!benchmark_ && !config.editorAssetViewerGuid) {
                throw std::invalid_argument(
                    "Deterministic frame capture requires --benchmark or "
                    "--open-asset-viewer.");
            }
            if (config.frameLimit != 0 &&
                *options_.captureFrameIndex >= config.frameLimit) {
                throw std::invalid_argument(
                    "--capture-frame must be lower than the measured frame limit.");
            }
        }
        if (options_.validateDepthPyramidCapture && config.frameLimit == 0u) {
            throw std::invalid_argument(
                "--validate-depth-pyramid-capture requires a bounded measured frame limit.");
        }
        if (options_.validateDepthPyramidResize) {
            if (config.frameLimit < 10u) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires at least ten measured frames.");
            }
            if (!benchmark_) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires a benchmark fixture.");
            }
            if (renderExtent.width != 1280u || renderExtent.height != 720u) {
                throw std::invalid_argument(
                    "--validate-depth-pyramid-resize requires the deterministic 1280x720 base extent.");
            }
            depthPyramidResizeValidation_.originalExtent = renderExtent;
        }
        const uint32_t layeredValidationModeCount =
            (options_.validateOrdinary2Capture ? 1u : 0u) +
            (options_.validateOrdinary2Fallback ? 1u : 0u) +
            (options_.validateOrdinary2Resize ? 1u : 0u) +
            (options_.validateWeightedOitResize ? 1u : 0u) +
            (options_.validateDeepLayeredCapture ? 1u : 0u) +
            (options_.validateDeepLayeredLifecycle ? 1u : 0u);
        if (layeredValidationModeCount > 1u) {
            throw std::invalid_argument(
                "Transparency capture, fallback, resize, and deep validations are mutually exclusive.");
        }
        if (options_.validateOrdinary2Capture) {
            if (config.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-capture requires a bounded measured frame limit.");
            }
            if (!mainModel ||
                !modelRequiresOrdinary2LayeredInterfaces(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-capture requires a startup model with Ordinary2 layered interfaces.");
            }
        }
        if (options_.validateOrdinary2Fallback) {
            if (config.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires a bounded measured frame limit.");
            }
            if (!mainModel) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires a startup model.");
            }
            const Ordinary2FallbackModelStats stats =
                ordinary2FallbackModelStats(*mainModel);
            if (stats.transparentSubmeshes == 0u ||
                stats.requestedLayeredCandidates !=
                    stats.transparentSubmeshes ||
                stats.fallbackThinGlassSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.fallbackFlaggedSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.topologyRequiredSubmeshes !=
                    stats.transparentSubmeshes ||
                stats.layeredGlassSubmeshes != 0u ||
                modelRequiresOrdinary2LayeredInterfaces(*mainModel) ||
                !modelRequiresRefractionPyramids(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-fallback requires only topology-rejected LayeredGlass candidates resolved to ThinGlass.");
            }
        }
        if (options_.validateOrdinary2Resize) {
            if (config.frameLimit < 6u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires at least six measured frames.");
            }
            if (!benchmark_ || !mainModel ||
                !modelRequiresOrdinary2LayeredInterfaces(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires a benchmark startup model with Ordinary2 layered interfaces.");
            }
            if (renderExtent.width != 1280u ||
                renderExtent.height != 720u) {
                throw std::invalid_argument(
                    "--validate-ordinary2-resize requires the deterministic 1280x720 base extent.");
            }
            ordinary2ResizeValidation_.originalExtent = renderExtent;
        }
        if (options_.validateWeightedOitResize) {
            if (config.frameLimit < 6u) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires at least six measured frames.");
            }
            if (!benchmark_ || !mainModel ||
                !modelRequiresWeightedOit(*mainModel)) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires a benchmark startup model with WeightedOIT work.");
            }
            if (renderExtent.width != 1280u ||
                renderExtent.height != 720u) {
                throw std::invalid_argument(
                    "--validate-weighted-oit-resize requires the deterministic 1280x720 base extent.");
            }
            weightedOitResizeValidation_.originalExtent = renderExtent;
        }
        if (options_.validateDeepLayeredCapture) {
            if (config.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a bounded measured frame limit.");
            }
            const bool hasRequestedTier = mainModel &&
                (options_.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? modelRequiresHero4LayeredInterfaces(*mainModel)
                    : modelRequiresCinematic8LayeredInterfaces(*mainModel));
            if (!benchmark_ || !hasRequestedTier) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a benchmark startup model with the selected deep layered quality.");
            }
        }
        if (options_.validateDeepLayeredLifecycle) {
            if (config.frameLimit < 250u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-lifecycle requires at least 250 measured frames.");
            }
            const bool hasRequestedTier = mainModel &&
                (options_.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? modelRequiresHero4LayeredInterfaces(*mainModel)
                    : modelRequiresCinematic8LayeredInterfaces(*mainModel));
            if (!benchmark_ || !hasRequestedTier) {
                throw std::invalid_argument(
                    "--validate-deep-layered-lifecycle requires a benchmark startup model with the selected deep layered quality.");
            }
        }
    }

    void QualificationHarness::recordTopologyBaselines(
        AppStartupContext& context) {
        const FrameTopologyPreparation& topology = context.topology;
        std::cout << "IRIDIUM_FRAME_TOPOLOGY_PREWARM {\"requested\":"
            << (topology.requested ? "true" : "false")
            << ",\"changed\":"
            << (topology.changed ? "true" : "false")
            << ",\"duration_ns\":"
            << topology.durationNanoseconds << "}\n" << std::flush;
        if (options_.validateOrdinary2Resize) {
            ordinary2ResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (options_.validateWeightedOitResize) {
            weightedOitResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (options_.validateDepthPyramidResize) {
            depthPyramidResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
    }

    void QualificationHarness::allocateResidencyChurnProbe(
        AppStartupContext& context) {
        const ApplicationConfig& config = context.config;
        if (!options_.validateTextureResidencyChurn) return;
        if (config.frameLimit != 0 &&
            config.warmupFrameCount + config.frameLimit < 3) {
            throw std::invalid_argument(
                "Texture residency churn validation requires at least three frames");
        }
        const TextureDesc probeDesc{
            .width = 512,
            .height = 512,
            .format = TextureFormat::RGBA8_UNorm,
            .usageClass = TextureUsageClass::Sampled2D,
        };
        constexpr size_t ProbeUploadBudgetBytes = 1u * 1024u * 1024u;
        residencyProbePixels_.resize(ProbeUploadBudgetBytes);
        for (size_t offset = 0; offset < ProbeUploadBudgetBytes;
            offset += 4) {
            residencyProbePixels_[offset] = std::byte{ 0x3f };
            residencyProbePixels_[offset + 1] = std::byte{ 0x7f };
            residencyProbePixels_[offset + 2] = std::byte{ 0xbf };
            residencyProbePixels_[offset + 3] = std::byte{ 0xff };
        }
        residencyProbeTexture_ = context.backend->allocateTexture(
            probeDesc, residencyProbePixels_);
    }

    void QualificationHarness::onFrameBegin(FrameBeginPhase phase,
        AppFrameContext& context) {
        switch (phase) {
        case FrameBeginPhase::PreSceneUpdate:
            updateScriptedChanges(context);
            updateBenchmarkState(context);
            return;
        case FrameBeginPhase::PostSceneUpdate: {
            const bool isMeasuredFrame = context.measuredFrameIndex.has_value();
            const uint64_t measured = context.measuredFrameIndex.value_or(0u);
            if (frameTaskProbe_) {
                CpuScope probeScope(context.profiler, "cpu.task.probe");
                frameTaskProbe_->runFrame(isMeasuredFrame);
            }
            if (isMeasuredFrame && options_.validateOrdinary2Resize) {
                updateOrdinary2ResizeValidation(context, measured);
            }
            if (isMeasuredFrame && options_.validateWeightedOitResize) {
                updateWeightedOitResizeValidation(context, measured);
            }
            if (isMeasuredFrame && options_.validateDepthPyramidResize) {
                updateDepthPyramidResizeValidation(context, measured);
            }
            const bool validateDeepLayeredLifecycle = isMeasuredFrame &&
                options_.validateDeepLayeredLifecycle &&
                updateDeepLayeredLifecycleValidation(context, measured);

            frame_ = {};
            frame_.captureId = isMeasuredFrame &&
                    options_.captureFrameIndex == measured
                ? options_.captureFrameIndex
                : std::nullopt;
            frame_.ordinary2Validation = isMeasuredFrame &&
                ((options_.validateOrdinary2Capture && measured == 0u) ||
                    (options_.validateOrdinary2Resize && measured == 5u));
            frame_.deepLayeredValidation = isMeasuredFrame &&
                ((options_.validateDeepLayeredCapture && measured == 0u) ||
                    validateDeepLayeredLifecycle);
            frame_.depthPyramidValidation = isMeasuredFrame &&
                options_.validateDepthPyramidCapture && measured == 0u;
            context.requests.suppressGridOverlay =
                benchmark_.has_value() || frame_.captureId.has_value();
            return;
        }
        case FrameBeginPhase::BackendFrameOpened:
            updateTextureResidencyChurn(context);
            // Every request is consumed by a backend hook later in this
            // frame's recording (validation readbacks during the forward
            // submission, the scene-linear capture when scene color is
            // complete, final-output captures after the output pass), so
            // arming them as soon as the frame is open is equivalent to the
            // pre-R2.9 calls made right before those points.
            if (frame_.ordinary2Validation) {
                backend_->armOrdinary2CaptureValidation(0u);
            }
            if (frame_.deepLayeredValidation) {
                backend_->armDeepLayeredCaptureValidation(
                    0u, options_.deepLayeredCaptureQuality);
            }
            if (frame_.depthPyramidValidation) {
                backend_->armDepthPyramidCaptureValidation(0u);
            }
            if (frame_.captureId) {
                backend_->armFrameCapture(*frame_.captureId,
                    options_.capturePoint);
                capturedApplicationFrameIndex_ = context.applicationFrameIndex;
            }
            return;
        }
    }

    void QualificationHarness::onFrameEnd(AppFrameContext& context) {
        updateOutputTransportValidation(context);
    }

    void QualificationHarness::onShutdown(ShutdownPhase phase,
        AppShutdownContext& context) {
        switch (phase) {
        case ShutdownPhase::RunComplete:
            finishStarvationLoad();
            finishScriptedChanges(context);
            reportRunMetrics(context);
            collectEndOfRunValidations(context);
            return;
        case ShutdownPhase::ReleaseResources:
            // A failed run skips RunComplete; the load stops before the
            // task system does.
            backgroundCook_.reset();
            frameTaskProbe_.reset();
            releaseScriptedChangeResources(context);
            releaseProbeResources(context);
            return;
        case ShutdownPhase::Finalize: {
            if (!completedCapture_ && options_.cpuProfileOutput.empty())
                return;
            const SystemProfile systemProfile = querySystemProfile();
            const std::optional<CaptureArtifactPaths> captureArtifact =
                exportCaptureArtifact(context, systemProfile);
            exportCpuProfile(context, systemProfile, captureArtifact);
            return;
        }
        }
    }

} // namespace Iridium
