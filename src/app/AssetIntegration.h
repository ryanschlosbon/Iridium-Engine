#pragma once

// M7R R5a.1 (design section 3.1): the Application's asset integration. It owns
// the asset manager and the asset services (catalog, model/environment
// preparation, thumbnails, runtime publication), drains their results once per
// frame (asset swaps, the runtime tick and their counters), owns the startup
// model and the loaded environments, persists baked reflection probes and
// configures cooked hot reload. No ImGui.

#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "app/ApplicationConfig.h"
#include "app/FrameObserver.h"
#include "assets/AssetCatalog.h"
#include "assets/AssetCatalogService.h"
#include "assets/AssetManager.h"
#include "assets/environment/AssetEnvironmentPreparationService.h"
#include "assets/model/AssetModelPreparationService.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "assets/thumbnail/AssetThumbnailUploadQueue.h"
#include "renderer/rhi/ReflectionProbeCapture.h"
#include "scene/SceneWorld.h"

namespace Iridium {

    class CpuProfiler;
    class EditorAssetDocumentService;
    class EditorSceneDocumentService;
    class EngineLog;
    class IEditorRenderBridge;
    class IRenderBackend;
    class LocalDerivedDataCache;
    namespace Tasks { class TaskSystem; }

    class AssetIntegration final {
    public:
        // The task system runs every asset service's work (M7R R5b.2).
        AssetIntegration(ApplicationConfig& config, CpuProfiler& profiler,
            EngineLog& log, Tasks::TaskSystem& tasks, SceneWorld& scene,
            EditorSceneDocumentService& sceneDocuments);
        ~AssetIntegration();

        AssetIntegration(const AssetIntegration&) = delete;
        AssetIntegration& operator=(const AssetIntegration&) = delete;

        // --- Startup (Application::initRenderer order) ---
        // The asset manager over the backend; qualification probe allocations
        // (BackendReady) follow it and precede every asset service.
        AssetManager& createAssetManager(IRenderBackend& backend,
            IEditorRenderBridge* editorBridge,
            TransparencyExecutionMode transparencyExecutionMode,
            uint32_t gpuLodMinimumResidentLevel);
        // The runtime service, and outside deterministic runs the catalog and
        // the preparation/thumbnail services over the editor DDC.
        void startServices(bool deterministicContent);
        // Interactive startup content: the built-in cube or
        // --cooked-model-artifact, --cooked-environment-artifact and their
        // hot reload. Returns the startup model's GUID.
        [[nodiscard]] AssetGuid loadInteractiveStartupContent(
            AppStartupTimings& timings);
        // IAppControl startup content.
        std::shared_ptr<ModelAsset> loadCookedStartupModel();
        void loadCookedStartupEnvironment();
        void publishStartupEnvironment(LoadedEnvironmentAsset environment);

        // --- Per frame ---
        // Preparation results, thumbnails and the probe/sky/mesh asset
        // requests. Failed requests are reported once and cleared; callers
        // must explicitly retry.
        void processMeshSwaps();
        // AssetRuntimeService::tick and the asset.* counters.
        void tickRuntime();
        // A finalized runtime capture: the probe component's diagnostic, and the
        // baked product when the capture was a bake.
        void publishCaptureCompletion(
            const ReflectionProbeCaptureCompletion& completion);
        // Material previews of the open asset documents.
        void processMaterialPreviews(
            const EditorAssetDocumentService& documents);

        // Cleanup: services, thumbnails, environments and the asset manager,
        // in the Application's former order.
        void shutdown();

        // --- State ---
        [[nodiscard]] AssetManager* assetManager() const noexcept {
            return assetManager_.get();
        }
        [[nodiscard]] AssetCatalog* catalog() const noexcept {
            return assetCatalog_.get();
        }
        [[nodiscard]] AssetCatalogService* catalogService() const noexcept {
            return assetCatalogService_.get();
        }
        [[nodiscard]] AssetModelPreparationService*
            modelPreparation() const noexcept {
            return assetModelPreparationService_.get();
        }
        [[nodiscard]] AssetEnvironmentPreparationService*
            environmentPreparation() const noexcept {
            return assetEnvironmentPreparationService_.get();
        }
        [[nodiscard]] AssetThumbnailService* thumbnails() const noexcept {
            return assetThumbnailService_.get();
        }
        [[nodiscard]] AssetRuntimeService* runtime() const noexcept {
            return assetRuntimeService_.get();
        }
        // Stable address: AppFrameContext::mainModel refers to it.
        [[nodiscard]] const std::shared_ptr<ModelAsset>& mainModel() const noexcept {
            return mainModel_;
        }
        [[nodiscard]] const EnvironmentLightingHandles&
            environmentLighting() const noexcept {
            return environmentLighting_;
        }
        [[nodiscard]] const EnvironmentLightingSettings&
            sceneEnvironmentSettings() const noexcept {
            return sceneEnvironmentSettings_;
        }
        [[nodiscard]] const std::map<AssetGuid, LoadedEnvironmentAsset>&
            loadedEnvironments() const noexcept {
            return loadedEnvironments_;
        }
        [[nodiscard]] AssetGuid activeEnvironmentAssetGuid() const noexcept {
            return activeEnvironmentAssetGuid_;
        }
        [[nodiscard]] const std::string&
            activeEnvironmentCookKey() const noexcept {
            return activeEnvironmentCookKey_;
        }
        [[nodiscard]] AppEnvironmentIdentity environmentIdentity() const;

    private:
        [[nodiscard]] std::string persistBakedReflectionProbe(
            SceneEntityUuid owner,
            const ReflectionProbeCaptureCompletion::Product& product);
        void configureCookedModelHotReload();
        void configureCookedEnvironmentHotReload();

        ApplicationConfig& config_;
        CpuProfiler& cpuProfiler_;
        EngineLog& engineLog_;
        Tasks::TaskSystem& tasks_;
        SceneWorld& sceneWorld_;
        Registry& registry;
        EditorSceneDocumentService& sceneDocumentService_;
        IRenderBackend* renderBackend = nullptr;

        std::unique_ptr<AssetManager> assetManager_;
        std::unique_ptr<AssetCatalog> assetCatalog_;
        std::unique_ptr<AssetCatalogService> assetCatalogService_;
        std::shared_ptr<LocalDerivedDataCache> editorModelDdc_;
        std::unique_ptr<AssetModelPreparationService>
            assetModelPreparationService_;
        std::unique_ptr<AssetEnvironmentPreparationService>
            assetEnvironmentPreparationService_;
        std::unique_ptr<AssetThumbnailService> assetThumbnailService_;
        std::unique_ptr<AssetRuntimeService> assetRuntimeService_;
        AssetThumbnailUploadQueue pendingThumbnailUploads_;
        uint64_t thumbnailUploadsTotal_ = 0;
        uint64_t thumbnailUploadBytesTotal_ = 0;

        std::shared_ptr<ModelAsset> mainModel_;
        std::filesystem::path activeCookedModelArtifact_;
        std::filesystem::path activeCookedEnvironmentArtifact_;
        AssetGuid activeEnvironmentAssetGuid_;
        AssetGuid activeEnvironmentSourceGuid_;
        std::string activeEnvironmentCookKey_;
        std::string activeEnvironmentSourcePrimaries_;
        float activeEnvironmentRadianceScale_ = 0.0f;
        EnvironmentLightingHandles environmentLighting_;
        EnvironmentLightingSettings sceneEnvironmentSettings_;
        std::map<AssetGuid, LoadedEnvironmentAsset> loadedEnvironments_;
    };

} // namespace Iridium
