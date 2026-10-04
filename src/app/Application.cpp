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
#include "imgui.h"
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
        template <typename T>
        uint64_t shadowRevision(const T& value) noexcept {
            uint64_t hash = 1469598103934665603ull;
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash == 0u ? 1u : hash;
        }

        template <typename T>
        void appendCaptureRevision(uint64_t& hash, const T& value) noexcept {
            const auto bytes = std::as_bytes(std::span{ &value, size_t{ 1 } });
            for (const std::byte byte : bytes) {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        }

        uint64_t reflectionProbeSettingsRevision(
            const ReflectionProbeCandidate& candidate) noexcept {
            uint64_t hash = 1469598103934665603ull;
            appendCaptureRevision(hash, candidate.probeToWorld);
            appendCaptureRevision(hash, candidate.probe.captureResolution);
            appendCaptureRevision(hash, candidate.probe.captureNearMeters);
            appendCaptureRevision(hash, candidate.probe.captureFarMeters);
            appendCaptureRevision(hash, candidate.probe.captureSky);
            appendCaptureRevision(hash, candidate.probe.updateMode);
            return hash == 0u ? 1u : hash;
        }

        uint64_t reflectionProbeLightingRevision(
            const LightingFramePacket& lights) noexcept {
            uint64_t hash = 1469598103934665603ull;
            for (uint64_t revision : lights.recordRevisions)
                appendCaptureRevision(hash, revision);
            appendCaptureRevision(hash, lights.activeListRevision);
            return hash == 0u ? 1u : hash;
        }

    }

    // M7R R5a.0 (design section 3.3): what the editor hands the frame. The
    // editor never writes runtime configuration, the backend or extraction
    // state; applyEditorFrameRequests performs those writes.
    struct EditorViewState {
        bool renderingAssetView = false;
        // An asset document is being rendered (renderingAssetView with an
        // active document); the preview fields are set only then.
        bool assetPreviewActive = false;
        std::shared_ptr<ModelAsset> previewModel;
        uint64_t previewSessionSerial = 0;
        uint64_t previewFramingRevision = 0;
        std::optional<AssetGuid> previewSelectedPart;
        bool previewSelectedPartIsMaterial = false;
        bool previewIsolateSelectedPart = false;
        AssetGuid previewHoveredPart;
        bool previewHoveredPartIsMaterial = false;
        RenderExtent previewRequestedExtent{};
        EnvironmentLightingSettings previewEnvironmentSettings{};
        float previewExposureEv = 0.0f;
        // The active preview's orbit camera (projection at the frame aspect).
        bool hasPreviewCamera = false;
        glm::vec3 previewCameraPosition{ 0.0f };
        glm::mat4 previewView{ 1.0f };
        glm::mat4 previewProjection{ 1.0f };
        float previewNearPlane = 0.0f;
        float previewFarPlane = 0.0f;
        float previewVerticalFovDegrees = 0.0f;
        // Null while an asset preview is rendered.
        Entity selectedEntity = NULL_ENTITY;
        RenderDebugView debugView = RenderDebugView::Final;
        // The rendered view's render mode is wireframe.
        bool wireframe = false;
        int layeredInterfaceOverride = 0;
    };

    struct EditorFrameRequests {
        std::optional<EditorOutputSettings> output;
        // Allocation fields (resolutions, pool capacities) are ignored on apply.
        std::optional<ProjectShadowSettings> shadows;
        // Also reconfigures the capture scheduler and the backend.
        std::optional<ProjectReflectionProbeSettings> probes;
        EditorViewState view;
        // Read after endFrame (the editor-driven scene resize).
        RenderExtent requestedSceneExtent{};
    };

    Application::Application(ApplicationConfig config,
        IFrameObserver* observer)
        : config_(std::move(config)),
          cpuProfiler_(config_.enableCpuProfiling),
          observer_(observer),
          reflectionProbeCaptureScheduler_({
              .maximumRenderedTexels = config_.reflectionProbeSettings.
                  maximumRenderedTexelsPerFrame,
              .maximumFacesPerProbePerFrame = config_.reflectionProbeSettings.
                  maximumFacesPerProbePerFrame,
              .maximumCapturesInFlight = config_.reflectionProbeSettings.
                  maximumCapturesInFlight,
              .minimumRealtimeFramesBetweenCaptures =
                  config_.reflectionProbeSettings.
                      minimumRealtimeFramesBetweenCaptures }),
          spotShadowAtlas_({
              .atlasResolution = config_.shadowSettings.spotAtlasResolution,
              .minimumTileResolution = 512,
              .guardTexels = 4 }),
          spotShadowCache_({
              .maximumRenderedTexels = config_.shadowSettings.
                  maximumSpotRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = config_.shadowSettings.
                  maximumCompatibleSpotStaleFrames }),
          pointShadowPools_({ .cubeCapacity = {
              config_.shadowSettings.pointPool256Capacity,
              config_.shadowSettings.pointPool512Capacity,
              config_.shadowSettings.pointPool1024Capacity } }),
          pointShadowCache_({
              .maximumRenderedTexels = config_.shadowSettings.
                  maximumPointRenderedTexelsPerFrame,
              .maximumCompatibleStaleFrames = config_.shadowSettings.
                  maximumCompatiblePointStaleFrames }),
          sceneDocumentService_(sceneWorld_),
          transactionService_(sceneDocumentService_),
          registry(sceneWorld_.registry()),
          assets_(config_, cpuProfiler_, engineLog_, sceneWorld_,
              sceneDocumentService_) {}

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
            .directionalShadowSelection = activeDirectionalShadowSelection_,
            .directionalShadowSampleableMask =
                activeDirectionalShadowSampleableMask_,
            .directionalShadowOwnerCount = activeDirectionalShadowOwnerCount_,
            .startup = startupProfile_,
            .debugView = editor.getDebugView(),
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
        gpuScenePublisher_ = std::make_unique<GpuScenePublisher>(
            GpuSceneCapacity{
                .maximumInstances = 262'144u,
                .maximumPrimitives = 1'048'576u,
                .maximumGeometries = 1'048'576u,
                .maximumTransforms = 524'288u,
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
        editor.init(window, &cpuProfiler_, config_.showProfiler,
            config_.showMaterialDiagnostics,
            config_.outputTransport, static_cast<float>(config_.manualExposureEv),
            static_cast<float>(config_.paperWhiteNits),
            static_cast<float>(config_.peakNits), config_.shadowSettings,
            config_.reflectionProbeSettings,
            assets_.catalog(),
            assets_.catalogService(),
            assets_.modelPreparation(),
            assets_.thumbnails(),
            assets_.runtime(),
            &engineLog_, &sceneDocumentService_, &transactionService_);
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        publishOutputTransportStatus();
        // The backend starts from the configured output settings; each
        // frame's RenderFrame::output carries later changes (M7R R3c.11).
        editor.setDebugView(config_.debugView);
        if (config_.editorAssetViewerGuid) {
            const std::vector<AssetCatalogRecord> records =
                assets_.catalog()->recordsForGuid(
                    *config_.editorAssetViewerGuid);
            const auto record = std::ranges::find_if(
                records,
                [](const AssetCatalogRecord& candidate) {
                    return candidate.status == AssetCatalogStatus::Ready &&
                        (candidate.assetType == "iridium.model" ||
                            candidate.assetType == "iridium.material");
                });
            if (record == records.end()) {
                throw std::runtime_error(
                    "--open-asset-viewer GUID is not a ready model or material asset");
            }
            const EditorAssetOpenResult opened =
                editor.assetDocuments().open({
                    .assetGuid = record->guid,
                    .parentAssetGuid = record->parentGuid,
                    .assetType = record->assetType,
                    .displayName = record->displayName,
                });
            if (!opened) {
                throw std::runtime_error(
                    "Could not open configured asset viewer: " +
                    opened.diagnostic);
            }
        }
        AssetGuid startupModelGuid;
        // Deterministic runs and hidden-window (automation) runs must not read or
        // overwrite the user's editor layout in imgui.ini.
        if (policy_.deterministicContent || !config_.windowVisible) {
            ImGui::GetIO().IniFilename = nullptr;
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
            editor.setSelectedEntity(entity);
            startup.firstEntity = entity;
        }
        notifyStartup(StartupPhase::SceneConstruction, startup);
        if (startup.initialSelection) {
            editor.setSelectedEntity(*startup.initialSelection);
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
            while (!resolveEditorAssetPreview()) {
                assets_.processMeshSwaps();
                if (AssetRuntimeService* runtime = assets_.runtime()) {
                    (void)runtime->tick();
                    const EditorAssetDocument* document =
                        editor.assetDocuments().active();
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

    void Application::prepareGpuScenePublication(Entity selectedEntity) {
        gpuSceneFrame_ = nullptr;
        gpuSceneDirectFallbackCount_ = 0;
        if (!gpuScenePublisher_) return;
        CpuScope publicationScope(cpuProfiler_, "cpu.gpu_scene.publish");
        size_t observationCount = 0;

        {
        CpuScope observationScope(cpuProfiler_, "cpu.gpu_scene.observe");
        auto* transformPool = registry.getPool<TransformComponent>();
        auto* meshPool = registry.getPool<MeshComponent>();
        auto* instanceBatchPool = registry.findPool<
            RenderInstanceBatchComponent>();
        if (transformPool && meshPool) {
            gpuSceneObservations_.reserve(meshPool->entities.size());
            gpuSceneObservationMetadata_.reserve(meshPool->entities.size());
            for (Entity entity : meshPool->entities) {
                const MeshComponent& mesh = meshPool->get(entity);
                if (!mesh.enabled || !mesh.model ||
                    !mesh.model->geometry.isValid() ||
                    !transformPool->has(entity)) continue;
                if (instanceBatchPool && instanceBatchPool->has(entity)) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const auto owner = sceneWorld_.identities().persistentId(entity);
                if (!owner || owner->isNil()) {
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                const ModelAsset& model = *mesh.model;
                if (observationCount == gpuSceneObservations_.size()) {
                    gpuSceneObservations_.emplace_back();
                    gpuSceneObservationMetadata_.emplace_back();
                }
                GpuSceneObservedInstance& observation =
                    gpuSceneObservations_[observationCount];
                GpuSceneObservationMetadata& metadata =
                    gpuSceneObservationMetadata_[observationCount];
                const glm::mat4 previousWorld = observation.worldTransform;
                const uint32_t previousFlags = observation.flags;
                const uint32_t previousMaximumLod = observation.maximumLod;
                const uint32_t previousConsumerMask =
                    observation.consumerMask;
                uint64_t overrideSignature = 1469598103934665603ull;
                const auto mix = [&overrideSignature](uint64_t value) {
                    overrideSignature ^= value;
                    overrideSignature *= 1099511628211ull;
                };
                for (const MeshComponent::MaterialOverride& value :
                        mesh.materialOverrides) {
                    for (uint8_t byte : value.sourceMaterialGuid.bytes()) mix(byte);
                    for (uint8_t byte : value.materialGuid.bytes()) mix(byte);
                    if (const auto resolved =
                            assets_.assetManager()->findCookedMaterialRuntime(
                                value.materialGuid)) {
                        mix(resolved->binding.material.id);
                        mix(resolved->binding.pipeline.id);
                        mix(static_cast<uint32_t>(
                            resolved->binding.renderQueue));
                    }
                }
                const bool rebuildPrimitives = metadata.owner != *owner ||
                    metadata.model != &model ||
                    metadata.geometry != model.geometry ||
                    metadata.cookKey != model.artifactCookKey ||
                    metadata.materialOverrideSignature != overrideSignature;
                observation.identity.owner = *owner;
                observation.identity.modelAssetGuid = model.assetGuid;
                observation.identity.publishedRevision = 1;
                observation.identity.artifactCookKey = model.artifactCookKey;
                observation.mobility = GpuSceneMobility::Movable;
                observation.flags = GpuSceneInstanceEnabled |
                    (entity == selectedEntity
                        ? GpuSceneInstanceSelected : 0u);
                observation.maximumLod = static_cast<uint32_t>(
                    std::clamp(mesh.maximumLodLevel, 0,
                        MeshComponent::MaximumLodLevel));
                observation.consumerMask = metadata.baseConsumerMask |
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                observation.worldTransform =
                    transformPool->get(entity).worldMatrix;
                if (rebuildPrimitives) {
                    metadata.owner = *owner;
                    metadata.model = &model;
                    metadata.geometry = model.geometry;
                    metadata.cookKey = model.artifactCookKey;
                    metadata.materialOverrideSignature = overrideSignature;
                    metadata.localMinimum = glm::vec3(
                        (std::numeric_limits<float>::max)());
                    metadata.localMaximum = glm::vec3(
                        (std::numeric_limits<float>::lowest)());
                    metadata.baseConsumerMask = 0;
                    metadata.valid = true;
                    observation.primitives.clear();
                    for (const SubMesh& subMesh : model.subMeshes) {
                    if (subMesh.materialIndex < 0 ||
                        static_cast<size_t>(subMesh.materialIndex) >=
                            model.materials.size()) continue;
                    const MaterialBinding* binding = &model.materials[
                        static_cast<size_t>(subMesh.materialIndex)];
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    const auto materialOverride = std::ranges::find_if(
                        mesh.materialOverrides,
                        [&subMesh](const MeshComponent::MaterialOverride& value) {
                            return value.sourceMaterialGuid == subMesh.materialGuid;
                        });
                    std::optional<CookedMaterialRuntimeBinding> overrideBinding;
                    if (materialOverride != mesh.materialOverrides.end()) {
                        overrideBinding = assets_.assetManager()->findCookedMaterialRuntime(
                            materialOverride->materialGuid);
                        if (overrideBinding) {
                            binding = &overrideBinding->binding;
                            effectiveMaterialGuid = materialOverride->materialGuid;
                        }
                    }
                    if (binding->renderQueue == RenderQueue::Transparent) continue;
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid() ||
                        subMesh.sourcePrimitiveGuid.isNil() ||
                        subMesh.primitiveGuid.isNil() ||
                        effectiveMaterialGuid.isNil() ||
                        subMesh.indexCount == 0) {
                        metadata.valid = false;
                        break;
                    }
                    uint32_t primitiveFlags = GpuScenePrimitiveOpaque;
                    if (subMesh.coverage == static_cast<uint8_t>(
                            ModelCoverage::Masked))
                        primitiveFlags |= GpuScenePrimitiveAlphaMask;
                    if ((subMesh.flags & ModelPrimitiveDoubleSided) != 0)
                        primitiveFlags |= GpuScenePrimitiveTwoSided;
                    metadata.baseConsumerMask |= binding->renderQueue ==
                        RenderQueue::ForwardOpaque
                        ? GpuSceneConsumerForwardOpaque
                        : GpuSceneConsumerMainOpaque;
                    metadata.baseConsumerMask |= GpuSceneConsumerShadow |
                        GpuSceneConsumerProbe;
                    observation.primitives.push_back({
                        .identity = { *owner, subMesh.sourcePrimitiveGuid,
                            subMesh.primitiveGuid, effectiveMaterialGuid },
                        .geometryIdentity = { model.assetGuid,
                            subMesh.sourcePrimitiveGuid,
                            subMesh.primitiveGuid },
                        .legacyGeometry = subMesh.geometry.isValid()
                            ? subMesh.geometry : model.geometry,
                        .material = binding->material,
                        .pipeline = binding->pipeline,
                        .firstIndex = subMesh.indexStart,
                        .indexCount = subMesh.indexCount,
                        .vertexOffset = subMesh.vertexOffset,
                        .indexType = subMesh.indexFormat,
                        .vertexLayout = subMesh.attributeMask,
                        .primitiveFlags = primitiveFlags,
                        .consumerMask = binding->renderQueue ==
                            RenderQueue::ForwardOpaque
                            ? GpuSceneConsumerForwardOpaque |
                                GpuSceneConsumerShadow |
                                GpuSceneConsumerProbe
                            : GpuSceneConsumerMainOpaque |
                                GpuSceneConsumerShadow |
                                GpuSceneConsumerProbe,
                        .localBoundsSphere = glm::vec4(
                            subMesh.boundsSphereCenter,
                            subMesh.boundsSphereRadius),
                        .localBoundsMin = glm::vec4(subMesh.boundsMin, 0.0f),
                        .localBoundsMax = glm::vec4(subMesh.boundsMax, 0.0f),
                        .geometryProductRevision = static_cast<uint32_t>(
                            observation.identity.publishedRevision),
                        .materialRevision = binding->material.id,
                    });
                    const auto lodChain = std::ranges::find(model.lodChains,
                        subMesh.primitiveGuid, &ModelLodChain::basePrimitiveGuid);
                    if (lodChain != model.lodChains.end()) {
                        auto& children = observation.primitives.back().lodChildren;
                        for (size_t levelIndex = 1; levelIndex < lodChain->levels.size(); ++levelIndex) {
                            const auto& level = lodChain->levels[levelIndex];
                            const SubMesh& child = level.subMesh;
                            children.push_back({
                                .identity = { model.assetGuid, child.sourcePrimitiveGuid, child.primitiveGuid },
                                .geometry = child.geometry,
                                .firstIndex = child.indexStart, .indexCount = child.indexCount,
                                .vertexOffset = child.vertexOffset,
                                .indexType = child.indexFormat, .vertexLayout = child.attributeMask,
                                .localBoundsSphere = glm::vec4(child.boundsSphereCenter, child.boundsSphereRadius),
                                .localBoundsMin = glm::vec4(child.boundsMin, 0.0f),
                                .localBoundsMax = glm::vec4(child.boundsMax, 0.0f),
                                .geometricError = level.geometricError,
                            });
                        }
                    }
                    metadata.localMinimum = glm::min(
                        metadata.localMinimum, subMesh.boundsMin);
                    metadata.localMaximum = glm::max(
                        metadata.localMaximum, subMesh.boundsMax);
                    }
                    metadata.valid = metadata.valid &&
                        !observation.primitives.empty();
                }
                observation.consumerMask = metadata.baseConsumerMask |
                    (entity == selectedEntity
                        ? GpuSceneConsumerSelection : 0u);
                if (!metadata.valid) {
                    observation.primitives.clear();
                    ++gpuSceneDirectFallbackCount_;
                    continue;
                }
                glm::vec3 worldMinimum((std::numeric_limits<float>::max)());
                glm::vec3 worldMaximum((std::numeric_limits<float>::lowest)());
                for (uint32_t corner = 0; corner < 8u; ++corner) {
                    const glm::vec3 local{
                        (corner & 1u) ? metadata.localMaximum.x
                            : metadata.localMinimum.x,
                        (corner & 2u) ? metadata.localMaximum.y
                            : metadata.localMinimum.y,
                        (corner & 4u) ? metadata.localMaximum.z
                            : metadata.localMinimum.z,
                    };
                    const glm::vec3 world = glm::vec3(
                        observation.worldTransform * glm::vec4(local, 1.0f));
                    worldMinimum = glm::min(worldMinimum, world);
                    worldMaximum = glm::max(worldMaximum, world);
                }
                const glm::vec3 center = (worldMinimum + worldMaximum) * 0.5f;
                observation.worldBoundsSphere = glm::vec4(center,
                    glm::length(worldMaximum - center));
                observation.worldBoundsMin = glm::vec4(worldMinimum, 0.0f);
                observation.worldBoundsMax = glm::vec4(worldMaximum, 0.0f);
                const bool observationChanged = rebuildPrimitives ||
                    std::memcmp(&previousWorld, &observation.worldTransform,
                        sizeof(previousWorld)) != 0 ||
                    previousFlags != observation.flags ||
                    previousMaximumLod != observation.maximumLod ||
                    previousConsumerMask != observation.consumerMask;
                if (observationChanged) {
                    if (metadata.observationRevision ==
                            (std::numeric_limits<uint64_t>::max)()) {
                        throw std::overflow_error(
                            "GPU-scene observation revision exhausted");
                    }
                    ++metadata.observationRevision;
                }
                observation.observationRevision =
                    metadata.observationRevision;
                ++observationCount;
            }
        }
        gpuSceneObservations_.resize(observationCount);
        gpuSceneObservationMetadata_.resize(observationCount);
        }

        const GpuSceneFrameSerials serials =
            renderBackend->getGpuSceneFrameSerials();
        {
            CpuScope synchronizeScope(
                cpuProfiler_, "cpu.gpu_scene.synchronize");
            gpuSceneFrame_ = &gpuScenePublisher_->synchronize(
                sceneWorld_.stateEpoch(), gpuSceneObservations_,
                serials.lastSubmitted, serials.completed);
        }
        {
            CpuScope capacityScope(cpuProfiler_, "cpu.gpu_scene.prepare");
            renderBackend->prepareGpuScene({
                static_cast<uint32_t>(gpuSceneFrame_->transforms.size()),
                static_cast<uint32_t>(gpuSceneFrame_->instances.size()),
                static_cast<uint32_t>(gpuSceneFrame_->primitives.size()),
                static_cast<uint32_t>(gpuSceneFrame_->geometries.size()),
            });
        }
        const GpuScenePublisherStats& stats = gpuScenePublisher_->stats();
        cpuProfiler_.recordCounter("gpu_scene.instance.active",
            stats.activeInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.active",
            stats.activePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.active",
            stats.activeGeometries);
        cpuProfiler_.recordCounter("gpu_scene.transform.changed",
            stats.changedTransforms);
        cpuProfiler_.recordCounter("gpu_scene.instance.changed",
            stats.changedInstances);
        cpuProfiler_.recordCounter("gpu_scene.primitive.changed",
            stats.changedPrimitives);
        cpuProfiler_.recordCounter("gpu_scene.geometry.changed",
            stats.changedGeometries);
        cpuProfiler_.recordCounter("gpu_scene.publication.unchanged_fast_path",
            stats.unchangedFastPath);
        cpuProfiler_.recordCounter("gpu_scene.direct_fallback",
            gpuSceneDirectFallbackCount_ + stats.capacityFallbackInstances);
    }

    void Application::onRenderFrameStage(RenderFrameStage stage) {
        switch (stage) {
        case RenderFrameStage::DirectionalShadows:
            for (const DirectionalShadowFramePacket& shadow : frameStage_.directionalShadows)
                directionalShadowCaches_[shadow.shadowIndex].markRendered(
                    shadow.updateMask);
            break;
        case RenderFrameStage::SpotShadows: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.spotSchedule->stats;
            spotShadowCache_.markScheduledRendered();
            const LocalShadowAllocationStats& allocation = frameStage_.spotAllocation;
            cpuProfiler_.recordCounter("shadow.spot.requested", allocation.requested);
            cpuProfiler_.recordCounter("shadow.spot.allocated", allocation.allocated);
            cpuProfiler_.recordCounter("shadow.spot.omitted", allocation.omitted);
            cpuProfiler_.recordCounter("shadow.spot.cache_hits", stats.cacheHits);
            cpuProfiler_.recordCounter("shadow.spot.updates", stats.updates);
            cpuProfiler_.recordCounter("shadow.spot.stale_sampled",
                stats.staleSampled);
            cpuProfiler_.recordCounter("shadow.spot.unshadowed", stats.unshadowed);
            cpuProfiler_.recordCounter("shadow.spot.rendered_texels",
                stats.renderedTexels);
            break;
        }
        case RenderFrameStage::PointShadows: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.pointSchedule->stats;
            pointShadowCache_.markScheduledRendered();
            const LocalShadowAllocationStats& allocation = frameStage_.pointAllocation;
            cpuProfiler_.recordCounter("shadow.point.requested", allocation.requested);
            cpuProfiler_.recordCounter("shadow.point.allocated", allocation.allocated);
            cpuProfiler_.recordCounter("shadow.point.omitted", allocation.omitted);
            cpuProfiler_.recordCounter("shadow.point.cache_hits", stats.cacheHits);
            cpuProfiler_.recordCounter("shadow.point.updates", stats.updates);
            cpuProfiler_.recordCounter("shadow.point.stale_sampled",
                stats.staleSampled);
            cpuProfiler_.recordCounter("shadow.point.unshadowed", stats.unshadowed);
            cpuProfiler_.recordCounter("shadow.point.rendered_texels",
                stats.renderedTexels);
            break;
        }
        case RenderFrameStage::ReflectionProbeCaptures: {
            // Completing the schedule retires it; its stats are read first.
            const auto stats = frameStage_.probeCaptureSchedule->stats;
            reflectionProbeCaptureScheduler_.markScheduledFacesRendered();
            const ReflectionProbeCaptureTelemetry telemetry =
                renderBackend->frameTelemetry().probeCaptures;
            cpuProfiler_.recordCounter("probe.capture.faces_scheduled",
                stats.facesScheduled);
            cpuProfiler_.recordCounter("probe.capture.budget_deferred",
                stats.budgetDeferred);
            cpuProfiler_.recordCounter("probe.capture.capacity_deferred",
                stats.capacityDeferred);
            cpuProfiler_.recordCounter("probe.capture.cadence_deferred",
                stats.cadenceDeferred);
            cpuProfiler_.recordCounter("probe.capture.faces_rendered",
                telemetry.facesRendered);
            cpuProfiler_.recordCounter("probe.capture.filtered",
                telemetry.capturesFiltered);
            cpuProfiler_.recordCounter("probe.capture.published",
                telemetry.capturesPublished);
            cpuProfiler_.recordCounter("probe.capture.staging_bytes",
                telemetry.stagingLogicalBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("probe.capture.published_bytes",
                telemetry.publishedLogicalBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            break;
        }
        case RenderFrameStage::Lighting: {
            const RenderFrameTelemetry telemetry = renderBackend->frameTelemetry();
            const LightingUploadTelemetry& lightUpload = telemetry.lightUploads;
            cpuProfiler_.recordCounter("light.gpu_upload_bytes", lightUpload.bytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("light.gpu_upload_ranges", lightUpload.ranges);
            const ClusteredLightingTelemetry& clusters = telemetry.clusters;
            const ProfileCounterStatus clusterStatus = clusters.available
                ? ProfileCounterStatus::Exact : ProfileCounterStatus::Unavailable;
            cpuProfiler_.recordCounter("cluster.buffer_bytes_per_frame",
                clusters.bufferBytesPerFrame, clusterStatus, ProfileCounterUnit::Bytes);
            cpuProfiler_.recordCounter("cluster.count", clusters.clusterCount, clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.active", clusters.activeLights,
                clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.directional",
                clusters.directionalLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.lights.local", clusters.localLights,
                clusterStatus);
            cpuProfiler_.recordCounter("cluster.references.requested",
                clusters.requestedReferences, clusterStatus);
            cpuProfiler_.recordCounter("cluster.references.published",
                clusters.publishedReferences, clusterStatus);
            cpuProfiler_.recordCounter("cluster.used", clusters.clustersUsed, clusterStatus);
            cpuProfiler_.recordCounter("cluster.occupancy.maximum",
                clusters.maximumOccupancy, clusterStatus);
            cpuProfiler_.recordCounter("cluster.fallback_lights",
                clusters.fallbackLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.dropped_lights",
                clusters.droppedLights, clusterStatus);
            cpuProfiler_.recordCounter("cluster.overflow_code", clusters.overflowCode,
                clusterStatus);
            break;
        }
        case RenderFrameStage::SceneLinearComplete:
            // Scene-linear captures read the lit scene here.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::SceneLinearReady,
                    *frameStage_.frame);
            break;
        case RenderFrameStage::OutputComplete:
            // Final-output captures read the output target before the UI pass.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::OutputReady,
                    *frameStage_.frame);
            break;
        }
    }

    EditorViewState Application::editorViewState(float aspect) {
        EditorViewState view{};
        view.renderingAssetView = editor.renderingAssetView;
        const EditorAssetDocument* previewDocument = editor.renderingAssetView
            ? editor.assetDocuments().active() : nullptr;
        view.assetPreviewActive = previewDocument != nullptr;
        AssetViewerPanel& viewer = editor.getAssetViewerPanel();
        if (view.assetPreviewActive) {
            view.previewModel = resolveEditorAssetPreview();
            view.previewSessionSerial = previewDocument->sessionSerial;
            view.previewFramingRevision = previewDocument->framingRevision;
            view.previewSelectedPart = previewDocument->selectedPart;
            view.previewSelectedPartIsMaterial =
                previewDocument->selectedPartIsMaterial;
            view.previewIsolateSelectedPart =
                previewDocument->isolateSelectedPart;
            view.previewHoveredPart = viewer.hoveredPart;
            view.previewHoveredPartIsMaterial = viewer.hoveredPartIsMaterial;
            view.previewRequestedExtent = viewer.requestedRenderExtent;
            const EditorPreviewLighting lighting = viewer.activeLighting();
            view.previewEnvironmentSettings = lighting.environmentSettings();
            view.previewExposureEv = lighting.exposureEv;
            if (const EditorOrbitCamera* camera = viewer.activeCamera()) {
                view.hasPreviewCamera = true;
                view.previewCameraPosition = camera->position();
                view.previewView = camera->viewMatrix();
                view.previewProjection = camera->projectionMatrix(aspect);
                const EditorOrbitCameraState& state = camera->state();
                view.previewNearPlane = state.nearPlane;
                view.previewFarPlane = state.farPlane;
                view.previewVerticalFovDegrees = state.verticalFovDegrees;
            }
        }
        view.selectedEntity = view.assetPreviewActive
            ? NULL_ENTITY : editor.getSelectedEntity();
        view.debugView = editor.getDebugView();
        view.wireframe = view.assetPreviewActive
            ? viewer.debugRenderMode == 1 : editor.currentRenderMode == 1;
        view.layeredInterfaceOverride = editor.layeredInterfaceOverride();
        return view;
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
            reflectionProbeCaptureScheduler_.configure({
                .maximumRenderedTexels = probeSettings.
                    maximumRenderedTexelsPerFrame,
                .maximumFacesPerProbePerFrame = probeSettings.
                    maximumFacesPerProbePerFrame,
                .maximumCapturesInFlight = probeSettings.
                    maximumCapturesInFlight,
                .minimumRealtimeFramesBetweenCaptures = probeSettings.
                    minimumRealtimeFramesBetweenCaptures,
            });
            renderBackend->configureReflectionProbeCaptures(
                probeSettings);
        }
    }

    void Application::drawFrame(AppFrameContext& frame) {
        const uint64_t applicationFrameIndex = frame.applicationFrameIndex;
        const bool dualViews = !policy_.fullscreenScenePresentation &&
            editor.assetDocuments().active();
        const auto cadenceNow = EditorViewCadence::Clock::now();
        uint32_t renderView = 0;
        if (dualViews) {
            const bool sceneFocused = editor.getViewportPanel().isFocused;
            renderView = editorViewScheduler_.choose(cadenceNow, sceneFocused ? 0u : 1u,
                editor.getViewportPanel().isVisible, editor.getAssetViewerPanel().isVisible,
                editor.getAssetViewerPanel().backgroundFramesPerSecond);
        } else editorViewScheduler_.reset();
        editor.renderingAssetView = renderView == 1;
        {
        CpuScope finalizeScope(cpuProfiler_, "cpu.probe.capture.finalize");
        for (const ReflectionProbeCaptureCompletion& completion :
                renderBackend->finalizeReflectionProbeCaptures()) {
            reflectionProbeCaptureScheduler_.markPublished(
                completion.owner, completion.captureTicket);
            assets_.publishCaptureCompletion(completion);
        }
        }
        const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments =
            assets_.loadedEnvironments();
        LightingFramePacket lightingFrame;
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.extract");
            if (editor.renderingAssetView) {
                auto& previewRegistry = previewLightingWorld_.registry();
                if (previewSun_ == NULL_ENTITY) {
                    previewSun_ = previewLightingWorld_.createEntity();
                    previewRegistry.addComponent<TransformComponent>(previewSun_);
                    previewRegistry.addComponent<LightComponent>(previewSun_);
                }
                const auto settings = editor.getAssetViewerPanel().activeLighting();
                auto& transform = previewRegistry.getComponent<TransformComponent>(previewSun_);
                transform.rotation = {settings.sunPitchDegrees, settings.sunYawDegrees, 0};
                auto& light = previewRegistry.getComponent<LightComponent>(previewSun_);
                light.colorLinearRec709 = settings.sunColor;
                light.illuminanceLux = settings.sunEnabled ? 1000.0f * std::exp2(settings.sunEv) : 0.0f;
                lightingFrame = lightExtractor_.extract(previewLightingWorld_);
            } else lightingFrame = lightExtractor_.extract(sceneWorld_);
        }
        {
            CpuScope lightScope(cpuProfiler_, "cpu.light.prepare");
            renderBackend->prepareLighting(lightingFrame.requiredCapacity);
        }
        ReflectionProbeFramePacket extractedProbes;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.extract");
            extractedProbes = extractReflectionProbes(sceneWorld_,
                [&loadedEnvironments](AssetGuid environment) {
                    return loadedEnvironments.contains(environment);
                });
            std::vector<SceneEntityUuid> runtimeCaptureOwners;
            runtimeCaptureOwners.reserve(extractedProbes.candidates.size());
            for (const ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates)
                if (candidate.probe.environmentAssetGuid.isNil())
                    runtimeCaptureOwners.push_back(candidate.owner);
            renderBackend->synchronizeReflectionProbeCaptureOwners(
                runtimeCaptureOwners);
            for (ReflectionProbeCandidate& candidate :
                    extractedProbes.candidates) {
                if (!candidate.probe.environmentAssetGuid.isNil()) continue;
                candidate.runtimeEnvironmentSlot = renderBackend->
                    capturedReflectionProbeEnvironmentSlot(candidate.owner);
                if (candidate.runtimeEnvironmentSlot)
                    candidate.resident = true;
            }
        }
        ReflectionProbeGpuFramePacket publishedProbes;
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.publish");
            reflectionProbeEnvironments_.clear();
            reflectionProbeEnvironments_.reserve(
                kMaximumGpuReflectionProbeEnvironments);
            for (const auto& [guid, environment] : loadedEnvironments) {
                (void)guid;
                if (reflectionProbeEnvironments_.size() >=
                    kMaximumGpuReflectionProbeEnvironments) break;
                reflectionProbeEnvironments_.push_back(environment.lighting);
            }
            publishedProbes = reflectionProbePublisher_.publish(
                extractedProbes.candidates,
                [this](AssetGuid environment) -> std::optional<uint32_t> {
                    const auto& environments = assets_.loadedEnvironments();
                    const auto found = environments.find(environment);
                    if (found == environments.end()) return std::nullopt;
                    const size_t index = static_cast<size_t>(std::distance(
                        environments.begin(), found));
                    if (index >= reflectionProbeEnvironments_.size())
                        return std::nullopt;
                    return static_cast<uint32_t>(index);
                });
        }
        {
            CpuScope probeScope(cpuProfiler_, "cpu.probe.prepare");
            renderBackend->prepareReflectionProbes(
                publishedProbes.requiredCapacity, reflectionProbeEnvironments_);
        }
        cpuProfiler_.recordCounter("probe.extracted",
            publishedProbes.stats.extractedCandidateCount);
        cpuProfiler_.recordCounter("probe.active",
            publishedProbes.stats.activeProbeCount);
        cpuProfiler_.recordCounter("probe.nonresident",
            publishedProbes.stats.nonresidentProbeCount);
        cpuProfiler_.recordCounter("probe.environment_unresolved",
            publishedProbes.stats.unresolvedEnvironmentCount);
        cpuProfiler_.recordCounter("probe.capacity_omitted",
            publishedProbes.stats.capacityOmittedCount);
        cpuProfiler_.recordCounter("probe.publish.changed_bytes",
            publishedProbes.stats.changedRecordBytes);
        assets_.processMaterialPreviews(editor.assetDocuments());
        prepareGpuScenePublication(editor.getSelectedEntity());
        // Descriptor publication waits for old users and must precede acquisition.
        // Keep the authored scene environment identity separate from this binding.
        auto desiredEnvironment = assets_.environmentLighting();
        AssetRuntimeService* const assetRuntimeService_ = assets_.runtime();
        AssetCatalog* const assetCatalog_ = assets_.catalog();
        AssetEnvironmentPreparationService* const
            assetEnvironmentPreparationService_ =
                assets_.environmentPreparation();
        auto& viewer = editor.getAssetViewerPanel();
        viewer.environmentDiagnostic.clear();
        if (editor.renderingAssetView) {
            const AssetGuid requested = viewer.activeLighting().environmentAsset;
            if (!requested.isNil()) {
                if (const auto loaded = loadedEnvironments.find(requested); loaded != loadedEnvironments.end()) {
                    desiredEnvironment = loaded->second.lighting;
                    if (assetRuntimeService_) assetRuntimeService_->touch(requested, applicationFrameIndex);
                } else {
                    viewer.environmentDiagnostic = "Preparing HDRI; showing the scene environment until ready.";
                    const auto state = assetRuntimeService_ ? assetRuntimeService_->snapshot(requested) : std::nullopt;
                    if (state && (state->state == RuntimeAssetState::Failed || state->state == RuntimeAssetState::ReadyWithError))
                        viewer.environmentDiagnostic = state->diagnostic;
                    else if (assetCatalog_ && assetEnvironmentPreparationService_ && !assetEnvironmentPreparationService_->pending(requested)) {
                        const auto records = assetCatalog_->recordsForGuid(requested);
                        const auto record = std::ranges::find_if(records, [](const AssetCatalogRecord& item) {
                            return !item.parentGuid && item.assetType == "iridium.environment" &&
                                item.assetRoot == "project" && item.status == AssetCatalogStatus::Ready;
                        });
                        if (record == records.end()) viewer.environmentDiagnostic = "The selected HDRI is no longer available in this project.";
                        else if (!state || state->state != RuntimeAssetState::Queued) {
                            try { (void)assetEnvironmentPreparationService_->request(*record); }
                            catch (const std::exception& error) { viewer.environmentDiagnostic = error.what(); }
                        }
                    }
                }
            }
        }
        if (desiredEnvironment.isValid() && desiredEnvironment != renderBackend->getEnvironmentLighting())
            renderBackend->setEnvironmentLighting(desiredEnvironment);
        editorBridge_->prepareRetainedViews(dualViews, renderView);
        editor.retainedSceneTexture = dualViews ? editorBridge_->retainedViewTextureId(0) : nullptr;
        editor.retainedAssetTexture = dualViews ? editorBridge_->retainedViewTextureId(1) : nullptr;
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (gpuSceneFrame_) renderBackend->publishGpuScene(*gpuSceneFrame_);
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
        opaqueQueue.clear();
        forwardOpaqueQueue.clear();
        transparentQueue.clear();
        sortedSurfaceQueue.clear();
        selectionQueue.clear();
        shadowCasterQueue.clear();
        probeCasterQueue_.clear();
        forwardInstanceTransforms_.clear();

        const float aspect = renderExtent_.height != 0
            ? static_cast<float>(renderExtent_.width) /
                static_cast<float>(renderExtent_.height)
            : 16.0f / 9.0f;
        // The previous-frame editor state that extraction reads before the
        // editor is built (design section 3.2, step 4).
        const EditorViewState preBuildView = editorViewState(aspect);
        const bool preBuildAssetPreviewActive =
            preBuildView.assetPreviewActive;
        const std::span<const uint32_t> shadowGpuScenePrimitiveIndices =
            !preBuildAssetPreviewActive && gpuSceneFrame_
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->shadowConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};
        const std::span<const uint32_t> probeGpuScenePrimitiveIndices =
            !preBuildAssetPreviewActive && gpuSceneFrame_ &&
                !policy_.routing.forceDirectGBufferReference &&
                !policy_.routing.forceDirectProbeCaptureReference
                ? std::span<const uint32_t>(
                    gpuSceneFrame_->probeConsumerPrimitiveIndices)
                : std::span<const uint32_t>{};

        // --- 2. GET CAMERA DATA ---
        glm::vec3 renderCameraPosition = camera_.position;
        float renderCameraNearPlane = camera_.nearPlane;
        float renderCameraFarPlane = camera_.farPlane;
        float renderVerticalFovDegrees = camera_.verticalFovDegrees;
        glm::mat4 viewMatrix = glm::lookAt(
            camera_.position, camera_.position + camera_.front, camera_.up);
        glm::mat4 projMatrix = glm::perspective(
            glm::radians(camera_.verticalFovDegrees), aspect,
            camera_.nearPlane, camera_.farPlane);
        projMatrix[1][1] *= -1.0f; // Vulkan inverted Y
        const auto usePreviewCamera = [&](const EditorViewState& view) {
            if (!view.assetPreviewActive || !view.hasPreviewCamera) return;
            renderCameraPosition = view.previewCameraPosition;
            viewMatrix = view.previewView;
            projMatrix = view.previewProjection;
            renderCameraNearPlane = view.previewNearPlane;
            renderCameraFarPlane = view.previewFarPlane;
            renderVerticalFovDegrees = view.previewVerticalFovDegrees;
        };
        usePreviewCamera(preBuildView);

        // Build ImGui only after beginFrame selected currentImageIndex. The UI
        // descriptors are per swapchain image, so using them before acquisition
        // can sample a different target that has not yet been transitioned.
        EditorFrameRequests editorRequests{};
        {
            CpuScope editorScope(cpuProfiler_, "cpu.editor.build");
            editorBridge_->beginUI();
            if (!policy_.fullscreenScenePresentation) {
                glm::mat4 sceneProjection = glm::perspective(glm::radians(camera_.verticalFovDegrees), aspect, camera_.nearPlane, camera_.farPlane);
                sceneProjection[1][1] *= -1.0f;
                editor.update(registry, assets_.assetManager(), glm::lookAt(camera_.position, camera_.position + camera_.front, camera_.up), sceneProjection,
                    editorBridge_->sceneTextureId(),
                    editorBridge_->glassDepthTextureId(),
                    aspect);
                EditorOutputSettings outputSettings{};
                if (editor.consumeOutputSettings(outputSettings))
                    editorRequests.output = std::move(outputSettings);
                ProjectShadowSettings shadowSettings{};
                if (editor.consumeShadowSettings(shadowSettings))
                    editorRequests.shadows = shadowSettings;
                ProjectReflectionProbeSettings probeSettings{};
                if (editor.consumeReflectionProbeSettings(probeSettings))
                    editorRequests.probes = probeSettings;
                editorRequests.requestedSceneExtent =
                    editor.requestedRenderExtent();
                applyEditorFrameRequests(editorRequests);
                editorRequests.view = editorViewState(aspect);
            }
            else {
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(viewport->Pos);
                ImGui::SetNextWindowSize(viewport->Size);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBringToFrontOnFocus;
                ImGui::Begin("Benchmark Output", nullptr, flags);
                ImGui::Image(reinterpret_cast<ImTextureID>(
                    editorBridge_->sceneTextureId()), ImGui::GetContentRegionAvail());
                ImGui::End();
                ImGui::PopStyleVar();
                if (policy_.colorValidationOverlay) {
                    editor.drawColorValidationOverlay();
                }
                // No asset preview without dual views: nothing is resolved.
                editorRequests.view = editorViewState(aspect);
            }
        }
        // The editor state after this frame's build: what extraction reads.
        const EditorViewState& view = editorRequests.view;
        usePreviewCamera(view);
        const bool assetPreviewActive = view.assetPreviewActive;
        const std::shared_ptr<ModelAsset>& previewModel = view.previewModel;
        const Entity selectedEntity = view.selectedEntity;

        if (assetPreviewActive) {
            const auto extent = view.previewRequestedExtent;
            const auto fit = previewImageFit(aspect, extent.height ? static_cast<float>(extent.width) / extent.height : aspect);
            projMatrix[0][0] *= fit.projectionScale;
            projMatrix[1][1] *= fit.projectionScale;
        }
        const ViewTransportRecord viewTransport = makeViewTransportRecord(
            viewMatrix, projMatrix, renderCameraPosition,
            renderCameraNearPlane, renderCameraFarPlane,
            { renderExtent_.width, renderExtent_.height });
        const RenderDebugView debugView = view.debugView;
        renderBackend->setEnvironmentLightingSettings(assetPreviewActive
            ? view.previewEnvironmentSettings : assets_.sceneEnvironmentSettings());
        const float viewExposure = assetPreviewActive ? view.previewExposureEv : config_.manualExposureEv;
        // M7R R3c.11: the frame is assembled from spans over this frame's
        // queues and packets and submitted once, after extraction.
        RenderFrame renderFrame{
            .view = viewTransport,
            .history = {
                .identity = assetPreviewActive ? view.previewSessionSerial + 2u : 1u,
                .resetRevision = assetPreviewActive ? view.previewFramingRevision :
                    frameRequests_.viewHistoryResetRevision.value_or(0u),
            },
            .debugView = debugView,
            .output = { viewExposure, static_cast<float>(config_.paperWhiteNits),
                static_cast<float>(config_.peakNits) },
        };
        if (!frameRequests_.suppressGridOverlay && !assetPreviewActive) {
            renderFrame.gridOverlay = editor.viewportGridOverlay(viewMatrix, projMatrix);
        }

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        uint64_t requestedModelRecords = 0;
        uint64_t requestedInstances = 0;
        uint64_t requestedSubmeshes = 0;
        uint64_t requestedSourceTriangles = 0;
        uint64_t requestedSourceIndexBytes = 0;
        uint64_t requestedArenaIndexBytes = 0;
        uint64_t requestedArenaSavedIndexBytes = 0;
        uint64_t requestedUInt16Indices = 0;
        uint64_t requestedUInt32Indices = 0;
        uint64_t requestedLodFallbackChains = 0;
        uint64_t requestedLodWithheldRanges = 0;
        uint64_t requestedLodWithheldIndexBytes = 0;
        uint32_t maximumRequestedLodResidentBase = 0;
        GpuSceneVisibilityStats gpuSceneVisibilityStats{};
        uint64_t gpuSceneDeferredCandidateCount = 0;
        uint64_t gpuSceneDeferredCandidateTriangles = 0;
        uint64_t gpuSceneForwardVisibleCount = 0;
        uint64_t gpuSceneForwardVisibleTriangles = 0;
        if (!assetPreviewActive && gpuSceneFrame_) {
            CpuScope classifyScope(cpuProfiler_, "cpu.render.classify");
            classifyGpuSceneFrustum(*gpuSceneFrame_,
                makeGpuSceneFrustum(projMatrix * viewMatrix),
                GpuSceneConsumerMainOpaque |
                    GpuSceneConsumerForwardOpaque,
                gpuSceneVisibility_);
            gpuSceneVisibilityStats = gpuSceneVisibility_.stats;
        }
        uint64_t transparentCulled = 0;
        {
            CpuScope extractionScope(cpuProfiler_, "cpu.render.extract");
            const auto appendModel = [&](const ModelAsset& model,
                    const glm::mat4& worldTransform,
                    const MeshComponent* meshComponent,
                    const RenderInstanceBatchComponent* instanceBatch,
                    const CookedMaterialRuntimeBinding* forcedMaterial,
                    bool selected, SceneEntityUuid owner,
                    bool persistentOpaque) {
                if (!model.geometry.isValid()) return;
                if (instanceBatch &&
                    instanceBatch->localTransforms.size() >
                        kWeightedOitMaximumInstanceCount) {
                    throw std::length_error(
                        "Render instance batch exceeds WeightedOIT capacity");
                }
                const uint32_t instanceCount = instanceBatch
                    ? static_cast<uint32_t>(
                        instanceBatch->localTransforms.size())
                    : 1u;
                if (instanceCount == 0u) return;
                ++requestedModelRecords;
                requestedSourceIndexBytes += model.sourceIndexBytes;
                requestedArenaIndexBytes += model.arenaIndexBytes;
                requestedArenaSavedIndexBytes += model.arenaSavedIndexBytes;
                requestedUInt16Indices += model.arenaUInt16IndexCount;
                requestedUInt32Indices += model.arenaUInt32IndexCount;
                requestedLodFallbackChains += model.lodFallbackChainCount;
                requestedLodWithheldRanges +=
                    model.lodWithheldPrimitiveRangeCount;
                requestedLodWithheldIndexBytes +=
                    model.lodWithheldIndexBytes;
                maximumRequestedLodResidentBase = (std::max)(
                    maximumRequestedLodResidentBase,
                    model.lodResidentBaseLevel);
                if (instanceBatch && selected) {
                    throw std::logic_error(
                        "Render instance batch selection is not implemented");
                }
                const uint32_t firstInstanceTransform = static_cast<uint32_t>(
                    forwardInstanceTransforms_.size());
                if (instanceBatch) {
                    forwardInstanceTransforms_.reserve(
                        forwardInstanceTransforms_.size() + instanceCount);
                    const glm::mat4 identity(1.0f);
                    if (std::memcmp(&worldTransform, &identity,
                            sizeof(glm::mat4)) == 0) {
                        forwardInstanceTransforms_.insert(
                            forwardInstanceTransforms_.end(),
                            instanceBatch->localTransforms.begin(),
                            instanceBatch->localTransforms.end());
                    }
                    else {
                        const size_t first =
                            forwardInstanceTransforms_.size();
                        forwardInstanceTransforms_.resize(first +
                            instanceCount);
                        for (uint32_t index = 0u;
                            index < instanceCount; ++index) {
                            forwardInstanceTransforms_[first + index] =
                                worldTransform *
                                instanceBatch->localTransforms[index];
                        }
                    }
                }
                requestedInstances += instanceCount;
                const float distanceToCamera = glm::distance(
                    renderCameraPosition, glm::vec3(worldTransform[3]));
                for (size_t subMeshIndex = 0u;
                    subMeshIndex < model.subMeshes.size(); ++subMeshIndex) {
                    const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                    const bool previewPartSelected = assetPreviewActive &&
                        view.previewSelectedPart &&
                        *view.previewSelectedPart == (view.previewSelectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (assetPreviewActive &&
                        view.previewIsolateSelectedPart && !previewPartSelected) continue;
                    requestedSubmeshes += instanceCount;
                    requestedSourceTriangles +=
                        (static_cast<uint64_t>(subMesh.indexCount) / 3u) *
                        instanceCount;
                    const int materialIndex = subMesh.materialIndex;
                    if (materialIndex < 0 ||
                        static_cast<size_t>(materialIndex) >= model.materials.size()) {
                        continue;
                    }
                    const MaterialBinding* binding = forcedMaterial
                        ? &forcedMaterial->binding
                        : &model.materials[materialIndex];
                    std::optional<CookedMaterialRuntimeBinding>
                        overrideBinding;
                    AssetGuid effectiveMaterialGuid = subMesh.materialGuid;
                    CompiledTransparencyPolicy effectiveTransparency =
                        subMesh.transparency;
                    TransparencyExecutionMode effectiveExecutionMode =
                        model.transparencyExecutionMode;
                    if (forcedMaterial) {
                        effectiveMaterialGuid = forcedMaterial->materialGuid;
                        effectiveTransparency = forcedMaterial->transparency;
                        effectiveExecutionMode =
                            forcedMaterial->transparencyExecutionMode;
                    }
                    if (!forcedMaterial && meshComponent) {
                        const auto materialOverride = std::ranges::find_if(
                            meshComponent->materialOverrides,
                            [&subMesh](const MeshComponent::MaterialOverride& candidate) {
                                return candidate.sourceMaterialGuid ==
                                    subMesh.materialGuid;
                            });
                        if (materialOverride !=
                            meshComponent->materialOverrides.end()) {
                            overrideBinding =
                                assets_.assetManager()->findCookedMaterialRuntime(
                                    materialOverride->materialGuid);
                            if (overrideBinding) {
                                effectiveMaterialGuid =
                                    materialOverride->materialGuid;
                                binding = &overrideBinding->binding;
                                effectiveTransparency =
                                    overrideBinding->transparency;
                                effectiveExecutionMode = overrideBinding
                                    ->transparencyExecutionMode;
                            }
                        }
                    }
                    if (!binding->material.isValid() ||
                        !binding->pipeline.isValid()) {
                        continue;
                    }
                    if (persistentOpaque &&
                        binding->renderQueue != RenderQueue::Transparent) {
                        continue;
                    }
                    DrawPacket packet{};
                    packet.geometry = subMesh.geometry.isValid()
                        ? subMesh.geometry : model.geometry;
                    packet.material = binding->material;
                    packet.pipeline = binding->pipeline;
                    packet.opaqueSortKey = binding->opaqueSortKey;
                    packet.indexCount = subMesh.indexCount;
                    packet.firstIndex = subMesh.indexStart;
                    packet.worldTransform = worldTransform;
                    packet.distanceToCamera = distanceToCamera;
                    packet.isSelected = selected ? 1 : 0;
                    packet.owner = owner;
                    packet.sourcePrimitiveGuid =
                        subMesh.sourcePrimitiveGuid;
                    packet.primitiveGuid = subMesh.primitiveGuid;
                    packet.materialGuid = effectiveMaterialGuid;
                    packet.transparency = policy_.deterministicContent ? effectiveTransparency :
                        withLayeredInterfaceBudget(effectiveTransparency,
                            static_cast<unsigned>(view.layeredInterfaceOverride));
                    packet.transparencyExecutionMode =
                        effectiveExecutionMode;
                    packet.coverage = subMesh.coverage;
                    packet.firstInstanceTransform = instanceBatch
                        ? firstInstanceTransform : UINT32_MAX;
                    packet.instanceCount = instanceCount;
                    glm::vec3 packetBoundsMin = subMesh.boundsMin;
                    glm::vec3 packetBoundsMax = subMesh.boundsMax;
                    if (instanceBatch) {
                        if (subMeshIndex >=
                                instanceBatch->subMeshBounds.size() ||
                            !instanceBatch->subMeshBounds[subMeshIndex].valid) {
                            throw std::logic_error(
                                "Render instance batch bounds do not match model");
                        }
                        packetBoundsMin = instanceBatch->subMeshBounds[
                            subMeshIndex].minimum;
                        packetBoundsMax = instanceBatch->subMeshBounds[
                            subMeshIndex].maximum;
                    }
                    const glm::vec3 packetBoundsCenter =
                        (packetBoundsMin + packetBoundsMax) * 0.5f;
                    const float packetBoundsRadius = glm::length(
                        packetBoundsMax - packetBoundsCenter);
                    const ShadowCasterSphere shadowBounds =
                        transformShadowCasterSphere(
                            instanceBatch ? packetBoundsCenter :
                                subMesh.boundsSphereCenter,
                            instanceBatch ? packetBoundsRadius :
                                subMesh.boundsSphereRadius,
                            worldTransform);
                    packet.boundsSphereCenterWorld = shadowBounds.center;
                    packet.boundsSphereRadiusWorld = shadowBounds.radius;
                    if (binding->renderQueue == RenderQueue::Transparent) {
                        if (instanceBatch && !isWeightedOitPacket(packet)) {
                            throw std::logic_error(
                                "Render instance batches currently require "
                                "classified WeightedOIT materials");
                        }
                        const bool visible = prepareTransparentWorkInterval(
                            packet, packetBoundsMin, packetBoundsMax,
                            viewMatrix, renderCameraNearPlane,
                            renderCameraFarPlane);
                        if (!visible && effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified) {
                            ++transparentCulled;
                            continue;
                        }
                        if (effectiveExecutionMode ==
                                TransparencyExecutionMode::Classified &&
                            (packet.transparency.resolvedClass ==
                                    TransparencyClass::SortedSurface ||
                                isWeightedOitPacket(packet))) {
                            sortedSurfaceQueue.push_back(packet);
                        }
                        else {
                            transparentQueue.push_back(packet);
                        }
                    }
                    else if (binding->renderQueue == RenderQueue::ForwardOpaque) {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Forward-opaque instance batches are not implemented");
                        }
                        forwardOpaqueQueue.push_back(packet);
                    }
                    else {
                        if (instanceBatch) {
                            throw std::logic_error(
                                "Opaque instance batches are not implemented");
                        }
                        opaqueQueue.push_back(packet);
                    }
                    const bool previewHovered = assetPreviewActive &&
                        !view.previewIsolateSelectedPart && !view.previewHoveredPart.isNil() &&
                        view.previewHoveredPart == (view.previewHoveredPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid);
                    if (selected || previewHovered || (previewPartSelected && !view.previewIsolateSelectedPart)) {
                        packet.selectionFeedback = static_cast<uint8_t>(
                            ((selected || previewPartSelected) ? 1u : 0u) |
                            (previewHovered ? 2u : 0u));
                        selectionQueue.push_back(packet);
                    }
                }
            };

            if (assetPreviewActive) {
                if (previewModel) {
                    appendModel(*previewModel, glm::mat4(1.0f), nullptr,
                        nullptr,
                        nullptr, false,
                        {}, false);
                }
            }
            else {
                auto* transformPool = registry.getPool<TransformComponent>();
                auto* meshPool = registry.getPool<MeshComponent>();
                auto* instanceBatchPool = registry.findPool<
                    RenderInstanceBatchComponent>();
                if (transformPool && meshPool) {
                    for (Entity entity : meshPool->entities) {
                        MeshComponent& meshComponent = meshPool->get(entity);
                        if (!meshComponent.enabled || !meshComponent.model ||
                            !transformPool->has(entity)) {
                            continue;
                        }
                        const SceneEntityUuid owner = sceneWorld_.identities()
                            .persistentId(entity).value_or(
                                SceneEntityUuid{});
                        bool persistentOpaque = false;
                        if (gpuSceneFrame_ && !owner.isNil()) {
                            const auto found = std::ranges::lower_bound(
                                gpuSceneFrame_->instanceIdentities, owner, {},
                                &GpuSceneInstanceIdentity::owner);
                            persistentOpaque = found !=
                                gpuSceneFrame_->instanceIdentities.end() &&
                                found->owner == owner;
                        }
                        appendModel(*meshComponent.model,
                            transformPool->get(entity).worldMatrix,
                            &meshComponent,
                            instanceBatchPool && instanceBatchPool->has(entity)
                                ? &instanceBatchPool->get(entity) : nullptr,
                            nullptr,
                            entity == selectedEntity, owner,
                            persistentOpaque);
                    }
                }
            }

            // M7.2 parity stage: ordinary scene opaque work is reconstructed
            // from the persistent publication. Transparent and explicit
            // fallback owners above continue to use the M6 packet path.
            if (!assetPreviewActive && gpuSceneFrame_) {
                CpuScope parityScope(cpuProfiler_, "cpu.render.extract.parity");
                for (uint32_t primitiveIndex = 0;
                        primitiveIndex < gpuSceneFrame_->primitives.size();
                        ++primitiveIndex) {
                    const GpuScenePrimitiveRecord& primitive =
                        gpuSceneFrame_->primitives[primitiveIndex];
                    const bool cpuVisible = primitiveIndex <
                            gpuSceneVisibility_.primitiveVisibility.size() &&
                        gpuSceneVisibility_.primitiveVisibility[primitiveIndex] != 0u;
                    const uint32_t instanceIndex = primitive.binding.x;
                    if (instanceIndex >= gpuSceneFrame_->instances.size() ||
                        instanceIndex >= gpuSceneFrame_->instanceIdentities.size() ||
                        primitive.binding.y >= gpuSceneFrame_->geometries.size() ||
                        primitiveIndex >= gpuSceneFrame_->primitiveIdentities.size()) {
                        throw std::logic_error(
                            "Published visible GPU-scene references are invalid");
                    }
                    const GpuSceneInstanceRecord& instance =
                        gpuSceneFrame_->instances[instanceIndex];
                    if (instance.references.x >= gpuSceneFrame_->transforms.size()) {
                        throw std::logic_error(
                            "Published visible GPU-scene transform is invalid");
                    }
                    const glm::mat4 worldTransform = unpackGpuSceneAffine(
                        gpuSceneFrame_->transforms[instance.references.x]);
                    const SceneEntityUuid owner = gpuSceneFrame_->
                        instanceIdentities[instanceIndex].owner;
                    const bool selected = (instance.state.z &
                        GpuSceneInstanceSelected) != 0;
                    const GpuSceneGeometryRecord& geometry =
                        gpuSceneFrame_->geometries[primitive.binding.y];
                        DrawPacket packet{};
                        packet.geometry = GeometryHandle{ geometry.storage.x };
                        packet.material = MaterialHandle{ primitive.binding.z };
                        packet.pipeline = PipelineHandle{ primitive.binding.w };
                        packet.opaqueSortKey =
                            (static_cast<uint64_t>(primitive.binding.w) << 32u) |
                            primitive.binding.z;
                        packet.indexCount = geometry.draw.y;
                        packet.firstIndex = geometry.draw.x;
                        packet.executionFlags =
                            DrawPacketGpuScenePrimitive;
                        if (cpuVisible) {
                            packet.executionFlags |=
                                DrawPacketCpuVisibilityOracle;
                        }
                        packet.firstInstanceTransform = primitiveIndex;
                        packet.worldTransform = worldTransform;
                        packet.distanceToCamera = glm::distance(
                            renderCameraPosition, glm::vec3(worldTransform[3]));
                        packet.isSelected = selected ? 1 : 0;
                        packet.owner = owner;
                        const GpuScenePrimitiveIdentity& identity =
                            gpuSceneFrame_->primitiveIdentities[primitiveIndex];
                        packet.sourcePrimitiveGuid = identity.sourcePrimitiveGuid;
                        packet.primitiveGuid = identity.primitiveGuid;
                        packet.materialGuid = identity.effectiveMaterialGuid;
                        packet.coverage = (primitive.state.y &
                            GpuScenePrimitiveAlphaMask) != 0
                            ? static_cast<uint8_t>(ModelCoverage::Masked)
                            : static_cast<uint8_t>(ModelCoverage::Opaque);
                        packet.boundsSphereCenterWorld = {
                            instance.worldBoundsSphere.x,
                            instance.worldBoundsSphere.y,
                            instance.worldBoundsSphere.z,
                        };
                        packet.boundsSphereRadiusWorld =
                            instance.worldBoundsSphere.w;
                        if ((primitive.state.w &
                                GpuSceneConsumerForwardOpaque) != 0) {
                            if (cpuVisible) {
                                forwardOpaqueQueue.push_back(packet);
                                ++gpuSceneForwardVisibleCount;
                                gpuSceneForwardVisibleTriangles +=
                                    packet.indexCount / 3u;
                            }
                        }
                        else if ((primitive.state.w &
                                GpuSceneConsumerMainOpaque) != 0) {
                            opaqueQueue.push_back(packet);
                            ++gpuSceneDeferredCandidateCount;
                            gpuSceneDeferredCandidateTriangles +=
                                packet.indexCount / 3u;
                        }
                    if (selected && cpuVisible)
                        selectionQueue.push_back(packet);
                }
            }
        }

        cpuProfiler_.recordCounter("draw.requested.opaque", opaqueQueue.size());
        cpuProfiler_.recordCounter("draw.requested.forward_opaque",
            forwardOpaqueQueue.size());
        const uint64_t requestedTransparent =
            transparentQueue.size() + sortedSurfaceQueue.size();
        cpuProfiler_.recordCounter("draw.requested.transparent",
            requestedTransparent);
        cpuProfiler_.recordCounter("draw.requested.selection", selectionQueue.size());
        cpuProfiler_.recordCounter("geometry.model_record.requested",
            requestedModelRecords);
        cpuProfiler_.recordCounter("instance.requested", requestedInstances);
        cpuProfiler_.recordCounter("submesh.requested", requestedSubmeshes);
        cpuProfiler_.recordCounter("geometry.triangle.source_requested",
            requestedSourceTriangles);
        cpuProfiler_.recordCounter("geometry.index.source_requested_bytes",
            requestedSourceIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_requested_bytes",
            requestedArenaIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.arena_saved_requested_bytes",
            requestedArenaSavedIndexBytes);
        cpuProfiler_.recordCounter("geometry.index.uint16_requested",
            requestedUInt16Indices);
        cpuProfiler_.recordCounter("geometry.index.uint32_requested",
            requestedUInt32Indices);
        cpuProfiler_.recordCounter("geometry.lod.resident_base_level.maximum_requested",
            maximumRequestedLodResidentBase);
        cpuProfiler_.recordCounter("geometry.lod.fallback_chains.requested",
            requestedLodFallbackChains);
        cpuProfiler_.recordCounter("geometry.lod.withheld_ranges.requested",
            requestedLodWithheldRanges);
        cpuProfiler_.recordCounter("geometry.lod.withheld_index_bytes.requested",
            requestedLodWithheldIndexBytes, ProfileCounterStatus::Exact,
            ProfileCounterUnit::Bytes);
        const uint64_t gpuSceneQueuedPrimitives =
            gpuSceneDeferredCandidateCount + gpuSceneForwardVisibleCount;
        const uint64_t totalQueuedPrimitives =
            opaqueQueue.size() + forwardOpaqueQueue.size();
        const uint64_t directOpaquePrimitives = totalQueuedPrimitives >=
                gpuSceneQueuedPrimitives
            ? totalQueuedPrimitives - gpuSceneQueuedPrimitives : 0;
        const uint64_t visibleOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.visiblePrimitives;
        const uint64_t requestedOpaquePrimitives = directOpaquePrimitives +
            gpuSceneVisibilityStats.requestedPrimitives;
        const auto submittedTriangles = [](const auto& queue) noexcept {
            uint64_t triangles = 0;
            for (const DrawPacket& packet : queue) {
                triangles += (static_cast<uint64_t>(packet.indexCount) / 3u) *
                    packet.instanceCount;
            }
            return triangles;
        };
        const uint64_t totalQueuedTriangles =
            submittedTriangles(opaqueQueue) +
            submittedTriangles(forwardOpaqueQueue);
        const uint64_t gpuSceneQueuedTriangles =
            gpuSceneDeferredCandidateTriangles +
            gpuSceneForwardVisibleTriangles;
        const uint64_t directOpaqueTriangles = totalQueuedTriangles >=
                gpuSceneQueuedTriangles
            ? totalQueuedTriangles - gpuSceneQueuedTriangles : 0;
        const uint64_t visibleOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.visibleTriangles;
        const uint64_t requestedOpaqueTriangles = directOpaqueTriangles +
            gpuSceneVisibilityStats.requestedTriangles;
        cpuProfiler_.recordCounter("opaque.primitive.requested",
            requestedOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.triangle.requested",
            requestedOpaqueTriangles);
        cpuProfiler_.recordCounter("opaque.primitive.visible",
            visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.frustum_rejected",
            requestedOpaquePrimitives - visibleOpaquePrimitives);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.requested",
            gpuSceneVisibilityStats.requestedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.visible",
            gpuSceneVisibilityStats.visibleInstances);
        cpuProfiler_.recordCounter(
            "gpu_scene.visibility.instance.frustum_rejected",
            gpuSceneVisibilityStats.frustumRejectedInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.instance.fail_visible",
            gpuSceneVisibilityStats.failVisibleInstances);
        cpuProfiler_.recordCounter("gpu_scene.visibility.primitive.fail_visible",
            gpuSceneVisibilityStats.failVisiblePrimitives);
        cpuProfiler_.recordCounter("opaque.primitive.occlusion_rejected", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("opaque.triangle.visible",
            visibleOpaqueTriangles);
        const GpuScenePublisherStats gpuSceneStats = gpuScenePublisher_
            ? gpuScenePublisher_->stats() : GpuScenePublisherStats{};
        cpuProfiler_.recordCounter("gpu_scene.instance_upload_bytes",
            gpuSceneStats.changedInstanceBytes);
        cpuProfiler_.recordCounter("gpu_scene.transform_upload_bytes",
            gpuSceneStats.changedTransformBytes);
        cpuProfiler_.recordCounter("transparent.primitive.requested",
            requestedTransparent);
        const uint64_t sortedSurfacePackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return packet.transparency.resolvedClass ==
                    TransparencyClass::SortedSurface;
            });
        const uint64_t weightedOitPackets = std::ranges::count_if(
            sortedSurfaceQueue, [](const DrawPacket& packet) {
                return isWeightedOitPacket(packet);
            });
        cpuProfiler_.recordCounter("transparent.class.sorted_surface",
            sortedSurfacePackets);
        cpuProfiler_.recordCounter("transparent.class.weighted_oit",
            weightedOitPackets);
        cpuProfiler_.recordCounter("transparent.class.compatibility_fallback",
            transparentQueue.size());
        uint64_t thinGlassPackets = 0;
        uint64_t layeredGlassPackets = 0;
        uint64_t zeroThicknessPackets = 0;
        uint64_t metricThicknessPackets = 0;
        const auto countTransportPackets = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (packet.transparency.resolvedClass ==
                        TransparencyClass::ThinGlass) {
                    ++thinGlassPackets;
                }
                else if (packet.transparency.resolvedClass ==
                        TransparencyClass::LayeredGlass) {
                    ++layeredGlassPackets;
                }
                else {
                    continue;
                }
                if (packet.transparency.thinSheetThicknessMeters > 0.0f)
                    ++metricThicknessPackets;
                else
                    ++zeroThicknessPackets;
            }
        };
        countTransportPackets(transparentQueue);
        countTransportPackets(sortedSurfaceQueue);
        cpuProfiler_.recordCounter("transparent.class.thin_glass",
            thinGlassPackets);
        cpuProfiler_.recordCounter("transparent.class.layered_glass",
            layeredGlassPackets);
        cpuProfiler_.recordCounter("transparent.transport.zero_sheet_thickness",
            zeroThicknessPackets);
        cpuProfiler_.recordCounter("transparent.transport.metric_sheet_thickness",
            metricThicknessPackets);
        cpuProfiler_.recordCounter("transparent.work.culled", transparentCulled);
        cpuProfiler_.recordCounter("light.scene",
            lightingFrame.stats.sceneLightCount);
        cpuProfiler_.recordCounter("light.active",
            lightingFrame.stats.activeLightCount);
        cpuProfiler_.recordCounter("light.omitted",
            lightingFrame.stats.omittedLightCount);
        cpuProfiler_.recordCounter("light.capacity",
            lightingFrame.stats.capacity);
        cpuProfiler_.recordCounter("light.changed_record_bytes",
            lightingFrame.stats.changedRecordBytes);
        cpuProfiler_.recordCounter("light.changed_record_ranges",
            lightingFrame.stats.changedRangeCount);
        cpuProfiler_.recordCounter("changed.transforms", changedTransformsThisFrame_);
        cpuProfiler_.recordCounter("changed.materials", 0,
            ProfileCounterStatus::Unavailable);
        cpuProfiler_.recordCounter("changed.lights",
            lightingFrame.stats.changedRecordCount);
        cpuProfiler_.recordCounter("changed.instances",
            gpuSceneStats.changedInstances);

        // --- 4. THE SORTING PHASE (CPU Cache Optimization) ---

        // Group opaque objects by the PSO/material identity carried by each binding.
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.opaque");
            std::sort(opaqueQueue.begin(), opaqueQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
                if (a.opaqueSortKey != b.opaqueSortKey) return a.opaqueSortKey < b.opaqueSortKey;
                if (a.geometry != b.geometry) return a.geometry < b.geometry;
                return a.firstIndex < b.firstIndex;
                });
        }
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.forward_opaque");
            std::sort(forwardOpaqueQueue.begin(), forwardOpaqueQueue.end(),
                [](const DrawPacket& a, const DrawPacket& b) {
                    if (a.opaqueSortKey != b.opaqueSortKey) {
                        return a.opaqueSortKey < b.opaqueSortKey;
                    }
                    if (a.geometry != b.geometry) return a.geometry < b.geometry;
                    return a.firstIndex < b.firstIndex;
                });
        }

        shadowCasterQueue.reserve(
            opaqueQueue.size() + forwardOpaqueQueue.size());
        const auto appendDirectShadowFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (!hasGpuScenePrimitive(packet))
                    shadowCasterQueue.push_back(packet);
            }
        };
        appendDirectShadowFallbacks(opaqueQueue);
        appendDirectShadowFallbacks(forwardOpaqueQueue);
        const auto appendDirectProbeFallbacks = [&](const auto& queue) {
            for (const DrawPacket& packet : queue) {
                if (policy_.routing.forceDirectGBufferReference ||
                    policy_.routing.forceDirectProbeCaptureReference ||
                    !hasGpuScenePrimitive(packet))
                    probeCasterQueue_.push_back(packet);
            }
        };
        appendDirectProbeFallbacks(opaqueQueue);
        appendDirectProbeFallbacks(forwardOpaqueQueue);
        const ShadowCasterSubmission shadowCasters{
            .gpuScenePrimitiveIndices = shadowGpuScenePrimitiveIndices,
            .directPackets = shadowCasterQueue,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_
                ? gpuSceneFrame_->shadowConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("shadow.casters.gpu_scene",
            shadowGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("shadow.casters.direct_fallback",
            shadowCasterQueue.size());
        cpuProfiler_.recordCounter("shadow.casters.submission_bytes",
            shadowGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                shadowCasterQueue.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        const ReflectionProbeCasterSubmission probeCasters{
            .gpuScenePrimitiveIndices = probeGpuScenePrimitiveIndices,
            .directPackets = probeCasterQueue_,
            .membershipRevision = !assetPreviewActive && gpuSceneFrame_ &&
                !policy_.routing.forceDirectGBufferReference &&
                !policy_.routing.forceDirectProbeCaptureReference
                ? gpuSceneFrame_->probeConsumerMembershipRevision : 0u,
        };
        cpuProfiler_.recordCounter("probe.capture.casters.gpu_scene",
            probeGpuScenePrimitiveIndices.size());
        cpuProfiler_.recordCounter("probe.capture.casters.direct_fallback",
            probeCasterQueue_.size());
        cpuProfiler_.recordCounter("probe.capture.casters.submission_bytes",
            probeGpuScenePrimitiveIndices.size() * sizeof(uint32_t) +
                probeCasterQueue_.size() * sizeof(DrawPacket),
            ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        {
            CpuScope sortScope(cpuProfiler_, "cpu.render.sort.transparent");
            std::sort(transparentQueue.begin(), transparentQueue.end(),
                transparentCompatibilityLess);
            std::sort(sortedSurfaceQueue.begin(), sortedSurfaceQueue.end(),
                transparentWorkLess);
        }
        cpuProfiler_.recordCounter("transparent.work.invalid_bounds",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkInvalidBoundsFallback) != 0;
                }));
        cpuProfiler_.recordCounter("transparent.work.near_clipped",
            std::ranges::count_if(sortedSurfaceQueue,
                [](const DrawPacket& packet) {
                    return (packet.transparentWorkFlags &
                        TransparentWorkNearClipped) != 0;
                }));
        {
            CpuScope intervalScope(cpuProfiler_, "cpu.render.transparent.intervals");
            transparentIntervalEndpointScratch.resize(sortedSurfaceQueue.size());
            transparentIntervalNearScratch.resize(sortedSurfaceQueue.size());
            transparentIntervalFenwickScratch.resize(
                sortedSurfaceQueue.size() + 1u);
            cpuProfiler_.recordCounter("transparent.sort.ambiguous_intervals",
                sweepAmbiguousTransparentIntervals(sortedSurfaceQueue,
                    transparentIntervalEndpointScratch,
                    transparentIntervalNearScratch,
                    transparentIntervalFenwickScratch));
        }

        // --- 5. THE SUBMISSION PHASE (The Black Box) ---

        std::vector<DirectionalShadowFramePacket> directionalShadows;
        {
        CpuScope directionalScope(cpuProfiler_, "cpu.shadow.directional.schedule");
        const std::vector<DirectionalShadowSelection> shadowSelections =
            selectDirectionalShadowLights(lightingFrame,
                config_.shadowSettings.maximumDirectionalLights);
        if (!shadowSelections.empty()) {
            const glm::mat4 inverseView = glm::inverse(viewMatrix);
            DirectionalShadowCamera shadowCamera{};
            shadowCamera.position = renderCameraPosition;
            shadowCamera.forward = glm::normalize(-glm::vec3(inverseView[2]));
            shadowCamera.up = glm::normalize(glm::vec3(inverseView[1]));
            shadowCamera.verticalFovRadians = glm::radians(
                renderVerticalFovDegrees);
            shadowCamera.aspectRatio = aspect;
            shadowCamera.nearPlane = renderCameraNearPlane;
            shadowCamera.farPlane = (std::min)(renderCameraFarPlane,
                (std::max)(config_.shadowSettings.
                    directionalMaximumDistanceMeters,
                    renderCameraNearPlane + 0.001f));
            DirectionalShadowConfig shadowConfig{
                .resolution = config_.shadowSettings.directionalResolution,
                .splitLambda = config_.shadowSettings.directionalSplitLambda,
                .guardBandFraction =
                    config_.shadowSettings.directionalGuardBandFraction,
                .depthPaddingMeters =
                    config_.shadowSettings.directionalDepthPaddingMeters,
            };
            directionalShadows.reserve(shadowSelections.size());
            uint32_t dirtyCascades = 0;
            uint32_t casterInvalidatedCascades = 0;
            uint32_t updatedCascades = 0;
            uint32_t cachedCascades = 0;
            for (uint32_t shadowIndex = 0;
                shadowIndex < shadowSelections.size(); ++shadowIndex) {
                const DirectionalShadowSelection& selection =
                    shadowSelections[shadowIndex];
                const DirectionalShadowCascadePlan plan =
                    buildDirectionalShadowCascades(shadowCamera,
                        selection.lightForward, shadowConfig);
                const uint64_t lightRevision = selection.lightSlot <
                    lightingFrame.recordRevisions.size()
                    ? lightingFrame.recordRevisions[selection.lightSlot] : 0;
                std::array<uint64_t, kDirectionalShadowCascadeCount>
                    casterRevisions{};
                {
                    CpuScope revisionScope(cpuProfiler_,
                        "cpu.shadow.caster_revision.directional");
                    casterRevisions = renderBackend->
                        getDirectionalShadowCasterRevisions(
                            shadowCasters, plan);
                }
                const DirectionalShadowSchedule schedule =
                    directionalShadowCaches_[shadowIndex].schedule({
                        .selection = selection,
                        .plan = plan,
                        .lightRevision = lightRevision,
                        .casterRevisions = casterRevisions,
                        .pipelineRevision = 1,
                    },
                        config_.shadowSettings.maximumCascadeUpdatesPerLight);
                directionalShadows.push_back(DirectionalShadowFramePacket{
                    .selection = selection,
                    .plan = plan,
                    .shadowIndex = shadowIndex,
                    .updateMask = schedule.updateMask,
                    .sampleableMask = schedule.sampleableMask,
                    .resolution = shadowConfig.resolution,
                    .sourceAngularDiameterDegrees = config_.shadowSettings.
                        directionalSourceAngularDiameterDegrees,
                    .receiverDepthBiasTexels = config_.shadowSettings.
                        directionalReceiverDepthBiasTexels,
                    .receiverPlaneClampTexels = config_.shadowSettings.
                        directionalReceiverPlaneClampTexels,
                    .normalOffsetTexels = config_.shadowSettings.
                        directionalNormalOffsetTexels,
                    .filterProfile = effectiveShadowFilterProfile(
                        config_.shadowSettings, selection.quality),
                });
                dirtyCascades += schedule.invalidatedCount;
                casterInvalidatedCascades +=
                    schedule.casterInvalidatedCount;
                updatedCascades += std::popcount(schedule.updateMask);
                cachedCascades += schedule.cacheHitCount;
            }
            for (uint32_t shadowIndex = static_cast<uint32_t>(
                    shadowSelections.size());
                shadowIndex < directionalShadowCaches_.size(); ++shadowIndex)
                directionalShadowCaches_[shadowIndex].reset();
            activeDirectionalShadowSelection_ = shadowSelections.front();
            activeDirectionalShadowSampleableMask_ =
                directionalShadows.front().sampleableMask;
            activeDirectionalShadowOwnerCount_ = static_cast<uint32_t>(
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.requested",
                shadowSelections.size());
            cpuProfiler_.recordCounter("shadow.directional.omitted",
                shadowSelections.front().omittedShadowDirectionalLights);
            cpuProfiler_.recordCounter("shadow.directional.cascades.dirty",
                dirtyCascades);
            cpuProfiler_.recordCounter(
                "shadow.directional.cascades.caster_invalidated",
                casterInvalidatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.updated",
                updatedCascades);
            cpuProfiler_.recordCounter("shadow.directional.cascades.cached",
                cachedCascades);
            const auto fixedMillionths = [](float value) {
                return static_cast<uint64_t>(std::llround(
                    static_cast<double>(value) * 1'000'000.0));
            };
            cpuProfiler_.recordCounter("shadow.directional.coverage_distance_m",
                fixedMillionths(shadowCamera.farPlane),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_depth_bias_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalReceiverDepthBiasTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.receiver_plane_clamp_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalReceiverPlaneClampTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            cpuProfiler_.recordCounter(
                "shadow.directional.normal_offset_texels",
                fixedMillionths(config_.shadowSettings.
                    directionalNormalOffsetTexels),
                ProfileCounterStatus::Exact, ProfileCounterUnit::Millionths);
            constexpr std::array<const char*, 4> splitCounterNames{
                "shadow.directional.cascade0.split_far_m",
                "shadow.directional.cascade1.split_far_m",
                "shadow.directional.cascade2.split_far_m",
                "shadow.directional.cascade3.split_far_m" };
            constexpr std::array<const char*, 4> densityCounterNames{
                "shadow.directional.cascade0.world_units_per_texel_m",
                "shadow.directional.cascade1.world_units_per_texel_m",
                "shadow.directional.cascade2.world_units_per_texel_m",
                "shadow.directional.cascade3.world_units_per_texel_m" };
            const DirectionalShadowCascadePlan& diagnosticPlan =
                directionalShadows.front().plan;
            for (uint32_t cascade = 0;
                    cascade < kDirectionalShadowCascadeCount; ++cascade) {
                cpuProfiler_.recordCounter(splitCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].splitFar),
                    ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
                cpuProfiler_.recordCounter(densityCounterNames[cascade],
                    fixedMillionths(diagnosticPlan.cascades[cascade].
                        worldUnitsPerTexel), ProfileCounterStatus::Exact,
                    ProfileCounterUnit::Millionths);
            }
        }
        else {
            for (DirectionalShadowCache& cache : directionalShadowCaches_)
                cache.reset();
            activeDirectionalShadowSelection_.reset();
            activeDirectionalShadowSampleableMask_ = 0;
            activeDirectionalShadowOwnerCount_ = 0;
            cpuProfiler_.recordCounter("shadow.directional.requested", 0);
        }
        }
        renderFrame.directionalShadows = { shadowCasters, directionalShadows };
        frameStage_ = { .frame = &frame, .directionalShadows = directionalShadows };

        // Spot shadows share the same extracted light slots and caster revision
        // as clustered lighting. Stable atlas allocation is reconciled before
        // cache scheduling so compatible tiles remain sampleable across frames.
        uint64_t localCasterRevision = 0;
        std::vector<SpotShadowFramePacket> spotShadows;
        std::vector<PointShadowFramePacket> pointShadows;
        {
        CpuScope localScope(cpuProfiler_, "cpu.shadow.local.schedule");
        const std::vector<LocalShadowRequest> localShadowRequests =
            buildLocalShadowRequests(lightingFrame, renderCameraPosition);
        const LocalShadowAllocationStats spotAllocation =
            spotShadowAtlas_.reconcile(localShadowRequests);
        spotShadowCache_.configure({
            .maximumRenderedTexels = config_.shadowSettings.
                maximumSpotRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = config_.shadowSettings.
                maximumCompatibleSpotStaleFrames,
        });
        {
            CpuScope revisionScope(cpuProfiler_,
                "cpu.shadow.caster_revision.local");
            localCasterRevision =
                renderBackend->getShadowCasterRevision(shadowCasters);
        }
        std::vector<LocalShadowCacheInput> spotCacheInputs;
        spotCacheInputs.reserve(spotShadowAtlas_.allocations().size());
        for (const SpotShadowTile& tile : spotShadowAtlas_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Spot &&
                        candidate.owner == tile.owner;
                });
            if (request == localShadowRequests.end() ||
                tile.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const std::array<uint32_t, 5> allocationIdentity{
                tile.x, tile.y, tile.size, tile.guardTexels,
                config_.shadowSettings.spotAtlasResolution };
            spotCacheInputs.push_back({
                .request = *request,
                .resolution = tile.size,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[tile.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(
                    projection.worldToShadowClip),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& spotSchedule =
            spotShadowCache_.schedule(spotCacheInputs);
        spotShadows.reserve(spotSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : spotSchedule.entries) {
            const auto tile = std::ranges::find_if(
                spotShadowAtlas_.allocations(),
                [&](const SpotShadowTile& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (tile == spotShadowAtlas_.allocations().end() ||
                tile->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[tile->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const SpotShadowProjection projection = buildSpotShadowProjection(
                glm::vec3(light.positionRange),
                glm::vec3(light.directionOuterCos),
                light.directionOuterCos.w, nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(spotShadowAtlas_.allocations().begin(), tile));
            if (shadowDataSlot >= kSpotShadowEntryCapacity) continue;
            spotShadows.push_back({
                .owner = entry.owner,
                .worldToShadowClip = projection.worldToShadowClip,
                .lightSlot = tile->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .atlasX = tile->x,
                .atlasY = tile->y,
                .tileSize = tile->size,
                .guardTexels = tile->guardTexels,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    config_.shadowSettings,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            });
        }
        renderFrame.spotShadows = { shadowCasters, spotShadows };
        frameStage_.spotAllocation = spotAllocation;
        frameStage_.spotSchedule = &spotSchedule;

        // Point lights use stable tiered cube slots. Cache publication is
        // all-or-nothing across the frozen six-face orientation so lighting can
        // never sample a partially refreshed cube.
        const LocalShadowAllocationStats pointAllocation =
            pointShadowPools_.reconcile(localShadowRequests);
        pointShadowCache_.configure({
            .maximumRenderedTexels = config_.shadowSettings.
                maximumPointRenderedTexelsPerFrame,
            .maximumCompatibleStaleFrames = config_.shadowSettings.
                maximumCompatiblePointStaleFrames,
        });
        std::vector<LocalShadowCacheInput> pointCacheInputs;
        pointCacheInputs.reserve(pointShadowPools_.allocations().size());
        for (const PointShadowSlot& slot : pointShadowPools_.allocations()) {
            const auto request = std::ranges::find_if(localShadowRequests,
                [&](const LocalShadowRequest& candidate) {
                    return candidate.kind == LocalShadowKind::Point &&
                        candidate.owner == slot.owner;
                });
            if (request == localShadowRequests.end() ||
                slot.lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot.lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            if (!(farPlane > nearPlane)) continue;
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            std::array<glm::mat4, 6> matrices{};
            for (uint32_t face = 0; face < matrices.size(); ++face)
                matrices[face] = faces[face].worldToShadowClip;
            const std::array<uint32_t, 2> allocationIdentity{
                slot.resolution, slot.cubeIndex };
            pointCacheInputs.push_back({
                .request = *request,
                .resolution = slot.resolution,
                .allocationRevision = shadowRevision(allocationIdentity),
                .lightRevision = lightingFrame.recordRevisions[slot.lightSlot],
                .casterRevision = localCasterRevision,
                .projectionRevision = shadowRevision(matrices),
                .pipelineRevision = 1,
            });
        }
        const LocalShadowSchedule& pointSchedule =
            pointShadowCache_.schedule(pointCacheInputs);
        pointShadows.reserve(pointSchedule.entries.size());
        for (const LocalShadowScheduleEntry& entry : pointSchedule.entries) {
            const auto slot = std::ranges::find_if(
                pointShadowPools_.allocations(),
                [&](const PointShadowSlot& candidate) {
                    return candidate.owner == entry.owner;
                });
            if (slot == pointShadowPools_.allocations().end() ||
                slot->lightSlot >= lightingFrame.records.size()) continue;
            const PackedGpuLight& light = lightingFrame.records[slot->lightSlot];
            const float farPlane = light.positionRange.w;
            const float nearPlane = (std::max)(0.001f,
                (std::min)(0.05f, farPlane * 0.01f));
            const auto faces = buildPointShadowFaces(
                glm::vec3(light.positionRange), nearPlane, farPlane);
            const uint32_t shadowDataSlot = static_cast<uint32_t>(
                std::distance(pointShadowPools_.allocations().begin(), slot));
            if (shadowDataSlot >= kPointShadowEntryCapacity) continue;
            PointShadowFramePacket packet{
                .owner = entry.owner,
                .lightPosition = glm::vec3(light.positionRange),
                .nearPlane = nearPlane,
                .farPlane = farPlane,
                .lightSlot = slot->lightSlot,
                .shadowDataSlot = shadowDataSlot,
                .resolution = slot->resolution,
                .cubeIndex = slot->cubeIndex,
                .update = entry.update,
                .sampleable = entry.sampleable,
                .stale = entry.stale,
                .staleAgeFrames = entry.staleAgeFrames,
                .sourceRadiusMeters = light.shapeMetadata.x,
                .filterProfile = effectiveShadowFilterProfile(
                    config_.shadowSettings,
                    (std::bit_cast<uint32_t>(light.shapeMetadata.z) &
                        PackedGpuLightShadowQualityMask) >>
                        PackedGpuLightShadowQualityShift),
            };
            for (uint32_t face = 0; face < packet.worldToShadowClip.size();
                ++face)
                packet.worldToShadowClip[face] =
                    faces[face].worldToShadowClip;
            pointShadows.push_back(packet);
        }
        renderFrame.pointShadows = { shadowCasters, pointShadows };
        frameStage_.pointAllocation = pointAllocation;
        frameStage_.pointSchedule = &pointSchedule;
        }

        // Scene probes must never capture the isolated model or its preview sun.
        if (!assetPreviewActive) {
        CpuScope captureScope(cpuProfiler_, "cpu.probe.capture.schedule");
        std::vector<ReflectionProbeCaptureRequest> probeCaptureRequests;
        probeCaptureRequests.reserve(extractedProbes.candidates.size());
        uint64_t environmentRevision = 1469598103934665603ull;
        for (char character : assets_.activeEnvironmentCookKey()) {
            environmentRevision ^= static_cast<uint8_t>(character);
            environmentRevision *= 1099511628211ull;
        }
        if (environmentRevision == 0u) environmentRevision = 1u;
        const uint64_t lightingRevision =
            reflectionProbeLightingRevision(lightingFrame);
        for (const ReflectionProbeCandidate& candidate :
                extractedProbes.candidates) {
            if (!candidate.probe.environmentAssetGuid.isNil()) continue;
            probeCaptureRequests.push_back({
                .owner = candidate.owner,
                .updateMode = candidate.probe.updateMode,
                .position = glm::vec3(candidate.probeToWorld[3]),
                .resolution = static_cast<uint32_t>(
                    candidate.probe.captureResolution),
                .nearPlane = candidate.probe.captureNearMeters,
                .farPlane = candidate.probe.captureFarMeters,
                .priority = candidate.probe.priority,
                .captureSky = candidate.probe.captureSky,
                .settingsRevision = reflectionProbeSettingsRevision(candidate),
                .explicitRequestRevision =
                    candidate.probe.explicitCaptureRevision,
                .sceneRevision = localCasterRevision,
                .lightingRevision = lightingRevision,
                .environmentRevision = environmentRevision,
                .pipelineRevision = 1,
                .frameIndex = applicationFrameIndex,
            });
        }
        const ReflectionProbeCaptureSchedule& probeCaptureSchedule =
            reflectionProbeCaptureScheduler_.schedule(probeCaptureRequests);
        renderFrame.submitReflectionProbeCaptures = true;
        renderFrame.probeCasters = probeCasters;
        renderFrame.probeCaptureSchedule = probeCaptureSchedule.entries;
        frameStage_.probeCaptureSchedule = &probeCaptureSchedule;
        }
        // Keep scene-probe resources resident, but exclude their local influence
        // from the isolated preview. Tag the active-list identity across views.
        publishedProbes.activeListRevision = publishedProbes.activeListRevision * 2u + (assetPreviewActive ? 1u : 0u);
        if (assetPreviewActive) {
            publishedProbes.activeSlots = {};
            publishedProbes.stats.activeProbeCount = 0;
        }
        // Pass 1: Opaque G-Buffer
        bool isWireframe = config_.forceWireframe || view.wireframe;
        const std::span<const DrawPacket> activeSelectionQueue =
            debugView == RenderDebugView::Final
            ? std::span<const DrawPacket>(selectionQueue.data(), selectionQueue.size())
            : std::span<const DrawPacket>{};
        renderFrame.opaqueQueue = opaqueQueue;
        renderFrame.selectionQueue = activeSelectionQueue;
        renderFrame.wireframe = isWireframe;
        renderFrame.forwardOpaqueQueue = forwardOpaqueQueue;
        renderFrame.sortedSurfaceQueue = sortedSurfaceQueue;
        renderFrame.compatibilityTransparentQueue = transparentQueue;
        renderFrame.instanceTransforms = forwardInstanceTransforms_;
        renderFrame.lights = &lightingFrame;
        renderFrame.reflectionProbes = &publishedProbes;
        renderFrame.stageObserver = this;

        // Shadows, probe captures, G-buffer, lighting, forward and
        // transparency, output and UI. The observer's scene-linear and
        // output submit points are reported from the stage boundaries.
        renderBackend->submitFrame(renderFrame);
        frameStage_ = {};

        if (renderBackend->endFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (dualViews) editorViewScheduler_.rendered(renderView, EditorViewCadence::Clock::now());
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
        editor.cleanup();
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
        if (!app->editor.getAssetViewerPanel().isFocused &&
            (app->editor.getViewportPanel().isHovered || glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) &&
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
        if (app->editor.getAssetViewerPanel().isFocused || !app->editor.getViewportPanel().isHovered) return;

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
            ((!app->editor.getAssetViewerPanel().isFocused && app->editor.getViewportPanel().isHovered) || action == GLFW_RELEASE)) {
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
        if (editor.getAssetViewerPanel().isFocused && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (!editor.getAssetViewerPanel().isFocused && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED &&
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

    std::shared_ptr<ModelAsset> Application::resolveEditorAssetPreview() {
        AssetManager* const assetManager = assets_.assetManager();
        const std::shared_ptr<ModelAsset>& mainModel = assets_.mainModel();
        AssetRuntimeService* const assetRuntimeService_ = assets_.runtime();
        AssetModelPreparationService* const assetModelPreparationService_ =
            assets_.modelPreparation();
        AssetCatalog* const assetCatalog_ = assets_.catalog();
        const EditorAssetDocument* document =
            editor.assetDocuments().active();
        if (!document || !assetManager) {
            framedPreviewDocumentGuid_ = {};
            framedPreviewCookKey_.clear();
            return {};
        }

        const AssetGuid presentationGuid = document->presentationAssetGuid;
        std::shared_ptr<ModelAsset> model =
            assetManager->findMaterialPreview(document->assetGuid);
        if (!model) model = assetManager->findCookedModel(presentationGuid);
        if (!model && mainModel && mainModel->assetGuid == presentationGuid) {
            model = mainModel;
        }
        if (model) {
            if (assetRuntimeService_) {
                assetRuntimeService_->touch(
                    presentationGuid, measuredFrameCount_ + 1);
            }
            if (framedPreviewDocumentGuid_ != document->assetGuid ||
                framedPreviewSession_ != document->sessionSerial ||
                framedPreviewCookKey_ != model->artifactCookKey ||
                framedPreviewRevision_ != document->framingRevision) {
                glm::vec3 minimum(
                    (std::numeric_limits<float>::max)());
                glm::vec3 maximum(
                    (std::numeric_limits<float>::lowest)());
                bool hasBounds = false;
                for (const SubMesh& subMesh : model->subMeshes) {
                    if (document->isolateSelectedPart && document->selectedPart &&
                        *document->selectedPart != (document->selectedPartIsMaterial
                            ? subMesh.materialGuid : subMesh.sourcePrimitiveGuid)) continue;
                    minimum = glm::min(minimum, subMesh.boundsMin);
                    maximum = glm::max(maximum, subMesh.boundsMax);
                    hasBounds = true;
                }
                if (!hasBounds) {
                    minimum = glm::vec3(-1.0f);
                    maximum = glm::vec3(1.0f);
                }
                const float aspect = renderExtent_.height != 0
                    ? static_cast<float>(renderExtent_.width) /
                        static_cast<float>(renderExtent_.height)
                    : 1.0f;
                editor.getAssetViewerPanel().frameActiveBounds(
                    minimum, maximum, aspect);
                framedPreviewDocumentGuid_ = document->assetGuid;
                framedPreviewCookKey_ = model->artifactCookKey;
                framedPreviewRevision_ = document->framingRevision;
                framedPreviewSession_ = document->sessionSerial;
            }
            return model;
        }

        if (!assetModelPreparationService_ || !assetCatalog_) return {};
        if (assetModelPreparationService_->pending(presentationGuid)) return {};
        if (assetRuntimeService_) {
            const auto snapshot = assetRuntimeService_->snapshot(presentationGuid);
            if (snapshot &&
                (snapshot->state == RuntimeAssetState::Queued ||
                 snapshot->state == RuntimeAssetState::Failed)) {
                return {};
            }
        }
        const std::vector<AssetCatalogRecord> records =
            assetCatalog_->recordsForGuid(presentationGuid);
        const auto record = std::ranges::find_if(records,
            [](const AssetCatalogRecord& candidate) {
                return !candidate.parentGuid &&
                    candidate.assetType == "iridium.model" &&
                    candidate.assetRoot == "project" &&
                    candidate.status == AssetCatalogStatus::Ready;
            });
        if (record != records.end()) {
            try {
                (void)assetModelPreparationService_->request(*record);
            }
            catch (const std::exception& exception) {
                if (assetRuntimeService_) {
                    assetRuntimeService_->reportFailure(
                        presentationGuid, exception.what());
                }
            }
        }
        return {};
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
        editor.setOutputTransportStatus(
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
