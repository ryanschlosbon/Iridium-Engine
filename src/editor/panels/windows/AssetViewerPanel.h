#pragma once

#include "core/types/AssetGuid.h"
#include "assets/AssetCatalog.h"
#include "editor/EditorOrbitCamera.h"
#include "editor/EditorPreviewLighting.h"
#include "editor/EditorMaterialDraftHistory.h"
#include "editor/EditorAssetSessionTracker.h"
#include "renderer/rhi/RhiResourceTypes.h"

#include <map>
#include <functional>
#include <array>

namespace Iridium {
    class AssetManager;
    class AssetRuntimeService;
    class EditorAssetDocumentService;
}

class AssetViewerPanel final {
public:
    AssetViewerPanel(
        Iridium::EditorAssetDocumentService* documents,
        Iridium::AssetRuntimeService* runtimeService)
        : documents_(documents), runtimeService_(runtimeService) {}

    void render(void* sceneTextureId, void* glassDepthTextureId,
        int& currentRenderMode, float sceneAspect,
        Iridium::AssetManager* assetManager);
    void frameActiveBounds(const glm::vec3& minimum,
        const glm::vec3& maximum, float aspect);

    [[nodiscard]] Iridium::EditorOrbitCamera* activeCamera() noexcept;
    [[nodiscard]] const Iridium::EditorOrbitCamera* activeCamera() const noexcept;
    [[nodiscard]] Iridium::EditorPreviewLighting activeLighting() const noexcept;

    bool isHovered = false;
    bool isFocused = false;
    bool isVisible = true;
    int debugRenderMode = 0;
    int backgroundFramesPerSecond = 30;
    Iridium::AssetGuid hoveredPart;
    bool hoveredPartIsMaterial = false;
    Iridium::RenderExtent requestedRenderExtent{};
    std::function<void(Iridium::AssetGuid, Iridium::AssetManager*)> drawAssetParameters;
    std::function<std::vector<Iridium::AssetCatalogRecord>(Iridium::AssetGuid)> lookupParts;
    std::function<std::vector<Iridium::AssetCatalogRecord>()> lookupEnvironments;
    std::string environmentDiagnostic;
    std::function<void(std::span<const Iridium::AssetCatalogRecord>)> demandThumbnails;

private:
    struct PreviewBounds {
        glm::vec3 minimum{ -1.0f };
        glm::vec3 maximum{ 1.0f };
    };

    void pruneClosedCameras();

    Iridium::EditorAssetDocumentService* documents_ = nullptr;
    Iridium::EditorAssetSessionTracker sessions_;
    Iridium::AssetRuntimeService* runtimeService_ = nullptr;
    std::map<Iridium::AssetGuid, Iridium::EditorOrbitCamera> cameras_;
    std::map<Iridium::AssetGuid, PreviewBounds> bounds_;
    Iridium::AssetGuid listDocument_;
    std::map<Iridium::AssetGuid, std::array<bool, 3>> expandedGroups_;
    bool parametersOpen_ = true;
    bool listOpen_ = true;
    std::map<Iridium::AssetGuid, Iridium::EditorPreviewLighting> lighting_;
    std::map<Iridium::AssetGuid, Iridium::EditorMaterialDraftHistory> lightingHistory_;
    bool sceneOptionsOpen_ = true;
    bool environmentPickerOpen_ = false;
    std::vector<Iridium::AssetCatalogRecord> environmentRecords_;
    std::map<Iridium::AssetGuid, Iridium::AssetCatalogRecord> partRecords_;
    Iridium::AssetGuid partRecordsRoot_;
    double partRecordsRefreshTime_ = -1;
};
