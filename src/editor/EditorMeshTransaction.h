#pragma once

#include "editor/EditorTransactionService.h"
#include "scene/components/MeshComponent.h"

#include <vector>

class Registry;

namespace Iridium {

    struct EditorMeshAuthoringState {
        bool enabled = true;
        AssetGuid modelGuid;
        AssetGuid rawRequestedModelGuid;
        std::vector<MeshComponent::MaterialOverride> materialOverrides;
    };

    [[nodiscard]] EditorMeshAuthoringState captureEditorMeshAuthoringState(
        const MeshComponent& mesh);
    [[nodiscard]] bool sameEditorMeshAuthoringState(
        const EditorMeshAuthoringState& lhs,
        const EditorMeshAuthoringState& rhs) noexcept;
    void restoreRawEditorMeshAuthoringState(MeshComponent& mesh,
        const EditorMeshAuthoringState& state);
    [[nodiscard]] EditorTransactionOperation makeEditorMeshAuthoringOperation(
        Registry& registry, Entity entity,
        EditorMeshAuthoringState before,
        EditorMeshAuthoringState after);

} // namespace Iridium
