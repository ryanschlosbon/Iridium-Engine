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
#include "core/ApplicationConfig.h"
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
#include "ecs/systems/TransformSystem.h"

// --- THE NEW RENDERING ARCHITECTURE ---
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/DrawPacket.h"
#include "benchmarks/BenchmarkManifest.h"
#include "renderer/rhi/FrameCapture.h"
#include "renderer/rhi/RenderBackendRuntimeInfo.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/ReflectionProbe.h"
#include "renderer/lighting/ReflectionProbeCapture.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/lighting/LocalShadow.h"

namespace Iridium {

    class Application {
    public:
        explicit Application(ApplicationConfig config = {});
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
        std::vector<DrawPacket> shadowCasterQueue;

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
        std::map<AssetGuid, LoadedEnvironmentAsset> loadedEnvironments_;
        TextureHandle outputTransformLut; // Pinned application-owned ACES 2 LUT.
        TextureHandle residencyProbeTexture_{};
        TextureHandle residencyReplacementTexture_{};
        uint32_t residencyRetiredIndex_ = UINT32_MAX;
        std::vector<std::byte> residencyProbePixels_;
        std::vector<TextureHandle>
            textureScaleProbeTextures_;
        TextureHandle materialScaleProbeTexture_{};
        std::vector<MaterialHandle>
            materialScaleProbeMaterials_;

        // --- CAMERA STATE ---
        float yaw = -90.0f;
        float pitch = 0.0f;
        float mouseSensitivity = 0.1f;
        glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f);
        glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
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
        struct Ordinary2ResizeValidationState {
            RenderExtent originalExtent{};
            uint64_t initialRenderGraphRebuildCount = 0;
            uint32_t requests = 0;
            uint32_t successes = 0;
            uint32_t failures = 0;
            std::string lastDiagnostic;
        } ordinary2ResizeValidation_;
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
        } deepLayeredLifecycleValidation_;
        struct StartupProfile {
            uint64_t totalNanoseconds = 0;
            uint64_t windowNanoseconds = 0;
            uint64_t backendNanoseconds = 0;
            uint64_t editorNanoseconds = 0;
            uint64_t manifestVerificationNanoseconds = 0;
            uint64_t modelLoadNanoseconds = 0;
            uint64_t environmentCreationNanoseconds = 0;
            uint64_t sceneConstructionNanoseconds = 0;
            uint64_t frameTopologyPrewarmNanoseconds = 0;
        } startupProfile_;
        std::optional<BenchmarkFixture> activeBenchmark_;
        std::string benchmarkManifestPath_;
        std::string benchmarkManifestSha256_;
        std::optional<FrameCapture> completedCapture_;
        std::optional<uint64_t> capturedApplicationFrameIndex_;
        struct BenchmarkInstanceState {
            Entity entity = NULL_ENTITY;
            glm::vec3 basePosition{ 0.0f };
        };
        std::vector<BenchmarkInstanceState> benchmarkInstances_;
        float verticalFovDegrees_ = 45.0f;
        float cameraNearPlane_ = 0.1f;
        float cameraFarPlane_ = 100.0f;
        AssetGuid framedPreviewDocumentGuid_;
        std::string framedPreviewCookKey_;

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
        void cleanup();

        void drawFrame(std::optional<uint64_t> captureFrameIndex,
            uint64_t applicationFrameIndex,
            bool validateOrdinary2Capture,
            bool validateDeepLayeredCapture);

        void processInput(GLFWwindow* window);
        void selectEntityAtMouse(double mouseX, double mouseY);
        // Failed requests are reported once and cleared; callers must explicitly retry.
        void ProcessMeshSwaps();
        [[nodiscard]] std::shared_ptr<ModelAsset>
            resolveEditorAssetPreview();
        void configureCookedModelHotReload();
        void configureCookedEnvironmentHotReload();
        void recreateSwapchain();
        void updateBenchmarkState(uint64_t frameIndex);
        void updateOrdinary2ResizeValidation(uint64_t measuredFrameIndex);
        [[nodiscard]] bool updateDeepLayeredLifecycleValidation(
            uint64_t measuredFrameIndex);
        void updateTextureResidencyChurn(uint64_t frameIndex);
    };

} // namespace Iridium
