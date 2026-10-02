#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h> 
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <array>
#include <memory>
#include <string>
#include <optional>
#include <map>

// --- ENGINE SUBSYSTEMS ---
#include "app/ApplicationConfig.h"
#include "app/FrameObserver.h"
#include "core/EngineLog.h"
#include "profiling/CpuProfiler.h"
#include "assets/AssetManager.h"  
#include "assets/AssetCatalog.h"
#include "assets/AssetCatalogService.h"
#include "assets/model/AssetModelPreparationService.h"
#include "assets/environment/AssetEnvironmentPreparationService.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "assets/thumbnail/AssetThumbnailUploadQueue.h"
#include "scene/SceneWorld.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorTransactionService.h"
#include "editor/EditorSystem.h"
#include "editor/ViewportRenderExtent.h"
#include "scene/systems/TransformSystem.h"

// --- THE NEW RENDERING ARCHITECTURE ---
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"
#include "renderer/lighting/LightExtractor.h"
#include "editor/EditorViewCadence.h"
#include "renderer/lighting/ReflectionProbe.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LocalShadow.h"
#include "renderer/scene/GpuScenePublisher.h"
#include "renderer/rhi/GpuSceneVisibility.h"

namespace Iridium {

    class Application final : private IAppControl {
    public:
        // The observer (the qualification harness in IRIDIUM_QUALIFICATION
        // builds) is optional and must outlive run(); without one the
        // Application runs the interactive editor.
        explicit Application(ApplicationConfig config = {},
            IFrameObserver* observer = nullptr);
        void run();

        // GLFW Callbacks must be static
        static void mouse_callback(GLFWwindow* window, double xposIn, double yposIn);
        static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
        static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
        static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

        bool wasWindowResized() const { return framebufferResized; }
        void resetWindowResizedFlag() { framebufferResized = false; }

    private:
        [[nodiscard]] std::string persistBakedReflectionProbe(
            SceneEntityUuid owner,
            const ReflectionProbeCaptureCompletion::Product& product);

        // --- CORE ENGINE STATE ---
        ApplicationConfig config_;
        EngineLog engineLog_;
        CpuProfiler cpuProfiler_;
        GLFWwindow* window = nullptr;
        bool glfwInitialized_ = false;
        bool framebufferResized = false;
        IFrameObserver* observer_ = nullptr;
        AppRunPolicy policy_{};
        AppFrameRequests frameRequests_{};

        // --- THE GRAPHICS ABSTRACTION ---
        // This single pointer replaces 40+ Vulkan variables!
        std::unique_ptr<IRenderBackend> renderBackend;

        // The Data-Driven Extraction Queues
        std::vector<DrawPacket> opaqueQueue;
        std::vector<DrawPacket> forwardOpaqueQueue;
        std::vector<DrawPacket> transparentQueue;
        std::vector<DrawPacket> sortedSurfaceQueue;
        std::vector<TransparentIntervalEndpoint>
            transparentIntervalEndpointScratch;
        std::vector<float> transparentIntervalNearScratch;
        std::vector<uint32_t> transparentIntervalFenwickScratch;
        std::vector<DrawPacket> selectionQueue;
        // Dense GPU-scene primitive references are the production shadow path;
        // shadowCasterQueue contains compatibility packets only.
        std::vector<DrawPacket> shadowCasterQueue;
        std::vector<DrawPacket> probeCasterQueue_;
        // Backend-neutral, frame-local transforms referenced by DrawPacket
        // instance ranges. Existing single-instance packets leave this empty.
        std::vector<glm::mat4> forwardInstanceTransforms_;
        std::vector<Entity> changedTransformEntities_;
        std::unique_ptr<GpuScenePublisher> gpuScenePublisher_;
        std::vector<GpuSceneObservedInstance> gpuSceneObservations_;
        struct GpuSceneObservationMetadata {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            GeometryHandle geometry;
            std::string cookKey;
            uint64_t materialOverrideSignature = 0;
            uint64_t observationRevision = 0;
            glm::vec3 localMinimum{ 0.0f };
            glm::vec3 localMaximum{ 0.0f };
            uint32_t baseConsumerMask = 0;
            bool valid = false;
        };
        std::vector<GpuSceneObservationMetadata>
            gpuSceneObservationMetadata_;
        const GpuScenePackedTables* gpuSceneFrame_ = nullptr;
        GpuSceneVisibilityResult gpuSceneVisibility_;
        uint32_t gpuSceneDirectFallbackCount_ = 0;

        // --- SUBSYSTEMS ---
        std::unique_ptr<AssetManager> assetManager;
        std::unique_ptr<AssetCatalog> assetCatalog_;
        std::unique_ptr<AssetCatalogService> assetCatalogService_;
        std::shared_ptr<LocalDerivedDataCache>
            editorModelDdc_;
        std::unique_ptr<AssetModelPreparationService>
            assetModelPreparationService_;
        std::unique_ptr<AssetEnvironmentPreparationService>
            assetEnvironmentPreparationService_;
        std::unique_ptr<AssetThumbnailService>
            assetThumbnailService_;
        std::unique_ptr<AssetRuntimeService>
            assetRuntimeService_;
        AssetThumbnailUploadQueue
            pendingThumbnailUploads_;
        uint64_t thumbnailUploadsTotal_ = 0;
        uint64_t thumbnailUploadBytesTotal_ = 0;
        SceneWorld sceneWorld_;
        SceneWorld previewLightingWorld_;
        Entity previewSun_ = NULL_ENTITY;
        EditorViewScheduler editorViewScheduler_;
        LightExtractor lightExtractor_;
        ReflectionProbePublisher reflectionProbePublisher_;
        ReflectionProbeCaptureScheduler reflectionProbeCaptureScheduler_;
        std::vector<EnvironmentLightingHandles>
            reflectionProbeEnvironments_;
        std::array<DirectionalShadowCache,
            kDirectionalShadowLightCapacity> directionalShadowCaches_;
        StableSpotShadowAtlas spotShadowAtlas_;
        LocalShadowCacheScheduler spotShadowCache_;
        StablePointShadowPools pointShadowPools_;
        LocalShadowCacheScheduler pointShadowCache_;
        std::optional<DirectionalShadowSelection>
            activeDirectionalShadowSelection_;
        uint32_t activeDirectionalShadowSampleableMask_ = 0;
        uint32_t activeDirectionalShadowOwnerCount_ = 0;
        EditorSceneDocumentService sceneDocumentService_;
        EditorTransactionService transactionService_;
        Registry& registry;
        TransformSystem transformSystem;
        EditorSystem editor;

        // --- SCENE DATA ---
        std::shared_ptr<ModelAsset> mainModel;
        std::filesystem::path
            activeCookedModelArtifact_;
        std::filesystem::path activeCookedEnvironmentArtifact_;
        AssetGuid activeEnvironmentAssetGuid_;
        AssetGuid activeEnvironmentSourceGuid_;
        std::string activeEnvironmentCookKey_;
        std::string activeEnvironmentSourcePrimaries_;
        float activeEnvironmentRadianceScale_ = 0.0f;
        EnvironmentLightingHandles environmentLighting_;
        EnvironmentLightingSettings sceneEnvironmentSettings_;
        std::optional<float> appliedViewExposureEv_;
        std::map<AssetGuid, LoadedEnvironmentAsset> loadedEnvironments_;
        TextureHandle outputTransformLut; // Pinned application-owned ACES 2 LUT.
        Color::OutputTransport outputTransformLutTransport_ =
            Color::OutputTransport::SdrSrgb;
        std::optional<Color::OutputTransport> pendingOutputTransport_;
        uint64_t outputTransportSwitchCount_ = 0;

        // --- CAMERA STATE ---
        float yaw = -90.0f;
        float pitch = 0.0f;
        float mouseSensitivity = 0.1f;
        AppCamera camera_{};
        float cameraSpeed = 2.5f;
        float deltaTime = 0.0f;
        uint64_t measuredFrameCount_ = 0;
        uint64_t measurementWallNanoseconds_ = 0;
        uint64_t changedTransformsThisFrame_ = 0;
        RenderExtent renderExtent_{};
        ViewportRenderExtentPolicy viewportExtentPolicy_;
        std::string viewportExtentDiagnostic_;
        RenderBackendCapabilities renderCapabilities_{};
        RenderBackendRuntimeInfo renderRuntimeInfo_{};
        AppStartupTimings startupProfile_;
        bool measurementStarted_ = false;
        AssetGuid framedPreviewDocumentGuid_;
        std::string framedPreviewCookKey_;
        uint64_t framedPreviewRevision_ = 0;
        uint64_t framedPreviewSession_ = 0;

        // --- MOUSE STATE ---
        float lastX = 1280 / 2.0f;
        float lastY = 720 / 2.0f;
        bool firstMouse = true;
        bool isRightMouseButtonDown = false;
        bool isMiddleMouseButtonDown = false;

        // --- INTERNAL FUNCTIONS ---
        void initWindow();
        void initRenderer(); // Formerly initVulkan()
        void mainLoop();
        void cleanup(bool completed);

        void drawFrame(AppFrameContext& frame);
        void prepareGpuScenePublication(Entity selectedEntity);

        void processInput(GLFWwindow* window);
        // Failed requests are reported once and cleared; callers must explicitly retry.
        void ProcessMeshSwaps();
        [[nodiscard]] std::shared_ptr<ModelAsset>
            resolveEditorAssetPreview();
        void configureCookedModelHotReload();
        void configureCookedEnvironmentHotReload();
        void recreateSwapchain();
        void replaceOutputTransformLut(Color::OutputTransport effectiveTransport);
        void publishOutputTransportStatus();
        OutputTransportSwitchResult switchOutputTransport(
            Color::OutputTransport requested);
        [[nodiscard]] AppRunSnapshot makeRunSnapshot() const;
        void notifyStartup(StartupPhase phase, AppStartupContext& context);
        void notifyShutdown(ShutdownPhase phase, bool completed);

        // IAppControl
        OutputTransportSwitchResult applyOutputTransport(
            Color::OutputTransport transport) override;
        [[nodiscard]] bool resizeSceneExtent(RenderExtent requested,
            std::string& diagnostic) override;
        [[nodiscard]] RenderExtent renderExtent() const override {
            return renderExtent_;
        }
        std::shared_ptr<ModelAsset> loadCookedStartupModel() override;
        void loadCookedStartupEnvironment() override;
        void publishStartupEnvironment(
            LoadedEnvironmentAsset environment) override;
    };

} // namespace Iridium
