#pragma once

#include "assets/AssetGuid.h"
#include "editor/EditorOrbitCamera.h"
#include "renderer/rhi/RhiResourceTypes.h"

#include <map>

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

    bool isHovered = false;
    bool isFocused = false;
    Iridium::RenderExtent requestedRenderExtent{};

private:
    struct PreviewBounds {
        glm::vec3 minimum{ -1.0f };
        glm::vec3 maximum{ 1.0f };
    };

    void pruneClosedCameras();

    Iridium::EditorAssetDocumentService* documents_ = nullptr;
    Iridium::AssetRuntimeService* runtimeService_ = nullptr;
    std::map<Iridium::AssetGuid, Iridium::EditorOrbitCamera> cameras_;
    std::map<Iridium::AssetGuid, PreviewBounds> bounds_;
};
