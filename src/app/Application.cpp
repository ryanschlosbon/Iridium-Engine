#include "app/Application.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorSceneActions.h"
#include "platform/UserCacheDirectory.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderBackendFactory.h"
#include "renderer/vulkan_imgui/VulkanImGuiEditorBridge.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/SkyComponent.h"
#include "scene/components/TransformComponent.h"

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
          // Worker scope streams are prepared even while profiling is off, so
          // enabling the profiler later (editor) still sees worker scopes.
          tasks_(Tasks::TaskSystemConfig{ .profiler = &cpuProfiler_ }),
          observer_(observer),
          extractor_(cpuProfiler_, sceneWorld_, config_.shadowSettings,
              config_.reflectionProbeSettings, &tasks_),
          sceneDocumentService_(sceneWorld_),
          transactionService_(sceneDocumentService_),
          registry(sceneWorld_.registry()),
          assets_(config_, cpuProfiler_, engineLog_, tasks_, sceneWorld_,
              sceneDocumentService_),
          editorHost_(cpuProfiler_),
          orchestrator_({
              .config = config_,
              .profiler = cpuProfiler_,
              .tasks = tasks_,
              .log = engineLog_,
              .observer = observer_,
              .policy = policy_,
              .scene = sceneWorld_,
              .camera = camera_,
              .control = *this,
              .extractor = extractor_,
              .assets = assets_,
              .editor = editorHost_,
          }) {}

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
            orchestrator_.run();
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
            .measurementStarted = orchestrator_.measurementStarted(),
            .measuredFrameCount = orchestrator_.measuredFrameCount(),
            .measurementWallNanoseconds =
                orchestrator_.measurementWallNanoseconds(),
            .renderExtent = orchestrator_.renderExtent(),
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

        orchestrator_.installWindowCallbacks(window);
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
            .antiAliasing = config_.antiAliasing,
            .taaTuning = config_.taaTuning.value_or(TemporalAntiAliasingTuning{}),
            .exposureMode = config_.exposureMode,
            .autoExposure = config_.autoExposureSettings.value_or(AutoExposureSettings{}),
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
        orchestrator_.adoptBackend(*renderBackend);
        renderCapabilities_ = renderBackend->getCapabilities();
        extractor_.attachBackend(*renderBackend, {
            .deterministicContent = policy_.deterministicContent,
            .forceDirectGBufferReference = routing.forceDirectGBufferReference,
            .forceDirectProbeCaptureReference =
                routing.forceDirectProbeCaptureReference,
            .verifyGpuSceneObservations = routing.verifyGpuSceneObservations,
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
        startup.renderExtent = orchestrator_.renderExtent();
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
            .tasks = &tasks_,
        });
        orchestrator_.refreshOutputTransportStatus();
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

        orchestrator_.initializeOutputTransformLut();

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
        startup.renderExtent = orchestrator_.renderExtent();
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
            while (!editorHost_.resolveAssetPreview(
                    orchestrator_.renderExtent(),
                    orchestrator_.measuredFrameCount())) {
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

    void Application::cleanup(bool completed) {
        notifyShutdown(ShutdownPhase::ReleaseResources, completed);
        editorHost_.cleanup();
        assets_.shutdown();
        // Every service is gone: finish started work and join the workers.
        tasks_.shutdown();

        if (renderBackend) orchestrator_.releaseOutputTransformLut();

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

    OutputTransportSwitchResult Application::applyOutputTransport(
        Color::OutputTransport transport) {
        config_.outputTransport = transport;
        return orchestrator_.switchOutputTransport(transport);
    }

    bool Application::resizeSceneExtent(RenderExtent requested,
        std::string& diagnostic) {
        return orchestrator_.resizeSceneExtent(requested, diagnostic);
    }

    RenderExtent Application::renderExtent() const {
        return orchestrator_.renderExtent();
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
