#pragma once

// M7R R5a (design section 3.3): the editor state render extraction reads, as
// plain values. EditorHost produces it once before the editor is built (the
// previous frame's state) and once after; extraction never sees an editor type.

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>

#include "core/types/AssetGuid.h"
#include "ecs/Entity.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/rhi/RhiResourceTypes.h"

namespace Iridium {

    struct ModelAsset;

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
        // Projection scale that fits the preview into the asset viewer's
        // requested extent at the frame aspect (previewImageFit).
        float previewProjectionScale = 1.0f;
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

} // namespace Iridium
