#include "editor/EditorMeshTransaction.h"

#include "ecs/Registry.h"
#include "renderer/rhi/Mesh.h"

#include <memory>
#include <utility>

namespace Iridium {

    EditorMeshAuthoringState captureEditorMeshAuthoringState(
        const MeshComponent& mesh) {
        const AssetGuid modelGuid = !mesh.requestedAssetGuid.isNil()
            ? mesh.requestedAssetGuid
            : !mesh.assetGuid.isNil()
                ? mesh.assetGuid
                : mesh.model ? mesh.model->assetGuid : AssetGuid{};
        return {
            .enabled = mesh.enabled,
            .modelGuid = modelGuid,
            .rawRequestedModelGuid = mesh.requestedAssetGuid,
            .materialOverrides = mesh.materialOverrides,
        };
    }

    bool sameEditorMeshAuthoringState(
        const EditorMeshAuthoringState& lhs,
        const EditorMeshAuthoringState& rhs) noexcept {
        return lhs.enabled == rhs.enabled && lhs.modelGuid == rhs.modelGuid &&
            lhs.materialOverrides == rhs.materialOverrides;
    }

    void restoreRawEditorMeshAuthoringState(MeshComponent& mesh,
        const EditorMeshAuthoringState& state) {
        mesh.enabled = state.enabled;
        mesh.requestedAssetGuid = state.rawRequestedModelGuid;
        mesh.materialOverrides = state.materialOverrides;
        mesh.requestedMaterialAssetRoots.clear();
    }

    EditorTransactionOperation makeEditorMeshAuthoringOperation(
        Registry& registry, Entity entity,
        EditorMeshAuthoringState before,
        EditorMeshAuthoringState after) {
        struct State {
            Registry* registry = nullptr;
            Entity entity = NULL_ENTITY;
            EditorMeshAuthoringState before;
            EditorMeshAuthoringState after;

            [[nodiscard]] EditorMutationResult write(
                const EditorMeshAuthoringState& expected,
                const EditorMeshAuthoringState& replacement) {
                auto* pool = registry->findPool<MeshComponent>();
                if (!pool || !pool->has(entity)) {
                    return EditorMutationResult::failure(
                        "Mesh component is no longer available");
                }
                MeshComponent& current = pool->get(entity);
                const EditorMeshAuthoringState currentState =
                    captureEditorMeshAuthoringState(current);
                if (sameEditorMeshAuthoringState(currentState, replacement)) {
                    return EditorMutationResult::noChange();
                }
                if (!sameEditorMeshAuthoringState(currentState, expected)) {
                    return EditorMutationResult::failure(
                        "Mesh authoring state changed outside transaction history");
                }
                current.enabled = replacement.enabled;
                current.requestedAssetGuid = replacement.modelGuid;
                if (replacement.modelGuid.isNil()) {
                    current.model.reset();
                    current.assetGuid = {};
                    current.requestedAssetSourcePath.clear();
                    current.assetResolutionDiagnostic.clear();
                }
                current.materialOverrides = replacement.materialOverrides;
                current.requestedMaterialAssetRoots.clear();
                return EditorMutationResult::applied();
            }
        };
        auto state = std::make_shared<State>(State{
            .registry = &registry,
            .entity = entity,
            .before = std::move(before),
            .after = std::move(after),
        });
        return {
            .target = "entity/mesh/authoring",
            .apply = [state] { return state->write(state->before, state->after); },
            .revert = [state] { return state->write(state->after, state->before); },
            .estimatedPayloadBytes = sizeof(State) +
                (state->before.materialOverrides.size() +
                 state->after.materialOverrides.size()) *
                    sizeof(MeshComponent::MaterialOverride),
        };
    }

} // namespace Iridium
