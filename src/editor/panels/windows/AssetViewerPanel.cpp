#include "editor/panels/windows/AssetViewerPanel.h"

#include "assets/AssetManager.h"
#include "assets/runtime/AssetRuntimeService.h"
#include "editor/EditorAssetDocumentService.h"
#include "editor/ViewportLayout.h"
#include "editor/ViewportRenderExtent.h"
#include "renderer/rhi/RenderDebugView.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>

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
    (void)assetManager;
    if (!documents_ || documents_->documents().empty()) return;

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
    pruneClosedCameras();

    bool windowOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin("Asset Viewer", &windowOpen);
    isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) {
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
        ImGui::PopStyleVar();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    cameras_.try_emplace(active->assetGuid);

    if (ImGui::BeginChild("asset-viewer-toolbar", ImVec2(0.0f, 66.0f))) {
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
            cameras_[active->assetGuid].frameBounds(
                bounds.minimum, bounds.maximum, sceneAspect);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("LMB orbit  |  MMB pan  |  Wheel zoom");
        ImGui::SameLine();
        constexpr const char* modes[] = {
            "Standard", "Wireframe", "Glass Depth", "Base Color", "Normals",
            "Roughness", "Metallic", "Emissive", "Depth", "AO", "F0", "F90",
            "Material ID", "Material Flags", "Closure Class",
            "Cluster Occupancy", "Cluster Overflow", "Direct Lighting",
        };
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("##asset-view-mode", &currentRenderMode,
            modes, static_cast<int>(std::size(modes)));
        if (!active->runtimeDiagnostic.empty()) {
            ImGui::TextWrapped("%s", active->runtimeDiagnostic.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    const ImVec2 screenPosition = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    requestedRenderExtent = Iridium::viewportPixelExtent(
        available.x, available.y, framebufferScale.x, framebufferScale.y);
    const Iridium::ViewportFitRect fitted = Iridium::fitViewportAspect(
        available.x, available.y, sceneAspect);
    const ImVec2 imageMinimum{
        screenPosition.x + fitted.offsetX,
        screenPosition.y + fitted.offsetY,
    };
    const ImVec2 imageSize{ fitted.width, fitted.height };
    const ImVec2 imageMaximum{
        imageMinimum.x + imageSize.x,
        imageMinimum.y + imageSize.y,
    };
    isHovered = ImGui::IsMouseHoveringRect(imageMinimum, imageMaximum);

    void* texture = currentRenderMode == 2
        ? glassDepthTextureId : sceneTextureId;
    if (imageSize.x > 0.0f && imageSize.y > 0.0f) {
        ImGui::SetCursorScreenPos(imageMinimum);
        ImGui::Image(reinterpret_cast<ImTextureID>(texture), imageSize);
        if (isHovered) {
            ImGuiIO& io = ImGui::GetIO();
            Iridium::EditorOrbitCamera& camera = cameras_[active->assetGuid];
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                camera.orbit(io.MouseDelta.x, -io.MouseDelta.y);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                camera.pan(io.MouseDelta.x, io.MouseDelta.y, imageSize.y);
            }
            if (io.MouseWheel != 0.0f) camera.dolly(io.MouseWheel);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    if (!windowOpen) documents_->closeAll();
}

void AssetViewerPanel::frameActiveBounds(const glm::vec3& minimum,
    const glm::vec3& maximum, float aspect) {
    if (!documents_ || !documents_->active()) return;
    const Iridium::AssetGuid activeGuid = documents_->active()->assetGuid;
    bounds_[activeGuid] = PreviewBounds{ minimum, maximum };
    cameras_[activeGuid].frameBounds(minimum, maximum, aspect);
}

const Iridium::EditorOrbitCamera* AssetViewerPanel::activeCamera() const noexcept {
    if (!documents_ || !documents_->active()) return nullptr;
    const auto found = cameras_.find(documents_->active()->assetGuid);
    return found == cameras_.end() ? nullptr : &found->second;
}

Iridium::EditorOrbitCamera* AssetViewerPanel::activeCamera() noexcept {
    return const_cast<Iridium::EditorOrbitCamera*>(
        std::as_const(*this).activeCamera());
}

void AssetViewerPanel::pruneClosedCameras() {
    for (auto camera = cameras_.begin(); camera != cameras_.end();) {
        if (!documents_->find(camera->first)) camera = cameras_.erase(camera);
        else ++camera;
    }
    for (auto bounds = bounds_.begin(); bounds != bounds_.end();) {
        if (!documents_->find(bounds->first)) bounds = bounds_.erase(bounds);
        else ++bounds;
    }
}
