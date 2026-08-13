#pragma once

#include "scene/SceneWorld.h"
#include "scene/authoring/SourceSceneDocument.h"

#include <optional>
#include <vector>

namespace Iridium {

    struct SourceSceneCaptureResult {
        std::optional<SourceSceneDocument> document;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return document.has_value() && !hasSceneErrors(diagnostics);
        }
    };

    // Captures every live UUID-backed entity through the frozen codec registry.
    // Opaque components and unknown envelope/property data are merged from the
    // previously loaded source document by UUID and stable component ID.
    [[nodiscard]] SourceSceneCaptureResult captureSourceScene(
        const SceneWorld& world,
        const SourceSceneDocument& previousDocument,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry);

} // namespace Iridium
