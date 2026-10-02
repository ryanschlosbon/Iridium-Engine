#include "qualification/harness/QualificationHarness.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>

#include "assets/AssetManager.h"
#include "platform/SystemProfile.h"
#include "qualification/harness/HarnessDetail.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/TransparencyQualityOverride.h"

namespace Iridium {

    AppRunPolicy qualificationRunPolicy(const ApplicationConfig& config) {
        const bool benchmark = !config.benchmarkId.empty();
        return AppRunPolicy{
            .deterministicContent = benchmark,
            .fullscreenScenePresentation = benchmark,
            .colorValidationOverlay = benchmark &&
                config.benchmarkId == "color_volume_transparency_v1",
            .ownsStartupContent = benchmark,
        };
    }

    std::unique_ptr<IFrameObserver> createQualificationHarness(
        const ApplicationConfig& config) {
        return std::make_unique<QualificationHarness>(config);
    }

    QualificationHarness::QualificationHarness(const ApplicationConfig& config)
        : policy_(qualificationRunPolicy(config)) {}

    void QualificationHarness::onStartup(StartupPhase phase,
        AppStartupContext& context) {
        switch (phase) {
        case StartupPhase::Configure:
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
            return;
        }
    }

    void QualificationHarness::allocateTableScaleProbes(
        AppStartupContext& context) {
        const ApplicationConfig& config = context.config;
        IRenderBackend& backend = *context.backend;
        if (config.validateTextureTableScale != 0) {
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
                config.validateTextureTableScale);
            for (uint32_t index = 0;
                index < config.validateTextureTableScale; ++index) {
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
        if (config.validateMaterialTableScale != 0) {
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
                config.validateMaterialTableScale);
            for (uint32_t index = 0;
                index < config.validateMaterialTableScale; ++index) {
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
        if (config.captureFrameIndex) {
            if (!benchmark_ && !config.editorAssetViewerGuid) {
                throw std::invalid_argument(
                    "Deterministic frame capture requires --benchmark or "
                    "--open-asset-viewer.");
            }
            if (config.frameLimit != 0 &&
                *config.captureFrameIndex >= config.frameLimit) {
                throw std::invalid_argument(
                    "--capture-frame must be lower than the measured frame limit.");
            }
        }
        if (config.validateDepthPyramidCapture && config.frameLimit == 0u) {
            throw std::invalid_argument(
                "--validate-depth-pyramid-capture requires a bounded measured frame limit.");
        }
        if (config.validateDepthPyramidResize) {
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
            (config.validateOrdinary2Capture ? 1u : 0u) +
            (config.validateOrdinary2Fallback ? 1u : 0u) +
            (config.validateOrdinary2Resize ? 1u : 0u) +
            (config.validateWeightedOitResize ? 1u : 0u) +
            (config.validateDeepLayeredCapture ? 1u : 0u) +
            (config.validateDeepLayeredLifecycle ? 1u : 0u);
        if (layeredValidationModeCount > 1u) {
            throw std::invalid_argument(
                "Transparency capture, fallback, resize, and deep validations are mutually exclusive.");
        }
        if (config.validateOrdinary2Capture) {
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
        if (config.validateOrdinary2Fallback) {
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
        if (config.validateOrdinary2Resize) {
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
        if (config.validateWeightedOitResize) {
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
        if (config.validateDeepLayeredCapture) {
            if (config.frameLimit == 0u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a bounded measured frame limit.");
            }
            const bool hasRequestedTier = mainModel &&
                (config.deepLayeredCaptureQuality ==
                        TransparencyQuality::Hero4
                    ? modelRequiresHero4LayeredInterfaces(*mainModel)
                    : modelRequiresCinematic8LayeredInterfaces(*mainModel));
            if (!benchmark_ || !hasRequestedTier) {
                throw std::invalid_argument(
                    "--validate-deep-layered-capture requires a benchmark startup model with the selected deep layered quality.");
            }
        }
        if (config.validateDeepLayeredLifecycle) {
            if (config.frameLimit < 250u) {
                throw std::invalid_argument(
                    "--validate-deep-layered-lifecycle requires at least 250 measured frames.");
            }
            const bool hasRequestedTier = mainModel &&
                (config.deepLayeredCaptureQuality ==
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
        const ApplicationConfig& config = context.config;
        const FrameTopologyPreparation& topology = context.topology;
        std::cout << "IRIDIUM_FRAME_TOPOLOGY_PREWARM {\"requested\":"
            << (topology.requested ? "true" : "false")
            << ",\"changed\":"
            << (topology.changed ? "true" : "false")
            << ",\"duration_ns\":"
            << topology.durationNanoseconds << "}\n" << std::flush;
        if (config.validateOrdinary2Resize) {
            ordinary2ResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (config.validateWeightedOitResize) {
            weightedOitResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
        if (config.validateDepthPyramidResize) {
            depthPyramidResizeValidation_.initialRenderGraphRebuildCount =
                context.backend->getRuntimeInfo().renderGraphRebuildCount;
        }
    }

    void QualificationHarness::allocateResidencyChurnProbe(
        AppStartupContext& context) {
        const ApplicationConfig& config = context.config;
        if (!config.validateTextureResidencyChurn) return;
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
        const ApplicationConfig& config = context.config;
        switch (phase) {
        case FrameBeginPhase::PreSceneUpdate:
            updateBenchmarkState(context);
            return;
        case FrameBeginPhase::PostSceneUpdate: {
            const bool isMeasuredFrame = context.measuredFrameIndex.has_value();
            const uint64_t measured = context.measuredFrameIndex.value_or(0u);
            if (isMeasuredFrame && config.validateOrdinary2Resize) {
                updateOrdinary2ResizeValidation(context, measured);
            }
            if (isMeasuredFrame && config.validateWeightedOitResize) {
                updateWeightedOitResizeValidation(context, measured);
            }
            if (isMeasuredFrame && config.validateDepthPyramidResize) {
                updateDepthPyramidResizeValidation(context, measured);
            }
            const bool validateDeepLayeredLifecycle = isMeasuredFrame &&
                config.validateDeepLayeredLifecycle &&
                updateDeepLayeredLifecycleValidation(context, measured);

            frame_ = {};
            frame_.captureId = isMeasuredFrame &&
                    config.captureFrameIndex == measured
                ? config.captureFrameIndex
                : std::nullopt;
            frame_.ordinary2Validation = isMeasuredFrame &&
                ((config.validateOrdinary2Capture && measured == 0u) ||
                    (config.validateOrdinary2Resize && measured == 5u));
            frame_.deepLayeredValidation = isMeasuredFrame &&
                ((config.validateDeepLayeredCapture && measured == 0u) ||
                    validateDeepLayeredLifecycle);
            frame_.depthPyramidValidation = isMeasuredFrame &&
                config.validateDepthPyramidCapture && measured == 0u;
            context.requests.suppressGridOverlay =
                benchmark_.has_value() || frame_.captureId.has_value();
            return;
        }
        case FrameBeginPhase::BackendFrameOpened:
            updateTextureResidencyChurn(context);
            // Readback validations are consumed by the forward submission, so
            // arming them as soon as the frame is open is equivalent to arming
            // them right before it.
            if (frame_.ordinary2Validation) {
                context.backend.requestOrdinary2CaptureValidation(0u);
            }
            if (frame_.deepLayeredValidation) {
                context.backend.requestDeepLayeredCaptureValidation(
                    0u, config.deepLayeredCaptureQuality);
            }
            if (frame_.depthPyramidValidation) {
                context.backend.requestDepthPyramidCaptureValidation(0u);
            }
            return;
        }
    }

    void QualificationHarness::onFrameSubmit(FrameSubmitPoint point,
        AppFrameContext& context) {
        if (!frame_.captureId) return;
        const FrameCapturePoint capturePoint = context.config.capturePoint;
        const bool sceneLinear = capturePoint == FrameCapturePoint::SceneLinear;
        const bool finalOutput = capturePoint == FrameCapturePoint::FinalSdr ||
            capturePoint == FrameCapturePoint::FinalOutput;
        if ((point == FrameSubmitPoint::SceneLinearReady && sceneLinear) ||
            (point == FrameSubmitPoint::OutputReady && finalOutput)) {
            context.backend.captureCurrentFrame(*frame_.captureId,
                capturePoint);
            capturedApplicationFrameIndex_ = context.applicationFrameIndex;
        }
    }

    void QualificationHarness::onFrameEnd(AppFrameContext& context) {
        updateOutputTransportValidation(context);
    }

    void QualificationHarness::onShutdown(ShutdownPhase phase,
        AppShutdownContext& context) {
        switch (phase) {
        case ShutdownPhase::RunComplete:
            reportRunMetrics(context);
            collectEndOfRunValidations(context);
            return;
        case ShutdownPhase::ReleaseResources:
            releaseProbeResources(context);
            return;
        case ShutdownPhase::Finalize: {
            if (!completedCapture_ && context.config.cpuProfileOutput.empty())
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
