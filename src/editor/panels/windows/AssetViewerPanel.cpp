#include "editor/panels/windows/AssetViewerPanel.h"

#include "assets/AssetManager.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorAssetPartList.h"
#include "editor/EditorPreviewImageFit.h"
#include "editor/ViewportLayout.h"
#include "editor/ViewportRenderExtent.h"
#include "renderer/rhi/RenderDebugView.h"

#include <algorithm>
#include <cfloat>
#include <optional>
#include <set>
#include <limits>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>

namespace {

    const char* runtimeStateName(
        Iridium::RuntimeAssetState state) noexcept {
        using Iridium::RuntimeAssetState;
        switch (state) {
        case RuntimeAssetState::Missing: return "Waiting for runtime asset";
        case RuntimeAssetState::Queued: return "Preparing preview";
        case RuntimeAssetState::Ready: return "Runtime asset ready";
        case RuntimeAssetState::ReadyWithError: return "Showing last-known-good revision";
        case RuntimeAssetState::Failed: return "Preview preparation failed";
        case RuntimeAssetState::Evicted: return "Preview asset evicted";
        }
        return "Runtime state unavailable";
    }

}

void AssetViewerPanel::render(void* sceneTextureId, void* glassDepthTextureId,
    int& currentRenderMode, float sceneAspect,
    Iridium::AssetManager* assetManager) {
    hoveredPart = {};
    isHovered = false;
    isFocused = false;
    isVisible = false;
    if (documents_) pruneClosedCameras();
    if (!documents_ || documents_->documents().empty()) {
        if (demandThumbnails) demandThumbnails({});
        return;
    }

    for (const Iridium::EditorAssetDocument& document :
        documents_->documents()) {
        const auto snapshot = runtimeService_
            ? runtimeService_->snapshot(document.presentationAssetGuid)
            : std::nullopt;
        documents_->updateRuntimeState(
            document.presentationAssetGuid,
            snapshot ? std::optional(snapshot->state) : std::nullopt,
            snapshot ? snapshot->diagnostic : std::string_view{});
    }

    bool windowOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1050, 700), ImGuiCond_FirstUseEver);
    const ImGuiViewport* hostViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(600.0f, hostViewport->WorkSize.x),
        std::min(400.0f, hostViewport->WorkSize.y)), hostViewport->WorkSize);
    const bool visible = ImGui::Begin("Asset Viewer", &windowOpen, ImGuiWindowFlags_NoDocking);
    const ImVec2 hostSize = ImGui::GetWindowSize();
    const ImVec2 hostPosition = ImGui::GetWindowPos();
    ImGui::SetWindowPos(ImVec2(std::clamp(hostPosition.x, hostViewport->WorkPos.x,
        hostViewport->WorkPos.x + std::max(0.0f, hostViewport->WorkSize.x - hostSize.x)),
        std::clamp(hostPosition.y, hostViewport->WorkPos.y,
        hostViewport->WorkPos.y + std::max(0.0f, hostViewport->WorkSize.y - hostSize.y))));
    isVisible = visible;
    isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) {
        // Docked windows are intentionally not submitted while the host is
        // collapsed. Keep their node ownership alive until it is expanded.
        ImGuiWindowClass panelClass{};
        panelClass.ClassId = ImGui::GetID("asset-viewer-panel-class");
        panelClass.DockingAllowUnclassed = false;
        ImGui::DockSpace(ImGui::GetID("asset-viewer-dockspace-v1"), ImVec2(0, 0),
            ImGuiDockNodeFlags_KeepAliveOnly, &panelClass);
        if (demandThumbnails) demandThumbnails({});
        ImGui::End();
        ImGui::PopStyleVar();
        if (!windowOpen) documents_->closeAll();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    std::optional<Iridium::AssetGuid> closeDocument;
    if (ImGui::BeginChild("asset-viewer-tabs", ImVec2(0.0f, 38.0f))) {
        if (ImGui::BeginTabBar("asset-viewer-documents",
                ImGuiTabBarFlags_Reorderable |
                ImGuiTabBarFlags_AutoSelectNewTabs)) {
            for (const Iridium::EditorAssetDocument& document :
                documents_->documents()) {
                ImGui::PushID(document.assetGuid.toString().c_str());
                bool tabOpen = true;
                if (ImGui::BeginTabItem(document.displayName.c_str(), &tabOpen)) {
                    (void)documents_->activate(document.assetGuid);
                    ImGui::EndTabItem();
                }
                if (!tabOpen) closeDocument = document.assetGuid;
                ImGui::PopID();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    if (closeDocument) (void)documents_->close(*closeDocument);

    const Iridium::EditorAssetDocument* active = documents_->active();
    if (!active) {
        if (demandThumbnails) demandThumbnails({});
        ImGui::PopStyleVar();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    cameras_.try_emplace(active->assetGuid);

    if (ImGui::BeginChild("asset-viewer-toolbar", ImVec2(0.0f, 96.0f))) {
        ImGui::TextUnformatted(active->displayName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", active->assetType.c_str());
        ImGui::SameLine();
        if (active->runtimeState) {
            ImGui::TextDisabled("| %s", runtimeStateName(*active->runtimeState));
        }
        if (ImGui::Button("Frame")) {
            const PreviewBounds bounds = bounds_.contains(active->assetGuid)
                ? bounds_.at(active->assetGuid)
                : PreviewBounds{};
            frameActiveBounds(bounds.minimum, bounds.maximum, sceneAspect);
        }
        ImGui::SameLine();
        if (ImGui::Button("Show whole model")) {
            documents_->selectPreviewPart(active->selectedPart, active->selectedPartIsMaterial, false);
            if (auto model = assetManager ? assetManager->findCookedModel(active->presentationAssetGuid) : nullptr) {
                glm::vec3 minimum((std::numeric_limits<float>::max)());
                glm::vec3 maximum((std::numeric_limits<float>::lowest)());
                for (const auto& part : model->subMeshes) {
                    minimum = glm::min(minimum, part.boundsMin);
                    maximum = glm::max(maximum, part.boundsMax);
                }
                if (!model->subMeshes.empty()) frameActiveBounds(minimum, maximum, sceneAspect);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect")) documents_->selectPreviewPart(std::nullopt, false, false);
        ImGui::SameLine();
        if (ImGui::Button("Material options")) {
            parametersOpen_ = true;
            ImGui::SetWindowCollapsed("Material options###asset-viewer-material", false);
            ImGui::SetWindowFocus("Material options###asset-viewer-material");
        }
        ImGui::SameLine();
        if (ImGui::Button("Scene options")) {
            sceneOptionsOpen_ = true;
            ImGui::SetWindowCollapsed("Scene options###asset-viewer-scene", false);
            ImGui::SetWindowFocus("Scene options###asset-viewer-scene");
        }
        constexpr const char* modes[] = {
            "Standard", "Wireframe", "Glass Depth", "Base Color", "Normals",
            "Roughness", "Metallic", "Emissive", "Depth", "AO", "F0", "F90",
            "Material ID", "Material Flags", "Closure Class",
            "Cluster Occupancy", "Cluster Overflow", "Direct Lighting",
            "Shadow Cascade", "Shadow Visibility", "Transparency Class",
            "Transparency Fallback", "Transparency Interval",
            "Transparency Pyramid Mip", "Transparency Layers",
            "Transparency Overflow",
        };
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("##asset-view-mode", &currentRenderMode,
            modes, static_cast<int>(std::size(modes)));
        ImGui::SameLine();
        if (ImGui::Button("Asset list")) listOpen_ = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Background: 30 FPS");
        if (!active->runtimeDiagnostic.empty()) {
            ImGui::TextWrapped("%s", active->runtimeDiagnostic.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGuiWindowClass panelClass{};
    panelClass.ClassId = ImGui::GetID("asset-viewer-panel-class");
    panelClass.DockingAllowUnclassed = false;
    const ImGuiID dockspace = ImGui::GetID("asset-viewer-dockspace-v1");
    const ImVec2 dockSize = ImGui::GetContentRegionAvail();
    if (!ImGui::DockBuilderGetNode(dockspace)) {
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, dockSize);
        ImGuiID center = dockspace;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, .23f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, .34f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, .5f, nullptr, &right);
        ImGui::DockBuilderDockWindow("Asset list###asset-viewer-list", left);
        ImGui::DockBuilderDockWindow("Scene options###asset-viewer-scene", right);
        ImGui::DockBuilderDockWindow("Material options###asset-viewer-material", bottom);
        ImGui::DockBuilderDockWindow("Preview###asset-viewer-image", center);
        ImGui::DockBuilderFinish(dockspace);
    }
    ImGui::DockSpace(dockspace, dockSize, ImGuiDockNodeFlags_None, &panelClass);
    ImGui::End();
    ImGui::PopStyleVar();
    // A private docking class prevents these panels docking into the main editor.
    ImVec4 panelBackground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    panelBackground.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, panelBackground);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, panelBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));

    const bool focusSelection = listDocument_ != active->assetGuid ||
        !expandedGroups_.contains(active->assetGuid);
    listDocument_ = active->assetGuid;
    std::vector<Iridium::AssetCatalogRecord> thumbnailRecords;
    if (listOpen_) {
    ImGui::SetNextWindowClass(&panelClass);
    const bool listVisible = ImGui::Begin("Asset list###asset-viewer-list", &listOpen_);
    isFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (listVisible) {
        const auto model = assetManager ? assetManager->findCookedModel(active->presentationAssetGuid) : nullptr;
        if (model) {
            if (lookupParts && (partRecordsRoot_ != active->presentationAssetGuid ||
                ImGui::GetTime() - partRecordsRefreshTime_ >= 1.0)) {
                partRecords_.clear();
                for (auto& record : lookupParts(active->presentationAssetGuid))
                    partRecords_.insert_or_assign(record.guid, std::move(record));
                partRecordsRoot_ = active->presentationAssetGuid;
                partRecordsRefreshTime_ = ImGui::GetTime();
            }
            const auto rows = Iridium::makeEditorAssetPartRows(*model);
            if (!expandedGroups_.contains(active->assetGuid)) {
                std::array<bool, 3> groups{};
                if (active->kind == Iridium::EditorAssetViewerKind::Material) groups[0] = true;
                if (active->kind == Iridium::EditorAssetViewerKind::Primitive) {
                    for (const auto& part : model->subMeshes) if (part.sourcePrimitiveGuid == active->assetGuid) {
                        const bool transparent = part.transparency.resolvedClass != Iridium::TransparencyClass::None &&
                            part.transparency.resolvedClass != Iridium::TransparencyClass::AlphaClip;
                        groups[transparent ? 2 : 1] = true;
                    }
                }
                expandedGroups_.emplace(active->assetGuid, groups);
            }
            ImGui::PushID(active->assetGuid.toString().c_str());
            for (int category = 0; category < 3; ++category) {
                ImGui::SetNextItemOpen(expandedGroups_.at(active->assetGuid)[category], ImGuiCond_Always);
                const bool expanded = ImGui::CollapsingHeader(category == 0 ? "Materials" :
                    category == 1 ? "Model primitives" : "Transparent primitives");
                expandedGroups_.at(active->assetGuid)[category] = expanded;
                if (!expanded) continue;
                for (const auto& row : rows) {
                    if (row.material != (category == 0) || (category && row.transparent != (category == 2))) continue;
                    const auto guid = row.guid;
                    ImGui::PushID(guid.toString().c_str());
                    const bool selected = active->selectedPart == guid && active->selectedPartIsMaterial == (category == 0);
                    const auto record = partRecords_.find(guid);
                    if (record != partRecords_.end()) thumbnailRecords.push_back(record->second);
                    const std::string label = record != partRecords_.end() && !record->second.displayName.empty()
                        ? record->second.displayName : category == 0 ? "Material" :
                            "Mesh " + std::to_string(row.sourceMesh) + " / primitive " + std::to_string(row.sourcePrimitive);
                    bool thumbnailClicked = false, thumbnailHovered = false;
                    if (void* thumbnail = assetManager->getEditorThumbnail(guid)) {
                        ImGui::Image(reinterpret_cast<ImTextureID>(thumbnail), ImVec2(28, 28));
                        thumbnailHovered = ImGui::IsItemHovered();
                        thumbnailClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        ImGui::SameLine();
                    }
                    // Match the thumbnail hit height, including the space above/below text.
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4,
                        std::max(0.0f, (28.0f - ImGui::GetTextLineHeight()) * .5f)));
                    const bool detailsOpen = ImGui::TreeNodeEx("##part", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                        ImGuiTreeNodeFlags_SpanAvailWidth | (selected ? ImGuiTreeNodeFlags_Selected : 0), "%s", label.c_str());
                    ImGui::PopStyleVar();
                    const bool rowHovered = ImGui::IsItemHovered() || thumbnailHovered;
                    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) || thumbnailClicked) {
                        const bool isolate = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        documents_->selectPreviewPart(guid, category == 0, isolate);
                        if (isolate) {
                            frameActiveBounds(row.minimum, row.maximum, sceneAspect);
                        }
                    }
                    if (rowHovered && !active->isolateSelectedPart) {
                        hoveredPart = guid;
                        hoveredPartIsMaterial = category == 0;
                    }
                    if (focusSelection && selected) ImGui::SetScrollHereY(.5f);
                    if (detailsOpen) {
                        ImGui::TextWrapped("ID: %s", guid.toString().c_str());
                        ImGui::Text("Runtime pieces: %u", row.runtimePieces);
                        ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(row.triangles));
                        if (record != partRecords_.end()) ImGui::TextWrapped("Source: %s", record->second.sourceKey.c_str());
                        if (category) {
                            const glm::vec3 size = row.maximum - row.minimum;
                            ImGui::Text("Bounds: %.3g x %.3g x %.3g m", size.x, size.y, size.z);
                            for (const auto materialGuid : row.materials) {
                                const auto materialRecord = partRecords_.find(materialGuid);
                                const std::string materialName = materialRecord != partRecords_.end() &&
                                    !materialRecord->second.displayName.empty()
                                    ? materialRecord->second.displayName : materialGuid.toString();
                                ImGui::TextWrapped("Material: %s", materialName.c_str());
                                ImGui::TextWrapped("Material ID: %s", materialGuid.toString().c_str());
                            }
                            if (row.materials.empty()) ImGui::TextDisabled("Material: unavailable");
                        }
                        else ImGui::TextDisabled("Material graph: planned");
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        } else ImGui::TextDisabled("Waiting for model...");
    }
    ImGui::End();
    }
    ImGui::SetNextWindowClass(&panelClass);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    isVisible = ImGui::Begin("Preview###asset-viewer-image", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    isFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (isVisible) {
    const ImVec2 screenPosition = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    requestedRenderExtent = Iridium::viewportPixelExtent(
        available.x, available.y, framebufferScale.x, framebufferScale.y);
    const auto fitted = Iridium::previewImageFit(sceneAspect, available.y > 0 ? available.x / available.y : sceneAspect);
    const ImVec2 imageMinimum{
        screenPosition.x,
        screenPosition.y,
    };
    const ImVec2 imageSize{ std::max(0.0f, available.x), std::max(0.0f, available.y) };
    const ImVec2 imageMaximum{
        imageMinimum.x + imageSize.x,
        imageMinimum.y + imageSize.y,
    };
    isHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(imageMinimum, imageMaximum);

    void* texture = currentRenderMode == 2
        ? glassDepthTextureId : sceneTextureId;
    if (imageSize.x > 0.0f && imageSize.y > 0.0f) {
        ImGui::SetCursorScreenPos(imageMinimum);
        ImGui::Image(reinterpret_cast<ImTextureID>(texture), imageSize,
            ImVec2(fitted.uvMinimum.x, fitted.uvMinimum.y), ImVec2(fitted.uvMaximum.x, fitted.uvMaximum.y));
        ImGui::SetCursorScreenPos(imageMinimum);
        ImGui::InvisibleButton("##preview-camera", imageSize,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool cameraDrag = ImGui::IsItemActive();
        if (isHovered || cameraDrag) {
            ImGuiIO& io = ImGui::GetIO();
            Iridium::EditorOrbitCamera& camera = cameras_[active->assetGuid];
            if (cameraDrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                camera.orbit(io.MouseDelta.x, -io.MouseDelta.y);
            }
            if (cameraDrag && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                camera.pan(io.MouseDelta.x, io.MouseDelta.y, imageSize.y);
            }
            if (isHovered && io.MouseWheel != 0.0f) camera.dolly(io.MouseWheel);
        }
    }

    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (parametersOpen_) {
    ImGui::SetNextWindowClass(&panelClass);
    const bool parametersVisible = ImGui::Begin("Material options###asset-viewer-material", &parametersOpen_);
    isFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (parametersVisible) {
        ImGui::SeparatorText("Selected asset parameters");
        if (active->selectedPart && drawAssetParameters)
            drawAssetParameters(*active->selectedPart, assetManager);
        else ImGui::TextWrapped("Select a material or primitive from the list to edit its parameters.");
    }
    ImGui::End();
    }
    if (sceneOptionsOpen_) {
        ImGui::SetNextWindowClass(&panelClass);
        const bool sceneOptionsVisible = ImGui::Begin("Scene options###asset-viewer-scene", &sceneOptionsOpen_);
        isFocused |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (sceneOptionsVisible) {
                auto& lighting = lighting_[active->assetGuid];
                const auto snapshot = [&] { return nlohmann::json::array({lighting.environmentRotationDegrees,
                    lighting.lightingEv, lighting.backgroundEv, lighting.exposureEv, lighting.environmentAsset.toString(),
                    lighting.sunYawDegrees, lighting.sunPitchDegrees, lighting.sunEv,
                    lighting.sunColor.x, lighting.sunColor.y, lighting.sunColor.z, lighting.sunEnabled}); };
                auto [historyIt, newHistory] = lightingHistory_.try_emplace(active->assetGuid);
                auto& history = historyIt->second;
                if (newHistory) history.reset(snapshot());
                ImGui::TextWrapped("Preview only. The scene's saved lighting is unchanged.");
                std::string environmentName = lighting.environmentAsset.isNil() ? "Scene HDRI (inherited)" : lighting.environmentAsset.toString();
                for (const auto& record : environmentRecords_) if (record.guid == lighting.environmentAsset) environmentName = record.displayName;
                if (ImGui::Button(environmentName.c_str(), ImVec2(-1, 0))) {
                    environmentPickerOpen_ = !environmentPickerOpen_;
                    if (environmentPickerOpen_ && lookupEnvironments) environmentRecords_ = lookupEnvironments();
                }
                if (environmentPickerOpen_) {
                    if (ImGui::BeginChild("HDRI-subcar", ImVec2(0, 135), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
                        if (ImGui::Button("Use scene HDRI", ImVec2(110, 70))) lighting.environmentAsset = {};
                        for (const auto& record : environmentRecords_) {
                            ImGui::SameLine();
                            ImGui::PushID(record.guid.toString().c_str());
                            ImGui::BeginGroup();
                            bool clicked = false;
                            if (void* thumbnail = assetManager ? assetManager->getEditorThumbnail(record.guid) : nullptr)
                                clicked = ImGui::ImageButton("##environment", reinterpret_cast<ImTextureID>(thumbnail), ImVec2(100, 64));
                            else clicked = ImGui::Button("HDRI", ImVec2(108, 72));
                            if (ImGui::IsItemVisible()) thumbnailRecords.push_back(record);
                            if (clicked) lighting.environmentAsset = record.guid;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", record.displayName.c_str());
                            ImGui::Text("%.16s", record.displayName.c_str());
                            ImGui::EndGroup();
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                }
                if (!environmentDiagnostic.empty()) ImGui::TextWrapped("%s", environmentDiagnostic.c_str());
                ImGui::SliderFloat("HDRI rotation", &lighting.environmentRotationDegrees, -180, 180, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("HDRI lighting EV", &lighting.lightingEv, -10, 10, "%.2f EV", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Background EV", &lighting.backgroundEv, -10, 10, "%.2f EV", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Exposure EV", &lighting.exposureEv, -10, 10, "%.2f EV", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SeparatorText("Preview sun");
                ImGui::Checkbox("Sun enabled", &lighting.sunEnabled);
                ImGui::SliderFloat("Sun yaw", &lighting.sunYawDegrees, -180, 360, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Sun pitch", &lighting.sunPitchDegrees, -90, 90, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat("Sun EV (1000 lux base)", &lighting.sunEv, -10, 10, "%.2f EV", ImGuiSliderFlags_AlwaysClamp);
                ImGui::ColorEdit3("Sun color (linear)", &lighting.sunColor.x, ImGuiColorEditFlags_Float);
                if (ImGui::Button("Reset preview lighting")) lighting = {};
                history.observe(snapshot(), ImGui::GetActiveID(), ImGui::GetCurrentContext()->ActiveIdIsJustActivated);
                const auto restore = [&](const std::optional<nlohmann::json>& value) {
                    if (!value) return;
                    lighting.environmentRotationDegrees = value->at(0).get<float>();
                    lighting.lightingEv = value->at(1).get<float>();
                    lighting.backgroundEv = value->at(2).get<float>();
                    lighting.exposureEv = value->at(3).get<float>();
                    lighting.environmentAsset = Iridium::AssetGuid::parse(value->at(4).get<std::string>()).value_or(Iridium::AssetGuid{});
                    lighting.sunYawDegrees = value->at(5).get<float>();
                    lighting.sunPitchDegrees = value->at(6).get<float>();
                    lighting.sunEv = value->at(7).get<float>();
                    lighting.sunColor = {value->at(8).get<float>(), value->at(9).get<float>(), value->at(10).get<float>()};
                    lighting.sunEnabled = value->at(11).get<bool>();
                };
                ImGui::BeginDisabled(!history.canUndo());
                if (ImGui::Button("Undo lighting")) restore(history.undo());
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!history.canRedo());
                if (ImGui::Button("Redo lighting")) restore(history.redo());
                ImGui::EndDisabled();
        }
        ImGui::End();
    }
    if (demandThumbnails) demandThumbnails(thumbnailRecords);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (!windowOpen) {
        documents_->closeAll();
        if (demandThumbnails) demandThumbnails({});
    }
}

void AssetViewerPanel::frameActiveBounds(const glm::vec3& minimum,
    const glm::vec3& maximum, float aspect) {
    if (!documents_ || !documents_->active()) return;
    pruneClosedCameras();
    if (requestedRenderExtent.height) aspect = static_cast<float>(requestedRenderExtent.width) / requestedRenderExtent.height;
    const Iridium::AssetGuid activeGuid = documents_->active()->assetGuid;
    bounds_[activeGuid] = PreviewBounds{ minimum, maximum };
    cameras_[activeGuid].frameBounds(minimum, maximum, aspect);
}

const Iridium::EditorOrbitCamera* AssetViewerPanel::activeCamera() const noexcept {
    if (!documents_ || !documents_->active()) return nullptr;
    if (!sessions_.matches(*documents_->active())) return nullptr;
    const auto found = cameras_.find(documents_->active()->assetGuid);
    return found == cameras_.end() ? nullptr : &found->second;
}

Iridium::EditorOrbitCamera* AssetViewerPanel::activeCamera() noexcept {
    return const_cast<Iridium::EditorOrbitCamera*>(
        std::as_const(*this).activeCamera());
}

void AssetViewerPanel::pruneClosedCameras() {
    for (const auto guid : sessions_.synchronize(*documents_)) {
        lightingHistory_.erase(guid);
        lighting_.erase(guid);
        expandedGroups_.erase(guid);
        cameras_.erase(guid);
        bounds_.erase(guid);
        if (listDocument_ == guid) {
            listDocument_ = {};
        }
    }
}

Iridium::EditorPreviewLighting AssetViewerPanel::activeLighting() const noexcept {
    if (documents_ && documents_->active()) {
        if (!sessions_.matches(*documents_->active())) return {};
        const auto found = lighting_.find(documents_->active()->assetGuid);
        if (found != lighting_.end()) return found->second;
    }
    return {};
}
