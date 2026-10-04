#include "app/Application.h"
#include "renderer/rhi/TransparencyQualityOverride.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <bit>

#include "core/BuildInfo.h"
#include "profiling/CpuAllocationProfile.h"
#include "renderer/rhi/RenderBackendFactory.h"
#include "renderer/vulkan_imgui/VulkanImGuiEditorBridge.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/RenderInstanceBatchComponent.h"
#include "scene/components/LightComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/SkyComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/transparency/WeightedOit.h"
#include "utils/Sha256.h"
#include "renderer/color/AcesOutputLut.h"
#include "assets/AssetDiscovery.h"
#include "assets/SqliteAssetCatalog.h"
#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookKey.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorPreviewImageFit.h"
#include "assets/environment/EnvironmentProduct.h"
#include "editor/EditorSceneActions.h"
#include "platform/UserCacheDirectory.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace Iridium {

    namespace {
        // --pipeline-cache (M7R R4c.4): empty disables the cache.
        std::filesystem::path pipelineCacheDirectory(const ApplicationConfig& config) {
            if (!config.pipelineCacheEnabled) return {};
            if (!config.pipelineCacheDirectory.empty()) {
                std::error_code error;
                const std::filesystem::path absolute =
                    std::filesystem::absolute(config.pipelineCacheDirectory, error);
                return error ? config.pipelineCacheDirectory : absolute;
            }
            return userCacheDirectory("PipelineCache");
        }
    }

    Application::Application(ApplicationConfig config,
        IFrameObserver* observer)
        : config_(std::move(config)),
          cpuProfiler_(config_.enableCpuProfiling),
          observer_(observer),
          extractor_(cpuProfiler_, sceneWorld_, config_.shadowSettings,
              config_.reflectionProbeSettings),
          sceneDocumentService_(sceneWorld_),
          transactionService_(sceneDocumentService_),
          registry(sceneWorld_.registry()),
          assets_(config_, cpuProfiler_, engineLog_, sceneWorld_,
              sceneDocumentService_),
          editorHost_(cpuProfiler_) {}

    void Application::run() {
        policy_ = observer_ ? observer_->runPolicy() : AppRunPolicy{};
        try {
            const auto startupStart = std::chrono::steady_clock::now();
            const auto windowStart = std::chrono::steady_clock::now();
            initWindow();
            startupProfile_.windowNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - windowStart).count());
            initRenderer();
            startupProfile_.totalNanoseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - startupStart).count());
            mainLoop();
            notifyShutdown(ShutdownPhase::RunComplete, true);
        }
        catch (...) {
            cleanup(false);
            throw;
        }
        cleanup(true);
        notifyShutdown(ShutdownPhase::Finalize, true);
    }

    AppRunSnapshot Application::makeRunSnapshot() const {
        return AppRunSnapshot{
            .measurementStarted = measurementStarted_,
            .measuredFrameCount = measuredFrameCount_,
            .measurementWallNanoseconds = measurementWallNanoseconds_,
            .renderExtent = renderExtent_,
            .mainModel = assets_.mainModel().get(),
            .environment = assets_.environmentIdentity(),
            .directionalShadowSelection =
                extractor_.activeDirectionalShadowSelection(),
            .directionalShadowSampleableMask =
                extractor_.activeDirectionalShadowSampleableMask(),
            .directionalShadowOwnerCount =
                extractor_.activeDirectionalShadowOwnerCount(),
            .startup = startupProfile_,
            .debugView = editorHost_.debugView(),
        };
    }

    void Application::notifyStartup(StartupPhase phase,
        AppStartupContext& context) {
        if (observer_) observer_->onStartup(phase, context);
    }

    void Application::notifyShutdown(ShutdownPhase phase, bool completed) {
        if (!observer_) return;
        const AppRunSnapshot snapshot = makeRunSnapshot();
        AppShutdownContext context{
            .config = config_,
            .profiler = cpuProfiler_,
            .backend = renderBackend.get(),
            .completed = completed,
            .run = snapshot,
        };
        observer_->onShutdown(phase, context);
    }

    void Application::initWindow() {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
        glfwInitialized_ = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Tell GLFW not to create an OpenGL context
        glfwWindowHint(GLFW_VISIBLE, config_.windowVisible ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_DECORATED, config_.windowDecorated ? GLFW_TRUE : GLFW_FALSE);
        window = glfwCreateWindow(static_cast<int>(config_.windowWidth),
            static_cast<int>(config_.windowHeight), "Iridium Engine", nullptr, nullptr);
        if (window == nullptr) {
            throw std::runtime_error("Failed to create the Iridium window");
        }

        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    void Application::initRenderer() {
        AppStartupContext startup{
            .config = config_,
            .profiler = cpuProfiler_,
            .control = *this,
            .scene = sceneWorld_,
            .camera = camera_,
            .timings = startupProfile_,
        };
        notifyStartup(StartupPhase::Configure, startup);
        // 1. Instantiate the RHI (The Strategy Pattern in action)
        const auto backendStart = std::chrono::steady_clock::now();
        // The editor UI is a backend extension (M7R R3c.10), attached after
        // the qualification harness's own extensions (M7R R2.9).
        editorBridge_ = createVulkanImGuiEditorBridge(window);
        backendExtensions_.clear();
        if (observer_) {
            for (IRenderBackendExtension* extension : observer_->backendExtensions())
                backendExtensions_.push_back(extension);
        }
        backendExtensions_.push_back(&editorBridge_->backendExtension());
        renderBackend = createRenderBackend(RenderBackendCreateInfo{
            .api = RenderBackendApi::Vulkan,
            .extensions = backendExtensions_,
        });
        const AppRenderRouting& routing = policy_.routing;
        renderBackend->init(window, {
            .enableValidation = config_.enableValidation,
            .enableSynchronizationValidation =
                config_.enableSynchronizationValidation,
            .pipelineCacheDirectory = pipelineCacheDirectory(config_),
            .experimentalDepthPyramid = config_.experimentalDepthPyramid,
            .experimentalVirtualShadowResources =
                config_.experimentalVirtualShadowResources,
            .experimentalDepthOcclusionQuery =
                config_.experimentalDepthOcclusionQuery,
            .experimentalDepthOcclusionRejection =
                config_.experimentalDepthOcclusionRejection,
            .renderGraphAliasing = config_.renderGraphAliasing,
            .uploadQueue = config_.uploadQueue,
            .cpuProfiler = &cpuProfiler_,
            .enableGpuProfiling = config_.enableGpuProfiling,
            .enableTransparentPipelineStatistics =
                config_.enableTransparentPipelineStatistics,
            .forceDirectGBufferReference = routing.forceDirectGBufferReference,
            .forceDirectShadowReference = routing.forceDirectShadowReference,
            .experimentalShadowLodErrorTexels =
                config_.experimentalShadowLodErrorTexels,
            .shadowLodMaximumLevel = config_.shadowLodMaximumLevel,
            .experimentalGpuLodErrorPixels = config_.experimentalGpuLodErrorPixels,
            .gpuLodMaximumLevel = config_.gpuLodMaximumLevel,
            .gpuLodHysteresisFraction = config_.gpuLodHysteresisFraction,
            .experimentalProbeLodErrorPixels =
                config_.experimentalProbeLodErrorPixels,
            .probeLodMaximumLevel = config_.probeLodMaximumLevel,
            .weightedOitOrderSeed = routing.weightedOitOrderSeed,
            .gBufferLayout = config_.gBufferLayout,
            .clusterTileSize = config_.clusterTileSize,
            .clusterDepthSlices = config_.clusterDepthSlices,
            .directionalShadowResolution =
                config_.shadowSettings.directionalResolution,
            .spotShadowAtlasResolution =
                config_.shadowSettings.spotAtlasResolution,
            .pointShadowPool256Capacity =
                config_.shadowSettings.pointPool256Capacity,
            .pointShadowPool512Capacity =
                config_.shadowSettings.pointPool512Capacity,
            .pointShadowPool1024Capacity =
                config_.shadowSettings.pointPool1024Capacity,
            .reflectionProbeSettings = config_.reflectionProbeSettings,
            .manualExposureEv = config_.manualExposureEv,
            .outputOperator = config_.outputOperator,
			.outputTransport = config_.outputTransport,
            .paperWhiteNits = config_.paperWhiteNits,
            .peakNits = config_.peakNits,
        });
        renderExtent_ = renderBackend->getRenderExtent();
        renderCapabilities_ = renderBackend->getCapabilities();
        extractor_.attachBackend(*renderBackend, {
            .deterministicContent = policy_.deterministicContent,
            .forceDirectGBufferReference = routing.forceDirectGBufferReference,
            .forceDirectProbeCaptureReference =
                routing.forceDirectProbeCaptureReference,
        });
        startupProfile_.backendNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - backendStart).count());

        // Production runtime always executes classified transparency (ADR-0012);
        // serialized LegacyTwoBucket values remain readable but never select it.
        const TransparencyExecutionMode runtimeTransparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        AssetManager& assetManager = assets_.createAssetManager(
            *renderBackend, editorBridge_.get(),
            runtimeTransparencyExecutionMode,
            policy_.routing.gpuLodMinimumResidentLevel);
        extractor_.attachAssets(assetManager);
        // Qualification probe allocations made here shift texture and material
        // indices, so they precede every asset service.
        startup.backend = renderBackend.get();
        startup.assets = &assetManager;
        startup.capabilities = renderCapabilities_;
        startup.transparencyExecutionMode = runtimeTransparencyExecutionMode;
        startup.renderExtent = renderExtent_;
        notifyStartup(StartupPhase::BackendReady, startup);
        assets_.startServices(policy_.deterministicContent);

        const auto editorStart = std::chrono::steady_clock::now();
        editorHost_.init({
            .window = window,
            .bridge = editorBridge_.get(),
            .showProfiler = config_.showProfiler,
            .showMaterialDiagnostics = config_.showMaterialDiagnostics,
            .outputTransport = config_.outputTransport,
            .manualExposureEv = static_cast<float>(config_.manualExposureEv),
            .paperWhiteNits = static_cast<float>(config_.paperWhiteNits),
            .peakNits = static_cast<float>(config_.peakNits),
            .shadowSettings = &config_.shadowSettings,
            .reflectionProbeSettings = &config_.reflectionProbeSettings,
            .assets = {
                .manager = assets_.assetManager(),
                .catalog = assets_.catalog(),
                .catalogService = assets_.catalogService(),
                .modelPreparation = assets_.modelPreparation(),
                .environmentPreparation = assets_.environmentPreparation(),
                .thumbnails = assets_.thumbnails(),
                .runtime = assets_.runtime(),
                .mainModel = &assets_.mainModel(),
                .loadedEnvironments = &assets_.loadedEnvironments(),
            },
            .log = &engineLog_,
            .sceneDocuments = &sceneDocumentService_,
            .transactions = &transactionService_,
        });
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        publishOutputTransportStatus();
        // The backend starts from the configured output settings; each
        // frame's RenderFrame::output carries later changes (M7R R3c.11).
        editorHost_.setDebugView(config_.debugView);
        if (config_.editorAssetViewerGuid) {
            editorHost_.openConfiguredAssetViewer(
                *config_.editorAssetViewerGuid);
        }
        AssetGuid startupModelGuid;
        // Deterministic runs and hidden-window (automation) runs must not read or
        // overwrite the user's editor layout in imgui.ini.
        if (policy_.deterministicContent || !config_.windowVisible) {
            editorHost_.disableLayoutPersistence();
        }
        startupProfile_.editorNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - editorStart).count());

        replaceOutputTransformLut(
            renderRuntimeInfo_.effectiveOutputTransportMode);

        if (policy_.ownsStartupContent) {
            notifyStartup(StartupPhase::ContentLoad, startup);
            if (assets_.mainModel())
                startupModelGuid = assets_.mainModel()->assetGuid;
        }
        else {
            // Interactive startup is asset-independent. An explicit cooked model
            // still overrides the built-in primitive for diagnostics and captures,
            // while the cached cube remains available to editor create commands.
            startupModelGuid =
                assets_.loadInteractiveStartupContent(startupProfile_);
        }
        const std::shared_ptr<ModelAsset>& mainModel = assets_.mainModel();
        startup.mainModel = mainModel;
        startup.renderExtent = renderExtent_;
        notifyStartup(StartupPhase::ScenePrerequisites, startup);
        const FrameTopologyPreparation topologyPreparation =
            renderBackend->prepareFrameTopology({
                .refractionPyramids = mainModel &&
                    modelRequiresRefractionPyramids(*mainModel),
                .ordinary2LayeredInterfaces = mainModel &&
                    modelRequiresOrdinary2LayeredInterfaces(*mainModel),
                .hero4LayeredInterfaces = mainModel &&
                    modelRequiresHero4LayeredInterfaces(*mainModel),
                .cinematic8LayeredInterfaces = mainModel &&
                    modelRequiresCinematic8LayeredInterfaces(*mainModel),
                .weightedOit = mainModel &&
                    modelRequiresWeightedOit(*mainModel)
            });
        startupProfile_.frameTopologyPrewarmNanoseconds =
            topologyPreparation.durationNanoseconds;
        startup.topology = topologyPreparation;
        notifyStartup(StartupPhase::TopologyReady, startup);
        const auto sceneStart = std::chrono::steady_clock::now();
        if (assets_.environmentLighting().isValid())
            renderBackend->setEnvironmentLighting(
                assets_.environmentLighting());

        startup.startupModelGuid = startupModelGuid;
        if (!policy_.ownsStartupContent) {
            // Interactive startup scene: one model entity, then (after any
            // observer content) the editor's default sun and HDRI sky.
            const Entity entity = createModelEditorEntity(registry,
                startupModelGuid,
                config_.cookedModelArtifact.empty() ? "Cube" : "Model",
                glm::vec3(0.0f));
            auto& transform = registry.getComponent<TransformComponent>(entity);
            transform.rotation = glm::vec3(0.0f);
            transform.scale = glm::vec3(1.0f);
            transform.worldMatrix = glm::mat4(1.0f);
            transform.isDirty = true;

            auto& meshComp = registry.getComponent<MeshComponent>(entity);
            meshComp.model = mainModel;
            meshComp.assetGuid = startupModelGuid;
            if (!mainModel) {
                meshComp.requestedAssetGuid = startupModelGuid;
            }
            meshComp.enabled = true;
            editorHost_.setSelectedEntity(entity);
            startup.firstEntity = entity;
        }
        notifyStartup(StartupPhase::SceneConstruction, startup);
        if (startup.initialSelection) {
            editorHost_.setSelectedEntity(*startup.initialSelection);
        }
        if (!policy_.ownsStartupContent) {
            (void)createEditorEntityPreset(
                registry, EditorEntityPreset::DirectionalLight);
            const Entity skyEntity = createEditorEntityPreset(
                registry, EditorEntityPreset::HdriSky);
            auto& sky = registry.getComponent<SkyComponent>(skyEntity);
            const AssetGuid activeEnvironment =
                assets_.activeEnvironmentAssetGuid();
            if (!activeEnvironment.isNil()) {
                sky.hdri.environmentAssetGuid = activeEnvironment;
                sky.resolvedEnvironmentAssetGuid = activeEnvironment;
                sky.requestedEnvironmentAssetGuid = {};
            }
        }
        startup.environmentAssetGuid = assets_.activeEnvironmentAssetGuid();
        notifyStartup(StartupPhase::SceneComplete, startup);
        startupProfile_.sceneConstructionNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sceneStart).count());

        notifyStartup(StartupPhase::Ready, startup);

        // The command-line viewer path is used for deterministic captures and
        // profiling. Interactive opens stay fully asynchronous, but a bounded
        // command-line run must not begin its measured frames before the
        // requested cooked revision is resident.
        if (config_.editorAssetViewerGuid) {
            constexpr auto ViewerReadyTimeout = std::chrono::seconds(30);
            const auto deadline = std::chrono::steady_clock::now() +
                ViewerReadyTimeout;
            while (!editorHost_.resolveAssetPreview(renderExtent_,
                    measuredFrameCount_)) {
                assets_.processMeshSwaps();
                if (AssetRuntimeService* runtime = assets_.runtime()) {
                    (void)runtime->tick();
                    const EditorAssetDocument* document =
                        editorHost_.assetDocuments().active();
                    const auto snapshot = document
                        ? runtime->snapshot(
                            document->presentationAssetGuid)
                        : std::nullopt;
                    if (snapshot && snapshot->state ==
                            RuntimeAssetState::Failed) {
                        throw std::runtime_error(
                            "Configured asset viewer preparation failed: " +
                            snapshot->diagnostic);
                    }
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error(
                        "Timed out preparing configured asset viewer");
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
            }
        }
    }

    void Application::mainLoop() {
        float lastFrameTime = 0.0f;
        uint64_t applicationFrameCount = 0;
        std::chrono::steady_clock::time_point measurementStart{};
        bool measurementStarted = false;

        // --- Added for FPS Tracking ---
        int frameCount = 0;
        float timeAccumulator = 0.0f;

        AppFrameContext frame{
            .config = config_,
            .backend = *renderBackend,
            .profiler = cpuProfiler_,
            .scene = sceneWorld_,
            .camera = camera_,
            .control = *this,
            .requests = frameRequests_,
            .mainModel = assets_.mainModel(),
        };

        while (!glfwWindowShouldClose(window)) {
            const bool isMeasuredFrame = applicationFrameCount >= config_.warmupFrameCount;
            if (isMeasuredFrame && !measurementStarted) {
                measurementStart = std::chrono::steady_clock::now();
                measurementStarted = true;
                measurementStarted_ = true;
            }
            const bool profileFrame = isMeasuredFrame &&
                cpuProfiler_.beginFrame(applicationFrameCount + 1);
            if (profileFrame) {
                beginCpuAllocationFrame();
            }
            {
                CpuScope frameScope(cpuProfiler_, "cpu.frame.total");
                {
                    CpuScope eventScope(cpuProfiler_, "cpu.platform.events");
                    glfwPollEvents();
                }

                // 1. Time & Input
                float currentFrameTime = static_cast<float>(glfwGetTime());
                deltaTime = currentFrameTime - lastFrameTime;
                lastFrameTime = currentFrameTime;

                // --- FPS CALCULATION ---
                frameCount++;
                timeAccumulator += deltaTime;

                // Update the window title once every second
                if (timeAccumulator >= 1.0f) {
                    char title[96]{};
                    const float millisecondsPerFrame = frameCount != 0
                        ? 1000.0f / static_cast<float>(frameCount)
                        : 0.0f;
                    std::snprintf(title, sizeof(title),
                        "Iridium Engine - FPS: %u (%.2f ms/frame)",
                        frameCount, millisecondsPerFrame);
                    glfwSetWindowTitle(window, title);

                    frameCount = 0;
                    timeAccumulator -= 1.0f;
                }

                if (!policy_.deterministicContent) {
                    processInput(window);
                }

                frameRequests_ = {};
                frame.applicationFrameIndex = applicationFrameCount;
                frame.measuredFrameIndex = isMeasuredFrame
                    ? std::optional<uint64_t>(measuredFrameCount_)
                    : std::nullopt;
                frame.outputTransportPending = false;
                if (observer_) {
                    observer_->onFrameBegin(
                        FrameBeginPhase::PreSceneUpdate, frame);
                }

                // 2. Process delayed ECS events (like swapping meshes on the main thread)
                assets_.processMeshSwaps();

                assets_.tickRuntime();

                // 3. Update ECS Systems (Physics, Transforms, Animations)
                // This recalculates all local/world matrices before we extract them.
                {
                    CpuScope transformScope(cpuProfiler_, "cpu.scene.transforms");
                    changedTransformsThisFrame_ = transformSystem.update(
                        registry, &changedTransformEntities_);
                }

                // Observer scene-extent resizes happen here, between frames.
                if (observer_) {
                    observer_->onFrameBegin(
                        FrameBeginPhase::PostSceneUpdate, frame);
                }

                // 4. The frame acquisition must precede UI construction so the
                // viewport texture IDs correspond to the image acquired this frame.
                drawFrame(frame);
            }
            if (profileFrame) {
                const CpuAllocationFrameSample allocationSample =
                    endCpuAllocationFrame();
                cpuProfiler_.recordCounter("allocation.cpp.calls",
                    allocationSample.allocationCount);
                cpuProfiler_.recordCounter("allocation.cpp.bytes",
                    allocationSample.requestedBytes);
                (void)cpuProfiler_.endFrame();
            }

            ++applicationFrameCount;
            if (isMeasuredFrame) {
                ++measuredFrameCount_;
            }
            if (config_.frameLimit != 0 && measuredFrameCount_ >= config_.frameLimit) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        if (measurementStarted) {
            measurementWallNanoseconds_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - measurementStart).count());
        }
    }

    void Application::onRenderFrameStage(RenderFrameStage stage) {
        switch (stage) {
        case RenderFrameStage::DirectionalShadows:
        case RenderFrameStage::SpotShadows:
        case RenderFrameStage::PointShadows:
        case RenderFrameStage::ReflectionProbeCaptures:
        case RenderFrameStage::Lighting:
            extractor_.onRenderFrameStage(stage);
            break;
        case RenderFrameStage::SceneLinearComplete:
            // Scene-linear captures read the lit scene here.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::SceneLinearReady,
                    *stageFrame_);
            break;
        case RenderFrameStage::OutputComplete:
            // Final-output captures read the output target before the UI pass.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::OutputReady,
                    *stageFrame_);
            break;
        }
    }

    void Application::applyEditorFrameRequests(
        const EditorFrameRequests& requests) {
        if (requests.output) {
            const EditorOutputSettings& outputSettings = *requests.output;
            if (outputSettings.transport != config_.outputTransport) {
                config_.outputTransport = outputSettings.transport;
                pendingOutputTransport_ = outputSettings.transport;
            }
            config_.manualExposureEv = outputSettings.manualExposureEv;
            config_.paperWhiteNits = outputSettings.paperWhiteNits;
            config_.peakNits = outputSettings.peakNits;
        }
        if (requests.shadows) {
            ProjectShadowSettings shadowSettings = *requests.shadows;
            // Resolution is immutable for the active backend allocation;
            // every remaining project policy applies on the next frame.
            shadowSettings.directionalResolution =
                config_.shadowSettings.directionalResolution;
            shadowSettings.spotAtlasResolution =
                config_.shadowSettings.spotAtlasResolution;
            shadowSettings.pointPool256Capacity =
                config_.shadowSettings.pointPool256Capacity;
            shadowSettings.pointPool512Capacity =
                config_.shadowSettings.pointPool512Capacity;
            shadowSettings.pointPool1024Capacity =
                config_.shadowSettings.pointPool1024Capacity;
            config_.shadowSettings = shadowSettings;
        }
        if (requests.probes) {
            const ProjectReflectionProbeSettings& probeSettings =
                *requests.probes;
            config_.reflectionProbeSettings = probeSettings;
            extractor_.configureProbeCaptures(probeSettings);
            renderBackend->configureReflectionProbeCaptures(
                probeSettings);
        }
    }

    void Application::drawFrame(AppFrameContext& frame) {
        const uint64_t applicationFrameIndex = frame.applicationFrameIndex;
        const EditorViewSelection viewSelection =
            editorHost_.chooseView(policy_.fullscreenScenePresentation);
        const bool dualViews = viewSelection.dualViews;
        const uint32_t renderView = viewSelection.renderView;
        {
        CpuScope finalizeScope(cpuProfiler_, "cpu.probe.capture.finalize");
        for (const ReflectionProbeCaptureCompletion& completion :
                renderBackend->finalizeReflectionProbeCaptures()) {
            extractor_.markCapturePublished(completion);
            assets_.publishCaptureCompletion(completion);
        }
        }
        const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments =
            assets_.loadedEnvironments();
        // Light and probe extraction run before beginFrame; their packets stay
        // valid until the frame is released after submitFrame.
        SceneWorld* const previewLightingWorld =
            editorHost_.previewLightingWorld();
        extractor_.prepareLightsAndProbes(
            previewLightingWorld ? *previewLightingWorld : sceneWorld_,
            loadedEnvironments);
        assets_.processMaterialPreviews(editorHost_.assetDocuments());
        extractor_.prepareGpuScenePublication(editorHost_.selectedEntity());
        const GpuScenePackedTables* const gpuSceneFrame =
            extractor_.gpuSceneFrame();
        // Descriptor publication waits for old users and must precede acquisition.
        // Keep the authored scene environment identity separate from this binding.
        const EnvironmentLightingHandles desiredEnvironment =
            editorHost_.selectViewEnvironment(assets_.environmentLighting(),
                applicationFrameIndex);
        if (desiredEnvironment.isValid() && desiredEnvironment != renderBackend->getEnvironmentLighting())
            renderBackend->setEnvironmentLighting(desiredEnvironment);
        editorHost_.prepareRetainedViews(dualViews, renderView);
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            extractor_.releaseFrame();
            recreateSwapchain();
            return;
        }
        if (gpuSceneFrame) renderBackend->publishGpuScene(*gpuSceneFrame);
        const GpuSceneUploadTelemetry gpuSceneUpload =
            renderBackend->frameTelemetry().gpuSceneUpload;
        cpuProfiler_.recordCounter("gpu_scene.upload.bytes",
            gpuSceneUpload.bytes);
        cpuProfiler_.recordCounter("gpu_scene.upload.ranges",
            gpuSceneUpload.ranges);
        if (observer_) {
            observer_->onFrameBegin(FrameBeginPhase::BackendFrameOpened, frame);
        }

        // --- 1. CLEAR THE QUEUES ---
        extractor_.clearQueues();

        const float aspect = renderExtent_.height != 0
            ? static_cast<float>(renderExtent_.width) /
                static_cast<float>(renderExtent_.height)
            : 16.0f / 9.0f;
        // The previous-frame editor state that extraction reads before the
        // editor is built (design section 3.2, step 4).
        const EditorViewState preBuildView =
            editorHost_.viewState(aspect, renderExtent_, measuredFrameCount_);
        // --- 2. GET CAMERA DATA ---
        extractor_.beginView(preBuildView, {
            .position = camera_.position,
            .front = camera_.front,
            .up = camera_.up,
            .verticalFovDegrees = camera_.verticalFovDegrees,
            .nearPlane = camera_.nearPlane,
            .farPlane = camera_.farPlane,
        }, aspect);

        EditorFrameRequests editorRequests = editorHost_.build({
            .registry = &registry,
            .cameraPosition = camera_.position,
            .cameraFront = camera_.front,
            .cameraUp = camera_.up,
            .verticalFovDegrees = camera_.verticalFovDegrees,
            .nearPlane = camera_.nearPlane,
            .farPlane = camera_.farPlane,
            .aspect = aspect,
            .renderExtent = renderExtent_,
            .measuredFrameCount = measuredFrameCount_,
            .fullscreenScenePresentation = policy_.fullscreenScenePresentation,
            .colorValidationOverlay = policy_.colorValidationOverlay,
        });
        applyEditorFrameRequests(editorRequests);
        // The editor state after this frame's build: what extraction reads.
        const EditorViewState& view = editorRequests.view;
        extractor_.finalizeView(view, {
            .renderExtent = renderExtent_,
            .sceneEnvironmentSettings = assets_.sceneEnvironmentSettings(),
            .manualExposureEv = config_.manualExposureEv,
            .paperWhiteNits = config_.paperWhiteNits,
            .peakNits = config_.peakNits,
            .viewHistoryResetRevision = frameRequests_.viewHistoryResetRevision,
        });
        if (!frameRequests_.suppressGridOverlay && !view.assetPreviewActive) {
            extractor_.setGridOverlay(editorHost_.viewportGridOverlay(
                extractor_.viewMatrix(), extractor_.projectionMatrix()));
        }

        // --- 3-5. EXTRACTION, SORTING AND SCHEDULES ---
        const RenderFrame& renderFrame = extractor_.extract({
            .forceWireframe = config_.forceWireframe,
            .changedTransforms = changedTransformsThisFrame_,
            .environmentCookKey = &assets_.activeEnvironmentCookKey(),
            .applicationFrameIndex = applicationFrameIndex,
            .stageObserver = this,
        });
        stageFrame_ = &frame;


        // Shadows, probe captures, G-buffer, lighting, forward and
        // transparency, output and UI. The observer's scene-linear and
        // output submit points are reported from the stage boundaries.
        renderBackend->submitFrame(renderFrame);
        stageFrame_ = nullptr;
        extractor_.releaseFrame();

        if (renderBackend->endFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (dualViews) editorHost_.viewRendered(renderView);
        if (observer_) {
            const uint64_t switchesBefore = outputTransportSwitchCount_;
            frame.outputTransportPending = pendingOutputTransport_.has_value();
            observer_->onFrameEnd(frame);
            if (outputTransportSwitchCount_ != switchesBefore) return;
        }
        if (pendingOutputTransport_) {
            const Color::OutputTransport requested = *pendingOutputTransport_;
            pendingOutputTransport_.reset();
            (void)switchOutputTransport(requested);
            return;
        }
        if (!policy_.fullscreenScenePresentation && config_.windowVisible) {
            const RenderExtent requested =
                editorRequests.requestedSceneExtent;
            if (requested.width == 0 || requested.height == 0 ||
                (requested.width == renderExtent_.width &&
                    requested.height == renderExtent_.height)) {
                // The steady/minimized path performs no clock query, target
                // allocation, descriptor update, or graph rebuild.
                viewportExtentPolicy_.reset();
                return;
            }
            const uint64_t nowMilliseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (const auto replacement = viewportExtentPolicy_.observe(
                    requested, renderExtent_, nowMilliseconds)) {
                cpuProfiler_.recordCounter("viewport.resize.requests", 1);
                std::string diagnostic;
                const auto resizeStart = std::chrono::steady_clock::now();
                const bool resized = renderBackend->resizeSceneRenderExtent(
                    *replacement, diagnostic);
                const uint64_t resizeNanoseconds = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - resizeStart).count());
                cpuProfiler_.recordCounter(
                    "viewport.resize.cpu_ns", resizeNanoseconds);
                if (resized) {
                    renderExtent_ = renderBackend->getRenderExtent();
                    viewportExtentDiagnostic_.clear();
                    cpuProfiler_.recordCounter("viewport.resize.successes", 1);
                    cpuProfiler_.recordCounter("viewport.target.pixels",
                        static_cast<uint64_t>(renderExtent_.width) *
                            renderExtent_.height);
                    engineLog_.info("renderer.viewport",
                        "Scene target resized to " +
                            std::to_string(renderExtent_.width) + "x" +
                            std::to_string(renderExtent_.height));
                }
                else {
                    viewportExtentPolicy_.reportFailure(nowMilliseconds);
                    viewportExtentDiagnostic_ = diagnostic.empty()
                        ? "Scene target resize failed; retaining the previous target"
                        : std::move(diagnostic);
                    cpuProfiler_.recordCounter("viewport.resize.failures", 1);
                    engineLog_.warning(
                        "renderer.viewport", viewportExtentDiagnostic_);
                }
            }
        }
    }

    void Application::cleanup(bool completed) {
        notifyShutdown(ShutdownPhase::ReleaseResources, completed);
        editorHost_.cleanup();
        assets_.shutdown();

        if (renderBackend && outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
            outputTransformLut = {};
        }

        if (renderBackend) {
            renderBackend->cleanup();
            renderBackend.reset();
        }
        backendExtensions_.clear();
        editorBridge_.reset();

        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfwInitialized_) {
            glfwTerminate();
            glfwInitialized_ = false;
        }
    }

    // --- GLFW CALLBACK STUBS ---
    void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (app) app->framebufferResized = true;
    }

    void Application::mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        float xpos = static_cast<float>(xposIn);
        float ypos = static_cast<float>(yposIn);

        if (app->firstMouse) {
            app->lastX = xpos;
            app->lastY = ypos;
            app->firstMouse = false;
        }

        float xoffset = xpos - app->lastX;
        float yoffset = app->lastY - ypos;
        app->lastX = xpos;
        app->lastY = ypos;

        // Asset documents own their orbit controls through ImGui and never move
        // the active scene camera while being inspected.
        if (!app->editorHost_.assetViewerFocused() &&
            (app->editorHost_.sceneViewportHovered() || glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            xoffset *= app->mouseSensitivity;
            yoffset *= app->mouseSensitivity;

            // INVERSION FIX: Swap += to -= if your X or Y still feels backward!
            app->yaw += xoffset;
            app->pitch += yoffset;

            // Clamp pitch to prevent flipping upside down
            if (app->pitch > 89.0f)  app->pitch = 89.0f;
            if (app->pitch < -89.0f) app->pitch = -89.0f;

            glm::vec3 front;
            front.x = cos(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            front.y = sin(glm::radians(app->pitch));
            front.z = sin(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            app->camera_.front = glm::normalize(front);
        }
    }

    void Application::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;
        if (app->editorHost_.assetViewerFocused() || !app->editorHost_.sceneViewportHovered()) return;

        // Use scroll wheel to change camera fly speed
        app->cameraSpeed += static_cast<float>(yoffset) * 0.5f;
        if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
        if (app->cameraSpeed > 20.0f) app->cameraSpeed = 20.0f;
    }

    void Application::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        // Only activate camera look on Right Click
        if (button == GLFW_MOUSE_BUTTON_RIGHT &&
            ((!app->editorHost_.assetViewerFocused() && app->editorHost_.sceneViewportHovered()) || action == GLFW_RELEASE)) {
            if (action == GLFW_PRESS) {
                app->firstMouse = true; // Prevent violent camera snapping
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); // Hide cursor
            }
            else if (action == GLFW_RELEASE) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // Show cursor
            }
        }
    }
    void Application::processInput(GLFWwindow* window) {
        CpuScope inputScope(cpuProfiler_, "cpu.input");
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Only move camera if Right Mouse Button is held down (standard editor behavior)
        if (editorHost_.assetViewerFocused() && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (!editorHost_.assetViewerFocused() && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            float velocity = cameraSpeed * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                camera_.position += camera_.front * velocity;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                camera_.position -= camera_.front * velocity;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                camera_.position -= glm::normalize(glm::cross(camera_.front, camera_.up)) * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                camera_.position += glm::normalize(glm::cross(camera_.front, camera_.up)) * velocity;
        }
    }

    void Application::recreateSwapchain() {
        if (renderBackend) {
            renderBackend->recreateSwapchain(window);
            renderExtent_ = renderBackend->getRenderExtent();
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
            if (renderRuntimeInfo_.effectiveOutputTransportMode !=
                    outputTransformLutTransport_) {
                replaceOutputTransformLut(
                    renderRuntimeInfo_.effectiveOutputTransportMode);
            }
            publishOutputTransportStatus();
        }
    }

    void Application::replaceOutputTransformLut(
        Color::OutputTransport effectiveTransport) {
        if (!renderBackend) return;
        if (effectiveTransport == Color::OutputTransport::Automatic) {
            throw std::logic_error(
                "The backend returned an unresolved automatic output transport.");
        }
        const bool hdrTransport = effectiveTransport !=
            Color::OutputTransport::SdrSrgb;
        const bool currentHdrTransport = outputTransformLutTransport_ !=
            Color::OutputTransport::SdrSrgb;
        // scRGB and HDR10 intentionally share the same P3-D65 1000-nit ACES
        // transform; only their post-LUT encoding/composition differs.
        if (outputTransformLut.isValid() &&
            currentHdrTransport == hdrTransport) {
            outputTransformLutTransport_ = effectiveTransport;
            return;
        }
        const Color::AcesOutputLut outputLut = Color::loadAcesOutputLut(
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "color" /
            (hdrTransport ? "aces2_p3d65_1000nit_rec2100_pq_128.irlt" :
                "aces2_rec709_100nit_srgb_128.irlt"));
        const TextureDesc outputLutDesc{
            .width = outputLut.width(),
            .height = outputLut.height(),
            .format = TextureFormat::RGBA32_SFloat,
            .usageClass = TextureUsageClass::Sampled2D,
            .sampler = {
                .minFilter = FilterMode::Nearest,
                .magFilter = FilterMode::Nearest,
                .addressU = SamplerAddressMode::ClampToEdge,
                .addressV = SamplerAddressMode::ClampToEdge,
                .addressW = SamplerAddressMode::ClampToEdge,
            },
        };
        const TextureHandle replacement = renderBackend->allocateTexture(
            outputLutDesc, std::as_bytes(std::span(outputLut.rgba32f)));
        renderBackend->setOutputTransformLut(replacement);
        if (outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
        }
        outputTransformLut = replacement;
        outputTransformLutTransport_ = effectiveTransport;
    }

    void Application::publishOutputTransportStatus() {
        editorHost_.setOutputTransportStatus(
            renderRuntimeInfo_.requestedOutputTransportMode,
            renderRuntimeInfo_.effectiveOutputTransportMode,
            renderRuntimeInfo_.supportedOutputTransportModes,
            renderRuntimeInfo_.outputTransportDiagnostic);
    }

    OutputTransportSwitchResult Application::switchOutputTransport(
        Color::OutputTransport requested) {
        const auto switchStart = std::chrono::steady_clock::now();
        renderBackend->setOutputTransport(window, requested);
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        replaceOutputTransformLut(
            renderRuntimeInfo_.effectiveOutputTransportMode);
        publishOutputTransportStatus();
        const uint64_t switchNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - switchStart).count());
        ++outputTransportSwitchCount_;
        std::cout << "IRIDIUM_OUTPUT_TRANSPORT_SWITCH {\"requested\":\""
            << renderRuntimeInfo_.requestedOutputTransport
            << "\",\"effective\":\""
            << renderRuntimeInfo_.effectiveOutputTransport
            << "\",\"diagnostic\":\""
            << renderRuntimeInfo_.outputTransportDiagnostic
            << "\",\"duration_ns\":" << switchNanoseconds
            << "}\n" << std::flush;
        return OutputTransportSwitchResult{
            .requested = renderRuntimeInfo_.requestedOutputTransport,
            .effective = renderRuntimeInfo_.effectiveOutputTransport,
            .diagnostic = renderRuntimeInfo_.outputTransportDiagnostic,
            .durationNanoseconds = switchNanoseconds,
        };
    }

    OutputTransportSwitchResult Application::applyOutputTransport(
        Color::OutputTransport transport) {
        config_.outputTransport = transport;
        return switchOutputTransport(transport);
    }

    bool Application::resizeSceneExtent(RenderExtent requested,
        std::string& diagnostic) {
        const bool resized = renderBackend->resizeSceneRenderExtent(
            requested, diagnostic);
        if (resized) renderExtent_ = renderBackend->getRenderExtent();
        return resized;
    }

    std::shared_ptr<ModelAsset> Application::loadCookedStartupModel() {
        return assets_.loadCookedStartupModel();
    }

    void Application::loadCookedStartupEnvironment() {
        assets_.loadCookedStartupEnvironment();
    }

    void Application::publishStartupEnvironment(
        LoadedEnvironmentAsset environment) {
        assets_.publishStartupEnvironment(std::move(environment));
    }
} // namespace Iridium
