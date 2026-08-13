#pragma once

#include "scene/SceneWorld.h"
#include "scene/authoring/SourceSceneDocument.h"

#include <memory>
#include <vector>

namespace Iridium {

    struct StagedSourceScene {
        std::unique_ptr<SceneWorld> world;
        SourceSceneDocument document;
    };

    struct SourceSceneStageResult {
        std::unique_ptr<StagedSourceScene> staging;
        std::vector<SceneDiagnostic> diagnostics;

        [[nodiscard]] explicit operator bool() const noexcept {
            return staging && !hasSceneErrors(diagnostics);
        }
    };

    [[nodiscard]] SourceSceneStageResult stageSourceScene(
        SourceSceneDocument document,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry);

    // Swaps only after staging has completed every required phase. The active
    // Registry address remains stable; displaced state moves into staging.
    void commitStagedSourceScene(
        SceneWorld& active,
        StagedSourceScene& staging);

} // namespace Iridium
