// M7R R5a.2: code moved from Application.cpp (design section 3.4); see
// EditorHost.h. The scene projection handed to the editor build uses Vulkan
// [0, 1] clip depth, as in the Application translation unit it came from, so
// this file defines GLM_FORCE_DEPTH_ZERO_TO_ONE before any GLM include and is
// compiled without the GLM precompiled header.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "editor/EditorHost.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "imgui.h"
#include "assets/AssetCatalog.h"
#include "assets/AssetManager.h"
#include "assets/environment/AssetEnvironmentPreparationService.h"
#include "assets/model/AssetModelPreparationService.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorPreviewImageFit.h"
#include "editor/EditorSystem.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/EditorRenderBridge.h"
#include "renderer/rhi/Mesh.h"
#include "scene/components/LightComponent.h"
#include "scene/components/TransformComponent.h"

namespace Iridium {

    EditorHost::EditorHost(CpuProfiler& profiler)
        : cpuProfiler_(profiler),
          editor_(std::make_unique<::EditorSystem>()) {}

    EditorHost::~EditorHost() = default;

    void EditorHost::init(const EditorHostInit& init) {
        bridge_ = init.bridge;
        assets_ = init.assets;
        editor_->init(init.window, &cpuProfiler_, init.showProfiler,
            init.showMaterialDiagnostics,
            init.outputTransport, init.manualExposureEv,
            init.paperWhiteNits,
            init.peakNits, *init.shadowSettings,
            *init.reflectionProbeSettings,
            assets_.catalog,
            assets_.catalogService,
            assets_.modelPreparation,
            assets_.thumbnails,
            assets_.runtime,
            init.log, init.sceneDocuments, init.transactions, init.tasks);
    }

    void EditorHost::setOutputTransportStatus(Color::OutputTransport requested,
        Color::OutputTransport effective, const std::array<bool, 3>& supported,
        std::string diagnostic) {
        editor_->setOutputTransportStatus(requested, effective, supported,
            std::move(diagnostic));
    }

    void EditorHost::setAntiAliasingStatus(AntiAliasingMode active,
        std::string diagnostic) {
        editor_->setAntiAliasingStatus(active, std::move(diagnostic));
    }

    void EditorHost::setBloomStatus(const BloomSettings& active, std::string diagnostic) {
        editor_->setBloomStatus(active, std::move(diagnostic));
    }

    void EditorHost::setDebugView(RenderDebugView view) {
        editor_->setDebugView(view);
    }

    void EditorHost::openConfiguredAssetViewer(AssetGuid asset) {
        const std::vector<AssetCatalogRecord> records =
            assets_.catalog->recordsForGuid(
                asset);
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
            editor_->assetDocuments().open({
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

    void EditorHost::disableLayoutPersistence() {
        ImGui::GetIO().IniFilename = nullptr;
    }

    void EditorHost::setSelectedEntity(Entity entity) {
        editor_->setSelectedEntity(entity);
    }

    EditorAssetDocumentService& EditorHost::assetDocuments() noexcept {
        return editor_->assetDocuments();
    }

    std::shared_ptr<ModelAsset> EditorHost::resolveAssetPreview(
        RenderExtent renderExtent, uint64_t measuredFrameCount) {
        AssetManager* const assetManager = assets_.manager;
        const std::shared_ptr<ModelAsset>& mainModel = *assets_.mainModel;
        AssetRuntimeService* const assetRuntimeService = assets_.runtime;
        AssetModelPreparationService* const assetModelPreparationService =
            assets_.modelPreparation;
        AssetCatalog* const assetCatalog = assets_.catalog;
        const EditorAssetDocument* document =
            editor_->assetDocuments().active();
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
            if (assetRuntimeService) {
                assetRuntimeService->touch(
                    presentationGuid, measuredFrameCount + 1);
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
                const float aspect = renderExtent.height != 0
                    ? static_cast<float>(renderExtent.width) /
                        static_cast<float>(renderExtent.height)
                    : 1.0f;
                editor_->getAssetViewerPanel().frameActiveBounds(
                    minimum, maximum, aspect);
                framedPreviewDocumentGuid_ = document->assetGuid;
                framedPreviewCookKey_ = model->artifactCookKey;
                framedPreviewRevision_ = document->framingRevision;
                framedPreviewSession_ = document->sessionSerial;
            }
            return model;
        }

        if (!assetModelPreparationService || !assetCatalog) return {};
        if (assetModelPreparationService->pending(presentationGuid)) return {};
        if (assetRuntimeService) {
            const auto snapshot = assetRuntimeService->snapshot(presentationGuid);
            if (snapshot &&
                (snapshot->state == RuntimeAssetState::Queued ||
                 snapshot->state == RuntimeAssetState::Failed)) {
                return {};
            }
        }
        const std::vector<AssetCatalogRecord> records =
            assetCatalog->recordsForGuid(presentationGuid);
        const auto record = std::ranges::find_if(records,
            [](const AssetCatalogRecord& candidate) {
                return !candidate.parentGuid &&
                    candidate.assetType == "iridium.model" &&
                    candidate.assetRoot == "project" &&
                    candidate.status == AssetCatalogStatus::Ready;
            });
        if (record != records.end()) {
            try {
                (void)assetModelPreparationService->request(*record);
            }
            catch (const std::exception& exception) {
                if (assetRuntimeService) {
                    assetRuntimeService->reportFailure(
                        presentationGuid, exception.what());
                }
            }
        }
        return {};
    }

    EditorViewSelection EditorHost::chooseView(
        bool fullscreenScenePresentation) {
        ::EditorSystem& editor = *editor_;
        const bool dualViews = !fullscreenScenePresentation &&
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
        return { .dualViews = dualViews, .renderView = renderView };
    }

    SceneWorld* EditorHost::previewLightingWorld() {
        if (!editor_->renderingAssetView) return nullptr;
        auto& previewRegistry = previewLightingWorld_.registry();
        if (previewSun_ == NULL_ENTITY) {
            previewSun_ = previewLightingWorld_.createEntity();
            previewRegistry.addComponent<TransformComponent>(previewSun_);
            previewRegistry.addComponent<LightComponent>(previewSun_);
        }
        const auto settings = editor_->getAssetViewerPanel().activeLighting();
        auto& transform = previewRegistry.getComponent<TransformComponent>(previewSun_);
        transform.rotation = {settings.sunPitchDegrees, settings.sunYawDegrees, 0};
        auto& light = previewRegistry.getComponent<LightComponent>(previewSun_);
        light.colorLinearRec709 = settings.sunColor;
        light.illuminanceLux = settings.sunEnabled ? 1000.0f * std::exp2(settings.sunEv) : 0.0f;
        return &previewLightingWorld_;
    }

    Entity EditorHost::selectedEntity() const noexcept {
        return editor_->getSelectedEntity();
    }

    EnvironmentLightingHandles EditorHost::selectViewEnvironment(
        const EnvironmentLightingHandles& sceneEnvironment,
        uint64_t applicationFrameIndex) {
        auto desiredEnvironment = sceneEnvironment;
        const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments =
            *assets_.loadedEnvironments;
        AssetRuntimeService* const assetRuntimeService_ = assets_.runtime;
        AssetCatalog* const assetCatalog_ = assets_.catalog;
        AssetEnvironmentPreparationService* const
            assetEnvironmentPreparationService_ =
                assets_.environmentPreparation;
        auto& viewer = editor_->getAssetViewerPanel();
        viewer.environmentDiagnostic.clear();
        if (editor_->renderingAssetView) {
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
        return desiredEnvironment;
    }

    void EditorHost::prepareRetainedViews(bool dualViews, uint32_t renderView) {
        bridge_->prepareRetainedViews(dualViews, renderView);
        editor_->retainedSceneTexture = dualViews ? bridge_->retainedViewTextureId(0) : nullptr;
        editor_->retainedAssetTexture = dualViews ? bridge_->retainedViewTextureId(1) : nullptr;
    }

    EditorViewState EditorHost::viewState(float aspect,
        RenderExtent renderExtent, uint64_t measuredFrameCount) {
        ::EditorSystem& editor = *editor_;
        EditorViewState view{};
        view.renderingAssetView = editor.renderingAssetView;
        const EditorAssetDocument* previewDocument = editor.renderingAssetView
            ? editor.assetDocuments().active() : nullptr;
        view.assetPreviewActive = previewDocument != nullptr;
        AssetViewerPanel& viewer = editor.getAssetViewerPanel();
        if (view.assetPreviewActive) {
            view.previewModel = resolveAssetPreview(renderExtent,
                measuredFrameCount);
            view.previewSessionSerial = previewDocument->sessionSerial;
            view.previewFramingRevision = previewDocument->framingRevision;
            view.previewSelectedPart = previewDocument->selectedPart;
            view.previewSelectedPartIsMaterial =
                previewDocument->selectedPartIsMaterial;
            view.previewIsolateSelectedPart =
                previewDocument->isolateSelectedPart;
            view.previewHoveredPart = viewer.hoveredPart;
            view.previewHoveredPartIsMaterial = viewer.hoveredPartIsMaterial;
            const auto extent = viewer.requestedRenderExtent;
            view.previewProjectionScale = previewImageFit(aspect, extent.height ? static_cast<float>(extent.width) / extent.height : aspect).projectionScale;
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

    EditorFrameRequests EditorHost::build(const EditorBuildInputs& inputs) {
        ::EditorSystem& editor = *editor_;
        EditorFrameRequests requests{};
        // Build ImGui only after beginFrame selected currentImageIndex. The UI
        // descriptors are per swapchain image, so using them before acquisition
        // can sample a different target that has not yet been transitioned.
        CpuScope editorScope(cpuProfiler_, "cpu.editor.build");
        bridge_->beginUI();
        const float aspect = inputs.aspect;
        if (!inputs.fullscreenScenePresentation) {
            glm::mat4 sceneProjection = glm::perspective(glm::radians(inputs.verticalFovDegrees), aspect, inputs.nearPlane, inputs.farPlane);
            sceneProjection[1][1] *= -1.0f;
            editor.update(*inputs.registry, assets_.manager, glm::lookAt(inputs.cameraPosition, inputs.cameraPosition + inputs.cameraFront, inputs.cameraUp), sceneProjection,
                bridge_->sceneTextureId(),
                bridge_->glassDepthTextureId(),
                aspect);
            EditorOutputSettings outputSettings{};
            if (editor.consumeOutputSettings(outputSettings))
                requests.output = std::move(outputSettings);
            ProjectShadowSettings shadowSettings{};
            if (editor.consumeShadowSettings(shadowSettings))
                requests.shadows = shadowSettings;
            ProjectReflectionProbeSettings probeSettings{};
            if (editor.consumeReflectionProbeSettings(probeSettings))
                requests.probes = probeSettings;
            requests.requestedSceneExtent =
                editor.requestedRenderExtent();
            requests.view = viewState(aspect, inputs.renderExtent,
                inputs.measuredFrameCount);
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
                bridge_->sceneTextureId()), ImGui::GetContentRegionAvail());
            ImGui::End();
            ImGui::PopStyleVar();
            if (inputs.colorValidationOverlay) {
                editor.drawColorValidationOverlay();
            }
            // No asset preview without dual views: nothing is resolved.
            requests.view = viewState(aspect, inputs.renderExtent,
                inputs.measuredFrameCount);
        }
        return requests;
    }

    ViewportGridOverlay EditorHost::viewportGridOverlay(const glm::mat4& view,
        const glm::mat4& projection) const {
        return editor_->viewportGridOverlay(view, projection);
    }

    void EditorHost::viewRendered(uint32_t renderView) {
        editorViewScheduler_.rendered(renderView, EditorViewCadence::Clock::now());
    }

    bool EditorHost::assetViewerFocused() const noexcept {
        return editor_->getAssetViewerPanel().isFocused;
    }

    bool EditorHost::sceneViewportHovered() const noexcept {
        return editor_->getViewportPanel().isHovered;
    }

    RenderDebugView EditorHost::debugView() const {
        return editor_->getDebugView();
    }

    void EditorHost::cleanup() {
        editor_->cleanup();
    }

} // namespace Iridium
