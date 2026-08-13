#include "editor/EditorSceneCommandService.h"

#include "editor/CoreEditorComponentRegistry.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorSceneHierarchy.h"
#include "editor/EditorSelectionState.h"
#include "editor/EditorTransactionService.h"
#include "scene/Components.h"
#include "scene/runtime/CoreComponentIds.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Iridium {
    namespace {

        struct StablePropertyReference {
            ComponentTypeId component;
            PropertyId property;
            std::optional<SceneEntityUuid> target;
            bool nullable = false;
        };

        struct ComponentSnapshot {
            ComponentTypeId id;
            EditorComponentSnapshot payload;
            std::vector<StablePropertyReference> references;
        };

        struct EntitySnapshot {
            SceneEntityUuid uuid;
            std::vector<ComponentSnapshot> components;
        };

        struct SelectionSnapshot {
            std::vector<SceneEntityUuid> entities;
            std::optional<SceneEntityUuid> primary;
        };

        struct SnapshotBundle {
            std::vector<EntitySnapshot> entities;
            std::vector<SourceSceneEntity> sourceEntities;
            SelectionSnapshot beforeSelection;
            SelectionSnapshot afterSelection;
            size_t estimatedBytes = 0;
        };

        struct ClipboardSnapshot {
            std::vector<EntitySnapshot> entities;
            std::vector<SourceSceneEntity> sourceEntities;
            SceneEntityUuid rootUuid;
        };

        [[nodiscard]] const EditorPropertyDescriptor* findProperty(
            const EditorComponentDescriptor& component, const PropertyId& id) {
            const auto found = std::ranges::find_if(component.properties,
                [&id](const EditorPropertyDescriptor& property) {
                    return property.id == id;
                });
            return found == component.properties.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool captureSelection(const SceneWorld& world,
            const EditorSelectionState& selection, SelectionSnapshot& result,
            std::string& diagnostic) {
            result = {};
            result.entities.reserve(selection.entities.size());
            for (Entity entity : selection.entities) {
                if (!world.registry().isAlive(entity)) continue;
                const auto uuid = world.identities().persistentId(entity);
                if (!uuid) {
                    diagnostic = "A selected entity has no persistent scene UUID";
                    return false;
                }
                result.entities.push_back(*uuid);
            }
            if (selection.primary != NULL_ENTITY &&
                world.registry().isAlive(selection.primary)) {
                result.primary = world.identities().persistentId(selection.primary);
                if (!result.primary) {
                    diagnostic = "The primary selection has no persistent scene UUID";
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool restoreSelection(const SceneWorld& world,
            EditorSelectionState& selection, const SelectionSnapshot& snapshot,
            std::string& diagnostic) {
            EditorSelectionState restored;
            restored.entities.reserve(snapshot.entities.size());
            for (SceneEntityUuid uuid : snapshot.entities) {
                const auto entity = world.identities().resolve(uuid);
                if (!entity) {
                    diagnostic = "A transaction selection target is unavailable";
                    return false;
                }
                restored.entities.push_back(*entity);
            }
            if (snapshot.primary) {
                const auto primary = world.identities().resolve(*snapshot.primary);
                if (!primary) {
                    diagnostic = "The transaction primary selection is unavailable";
                    return false;
                }
                restored.primary = *primary;
                if (!restored.contains(*primary)) {
                    restored.entities.push_back(*primary);
                }
            }
            selection = std::move(restored);
            return true;
        }

        [[nodiscard]] bool captureEntity(const SceneWorld& world, Entity entity,
            const EditorComponentRegistry& components, EntitySnapshot& result,
            std::string& diagnostic) {
            const Registry& registry = world.registry();
            const auto uuid = world.identities().persistentId(entity);
            if (!uuid) {
                diagnostic = "A structural snapshot target has no persistent UUID";
                return false;
            }
            result.uuid = *uuid;
            for (const EditorComponentDescriptor& descriptor :
                components.descriptors()) {
                if (!descriptor.has(registry, entity)) {
                    if (descriptor.required) {
                        diagnostic = "A structural snapshot target is missing required component " +
                            descriptor.id.value();
                        return false;
                    }
                    continue;
                }
                EditorComponentSnapshot payload = descriptor.capture(registry, entity);
                if (!payload) {
                    diagnostic = "Could not capture component " + descriptor.id.value();
                    return false;
                }
                ComponentSnapshot captured{
                    .id = descriptor.id,
                    .payload = std::move(payload),
                };
                for (const EditorPropertyDescriptor& property :
                    descriptor.properties) {
                    if (property.valueType != PropertyValueType::EntityReference) {
                        continue;
                    }
                    void* component = const_cast<void*>(captured.payload.get());
                    auto* reference = static_cast<Entity*>(
                        property.getMutable(component));
                    if (!reference) {
                        diagnostic = "Could not capture entity reference " +
                            descriptor.id.value() + "/" + property.id.value();
                        return false;
                    }
                    StablePropertyReference stable{
                        .component = descriptor.id,
                        .property = property.id,
                        .nullable = property.nullable,
                    };
                    if (*reference != NULL_ENTITY) {
                        stable.target = world.identities().persistentId(*reference);
                        if (!stable.target) {
                            diagnostic = "An entity reference targets an entity without a persistent UUID";
                            return false;
                        }
                    }
                    captured.references.push_back(std::move(stable));
                }
                result.components.push_back(std::move(captured));
            }
            return true;
        }

        [[nodiscard]] bool captureEntities(SceneWorld& world,
            std::span<const Entity> entities,
            const EditorComponentRegistry& components,
            std::vector<EntitySnapshot>& result, std::string& diagnostic) {
            result.clear();
            result.reserve(entities.size());
            for (Entity entity : entities) {
                EntitySnapshot snapshot;
                if (!captureEntity(world, entity, components, snapshot, diagnostic)) {
                    result.clear();
                    return false;
                }
                result.push_back(std::move(snapshot));
            }
            return true;
        }

        [[nodiscard]] bool restoreEntities(SceneWorld& world,
            const EditorComponentRegistry& components,
            const std::vector<EntitySnapshot>& snapshots,
            std::string& diagnostic) {
            Registry& registry = world.registry();
            std::vector<Entity> created;
            created.reserve(snapshots.size());
            const auto rollback = [&] {
                for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator) {
                    if (registry.isAlive(*iterator)) (void)world.destroyEntity(*iterator);
                }
                (void)rebuildEditorSceneHierarchy(registry);
            };
            try {
                for (const EntitySnapshot& snapshot : snapshots) {
                    if (world.identities().resolve(snapshot.uuid)) {
                        diagnostic = "A structural restore UUID is already live";
                        rollback();
                        return false;
                    }
                    created.push_back(world.createEntity(snapshot.uuid));
                }
                for (size_t index = 0; index < snapshots.size(); ++index) {
                    for (const ComponentSnapshot& component :
                        snapshots[index].components) {
                        const EditorComponentDescriptor* descriptor =
                            components.find(component.id);
                        if (!descriptor ||
                            !descriptor->restore(registry, created[index],
                                component.payload)) {
                            diagnostic = "Could not restore component " +
                                component.id.value();
                            rollback();
                            return false;
                        }
                    }
                }
                for (size_t index = 0; index < snapshots.size(); ++index) {
                    for (const ComponentSnapshot& component :
                        snapshots[index].components) {
                        if (component.references.empty()) continue;
                        const EditorComponentDescriptor* descriptor =
                            components.find(component.id);
                        void* restored = descriptor->getMutable(
                            registry, created[index]);
                        for (const StablePropertyReference& reference :
                            component.references) {
                            const EditorPropertyDescriptor* property =
                                findProperty(*descriptor, reference.property);
                            auto* destination = property
                                ? static_cast<Entity*>(property->getMutable(restored))
                                : nullptr;
                            if (!destination) {
                                diagnostic = "Could not restore entity reference " +
                                    component.id.value() + "/" +
                                    reference.property.value();
                                rollback();
                                return false;
                            }
                            if (!reference.target) {
                                *destination = NULL_ENTITY;
                                continue;
                            }
                            const auto target = world.identities().resolve(
                                *reference.target);
                            if (!target) {
                                if (reference.nullable) {
                                    *destination = NULL_ENTITY;
                                    continue;
                                }
                                diagnostic = "A required entity reference target is unavailable";
                                rollback();
                                return false;
                            }
                            *destination = *target;
                        }
                    }
                }
                const EditorHierarchyResult hierarchy =
                    rebuildEditorSceneHierarchy(registry);
                if (!hierarchy) {
                    diagnostic = hierarchy.diagnostic;
                    rollback();
                    return false;
                }
                return true;
            }
            catch (const std::exception& exception) {
                diagnostic = exception.what();
                rollback();
                return false;
            }
        }

        [[nodiscard]] bool eraseEntities(SceneWorld& world,
            const std::vector<EntitySnapshot>& snapshots,
            std::string& diagnostic) {
            std::vector<Entity> resolved;
            resolved.reserve(snapshots.size());
            for (const EntitySnapshot& snapshot : snapshots) {
                const auto entity = world.identities().resolve(snapshot.uuid);
                if (!entity) {
                    diagnostic = "A structural delete target is unavailable";
                    return false;
                }
                resolved.push_back(*entity);
            }
            try {
                for (auto iterator = resolved.rbegin(); iterator != resolved.rend();
                    ++iterator) {
                    if (!world.destroyEntity(*iterator)) {
                        diagnostic = "A structural delete target became unavailable";
                        return false;
                    }
                }
                const EditorHierarchyResult hierarchy =
                    rebuildEditorSceneHierarchy(world.registry());
                if (!hierarchy) {
                    diagnostic = hierarchy.diagnostic;
                    return false;
                }
                return true;
            }
            catch (const std::exception& exception) {
                diagnostic = exception.what();
                return false;
            }
        }

        struct StructuralOperationState {
            SceneWorld* world = nullptr;
            EditorSceneDocumentService* document = nullptr;
            EditorSelectionState* selection = nullptr;
            std::shared_ptr<const EditorComponentRegistry> components;
            SnapshotBundle snapshot;
            bool createDirection = false;

            [[nodiscard]] EditorMutationResult create() {
                std::string diagnostic;
                if (!restoreEntities(*world, *components,
                        snapshot.entities, diagnostic)) {
                    return EditorMutationResult::failure(std::move(diagnostic));
                }
                document->restorePreservedEntities(snapshot.sourceEntities);
                if (!restoreSelection(*world, *selection,
                        snapshot.afterSelection, diagnostic)) {
                    (void)eraseEntities(*world, snapshot.entities, diagnostic);
                    return EditorMutationResult::failure(std::move(diagnostic));
                }
                return EditorMutationResult::applied();
            }

            [[nodiscard]] EditorMutationResult erase() {
                std::string diagnostic;
                if (!eraseEntities(*world, snapshot.entities, diagnostic)) {
                    return EditorMutationResult::failure(std::move(diagnostic));
                }
                if (!restoreSelection(*world, *selection,
                        snapshot.afterSelection, diagnostic)) {
                    (void)restoreEntities(*world, *components,
                        snapshot.entities, diagnostic);
                    return EditorMutationResult::failure(std::move(diagnostic));
                }
                return EditorMutationResult::applied();
            }

            [[nodiscard]] EditorMutationResult apply() {
                return createDirection ? create() : erase();
            }

            [[nodiscard]] EditorMutationResult revert() {
                std::swap(snapshot.beforeSelection, snapshot.afterSelection);
                EditorMutationResult result = createDirection ? erase() : create();
                std::swap(snapshot.beforeSelection, snapshot.afterSelection);
                return result;
            }
        };

        [[nodiscard]] EditorTransactionOperation structuralOperation(
            std::shared_ptr<StructuralOperationState> state,
            std::string target) {
            return {
                .target = std::move(target),
                .apply = [state] { return state->apply(); },
                .revert = [state] { return state->revert(); },
                .estimatedPayloadBytes = state->snapshot.estimatedBytes,
            };
        }

        template<typename Component>
        void appendComponent(EntitySnapshot& entity, std::string_view id,
            Component value) {
            entity.components.push_back({
                .id = *ComponentTypeId::parse(id),
                .payload = std::make_shared<Component>(std::move(value)),
            });
        }

        [[nodiscard]] SelectionSnapshot exclusiveSelection(SceneEntityUuid uuid) {
            return { .entities = { uuid }, .primary = uuid };
        }

    } // namespace

    struct EditorSceneCommandService::Impl {
        SceneWorld& world;
        EditorSceneDocumentService& document;
        EditorTransactionService& transactions;
        EditorSelectionState& selection;
        std::shared_ptr<EditorComponentRegistry> components;
        std::optional<ClipboardSnapshot> clipboard;
        std::string diagnostic;

        Impl(EditorSceneDocumentService& document,
            EditorTransactionService& transactionService,
            EditorSelectionState& editorSelection)
            : world(document.world()), document(document),
              transactions(transactionService),
              selection(editorSelection) {
            CoreEditorComponentRegistryResult result =
                createCoreEditorComponentRegistry();
            if (!result.status) {
                diagnostic = result.status.message;
                return;
            }
            components = std::make_shared<EditorComponentRegistry>(
                std::move(result.registry));
        }

        [[nodiscard]] Entity create(std::string_view preferredName,
            glm::vec3 position, std::optional<AssetGuid> modelGuid) {
            diagnostic.clear();
            if (!components) {
                diagnostic = "Editor component registry is unavailable";
                return NULL_ENTITY;
            }
            const EditorHierarchyResult hierarchy =
                rebuildEditorSceneHierarchy(world.registry());
            if (!hierarchy) {
                diagnostic = hierarchy.diagnostic;
                return NULL_ENTITY;
            }
            SnapshotBundle bundle;
            if (!captureSelection(world, selection,
                    bundle.beforeSelection, diagnostic)) {
                return NULL_ENTITY;
            }
            EntitySnapshot entity;
            entity.uuid = world.allocateEntityUuid();
            appendComponent(entity, CoreNameComponentId,
                NameComponent{ uniqueEntityName(
                    world.registry(), preferredName) });
            TransformComponent transform;
            transform.position = position;
            transform.isDirty = true;
            appendComponent(entity, CoreTransformComponentId, transform);
            RelationshipComponent relationship;
            int nextOrder = 0;
            const auto* relationships =
                world.registry().findPool<RelationshipComponent>();
            if (relationships) {
                for (Entity root : hierarchy.roots) {
                    nextOrder = (std::max)(nextOrder,
                        relationships->get(root).siblingOrder + 1);
                }
            }
            relationship.siblingOrder = nextOrder;
            appendComponent(entity, CoreRelationshipComponentId, relationship);
            if (modelGuid) {
                MeshComponent mesh;
                mesh.assetGuid = *modelGuid;
                mesh.requestedAssetGuid = *modelGuid;
                appendComponent(entity, CoreMeshComponentId, std::move(mesh));
            }
            bundle.afterSelection = exclusiveSelection(entity.uuid);
            bundle.estimatedBytes = sizeof(EntitySnapshot) +
                entity.components.size() * sizeof(ComponentSnapshot);
            bundle.entities.push_back(std::move(entity));
            auto state = std::make_shared<StructuralOperationState>(
                StructuralOperationState{
                    .world = &world,
                    .document = &document,
                    .selection = &selection,
                    .components = components,
                    .snapshot = std::move(bundle),
                    .createDirection = true,
                });
            const SceneEntityUuid uuid = state->snapshot.entities.front().uuid;
            EditorTransaction transaction;
            transaction.label = modelGuid ? "Create Model" : "Create Entity";
            transaction.operations.push_back(structuralOperation(
                std::move(state), uuid.toString()));
            const EditorTransactionResult result =
                transactions.execute(std::move(transaction));
            if (!result) {
                diagnostic = result.diagnostic;
                return NULL_ENTITY;
            }
            return world.identities().resolve(uuid).value_or(NULL_ENTITY);
        }

        [[nodiscard]] bool erase(Entity root) {
            diagnostic.clear();
            if (!components || !world.registry().isAlive(root)) {
                diagnostic = components
                    ? "The entity to delete is unavailable"
                    : "Editor component registry is unavailable";
                return false;
            }
            std::vector<Entity> subtree;
            const EditorHierarchyResult hierarchy =
                collectEditorSceneSubtree(world.registry(), root, subtree);
            if (!hierarchy) {
                diagnostic = hierarchy.diagnostic;
                return false;
            }
            SnapshotBundle bundle;
            if (!captureSelection(world, selection,
                    bundle.beforeSelection, diagnostic) ||
                !captureEntities(world, subtree, *components,
                    bundle.entities, diagnostic)) {
                return false;
            }
            for (const SourceSceneEntity& source : document.document().entities) {
                if (std::ranges::find_if(bundle.entities,
                        [&source](const EntitySnapshot& runtime) {
                            return runtime.uuid == source.uuid;
                        }) != bundle.entities.end()) {
                    bundle.sourceEntities.push_back(source);
                }
            }
            bundle.afterSelection = bundle.beforeSelection;
            std::vector<SceneEntityUuid> deleted;
            deleted.reserve(bundle.entities.size());
            for (const EntitySnapshot& entity : bundle.entities) {
                deleted.push_back(entity.uuid);
            }
            const auto isDeleted = [&deleted](SceneEntityUuid uuid) {
                return std::ranges::find(deleted, uuid) != deleted.end();
            };
            std::erase_if(bundle.afterSelection.entities, isDeleted);
            if (bundle.afterSelection.primary &&
                isDeleted(*bundle.afterSelection.primary)) {
                bundle.afterSelection.primary =
                    bundle.afterSelection.entities.empty()
                    ? std::optional<SceneEntityUuid>{}
                    : std::optional(bundle.afterSelection.entities.back());
            }
            bundle.estimatedBytes = bundle.entities.size() *
                sizeof(EntitySnapshot);
            for (const EntitySnapshot& entity : bundle.entities) {
                bundle.estimatedBytes += entity.components.size() *
                    sizeof(ComponentSnapshot);
            }
            const auto uuid = world.identities().persistentId(root);
            auto state = std::make_shared<StructuralOperationState>(
                StructuralOperationState{
                    .world = &world,
                    .document = &document,
                    .selection = &selection,
                    .components = components,
                    .snapshot = std::move(bundle),
                    .createDirection = false,
                });
            EditorTransaction transaction;
            transaction.label = subtree.size() == 1
                ? "Delete Entity" : "Delete Subtree";
            transaction.operations.push_back(structuralOperation(
                std::move(state), uuid ? uuid->toString() : "entity"));
            const EditorTransactionResult result =
                transactions.execute(std::move(transaction));
            if (!result) diagnostic = result.diagnostic;
            return static_cast<bool>(result);
        }

        [[nodiscard]] bool reorder(Entity source, Entity target,
            bool insertAfter) {
            diagnostic.clear();
            Registry& registry = world.registry();
            if (!registry.isAlive(source) || !registry.isAlive(target) ||
                source == target) {
                diagnostic = "The hierarchy reorder targets are unavailable";
                return false;
            }
            const EditorHierarchyResult hierarchy =
                rebuildEditorSceneHierarchy(registry);
            if (!hierarchy) {
                diagnostic = hierarchy.diagnostic;
                return false;
            }
            auto* relationships = registry.findPool<RelationshipComponent>();
            const Entity parent = relationships->get(source).parent;
            if (relationships->get(target).parent != parent) {
                diagnostic = "Reordering across parents requires a reparent command";
                return false;
            }
            std::vector<Entity> siblings = parent == NULL_ENTITY
                ? hierarchy.roots
                : relationships->get(parent).children;
            const auto sourceFound = std::ranges::find(siblings, source);
            const auto targetFound = std::ranges::find(siblings, target);
            if (sourceFound == siblings.end() || targetFound == siblings.end()) {
                diagnostic = "The hierarchy reorder targets are not siblings";
                return false;
            }
            size_t sourceIndex = static_cast<size_t>(sourceFound - siblings.begin());
            size_t targetIndex = static_cast<size_t>(targetFound - siblings.begin());
            siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
            if (sourceIndex < targetIndex) --targetIndex;
            if (insertAfter) ++targetIndex;
            targetIndex = (std::min)(targetIndex, siblings.size());
            siblings.insert(siblings.begin() +
                static_cast<std::ptrdiff_t>(targetIndex), source);

            struct OrderValue {
                SceneEntityUuid uuid;
                int order = 0;
            };
            struct OrderState {
                SceneWorld* world = nullptr;
                std::vector<OrderValue> before;
                std::vector<OrderValue> after;

                [[nodiscard]] EditorMutationResult write(
                    const std::vector<OrderValue>& expected,
                    const std::vector<OrderValue>& replacement) {
                    auto* pool = world->registry().findPool<RelationshipComponent>();
                    std::vector<Entity> entities;
                    entities.reserve(expected.size());
                    for (size_t index = 0; index < expected.size(); ++index) {
                        const auto entity = world->identities().resolve(
                            expected[index].uuid);
                        if (!entity || !pool || !pool->has(*entity)) {
                            return EditorMutationResult::failure(
                                "A hierarchy sibling is unavailable");
                        }
                        if (pool->get(*entity).siblingOrder != expected[index].order) {
                            return EditorMutationResult::failure(
                                "Hierarchy order changed outside transaction history");
                        }
                        entities.push_back(*entity);
                    }
                    for (size_t index = 0; index < entities.size(); ++index) {
                        pool->get(entities[index]).siblingOrder =
                            replacement[index].order;
                    }
                    const EditorHierarchyResult rebuilt =
                        rebuildEditorSceneHierarchy(world->registry());
                    if (!rebuilt) {
                        for (size_t index = 0; index < entities.size(); ++index) {
                            pool->get(entities[index]).siblingOrder =
                                expected[index].order;
                        }
                        (void)rebuildEditorSceneHierarchy(world->registry());
                        return EditorMutationResult::failure(rebuilt.diagnostic);
                    }
                    return EditorMutationResult::applied();
                }
            };
            auto state = std::make_shared<OrderState>();
            state->world = &world;
            state->before.reserve(siblings.size());
            state->after.reserve(siblings.size());
            for (size_t index = 0; index < siblings.size(); ++index) {
                const auto uuid = world.identities().persistentId(siblings[index]);
                if (!uuid) {
                    diagnostic = "A hierarchy sibling has no persistent UUID";
                    return false;
                }
                state->before.push_back({
                    .uuid = *uuid,
                    .order = relationships->get(siblings[index]).siblingOrder,
                });
                state->after.push_back({
                    .uuid = *uuid,
                    .order = static_cast<int>(index),
                });
            }
            // before must be keyed in the same entity order as after.
            for (size_t index = 0; index < siblings.size(); ++index) {
                const SceneEntityUuid uuid = state->after[index].uuid;
                const auto original = std::ranges::find_if(state->before,
                    [uuid](const OrderValue& value) { return value.uuid == uuid; });
                state->before[index] = *original;
            }
            EditorTransaction transaction;
            transaction.label = "Reorder Entity";
            transaction.operations.push_back({
                .target = "hierarchy/sibling-order",
                .apply = [state] { return state->write(state->before, state->after); },
                .revert = [state] { return state->write(state->after, state->before); },
                .estimatedPayloadBytes = sizeof(OrderState) +
                    state->before.size() * sizeof(OrderValue) * 2,
            });
            const EditorTransactionResult result =
                transactions.execute(std::move(transaction));
            if (!result) diagnostic = result.diagnostic;
            return static_cast<bool>(result);
        }

        [[nodiscard]] bool reparent(Entity source, Entity newParent) {
            diagnostic.clear();
            Registry& registry = world.registry();
            if (!registry.isAlive(source) || source == newParent ||
                (newParent != NULL_ENTITY && !registry.isAlive(newParent))) {
                diagnostic = "The hierarchy reparent targets are unavailable";
                return false;
            }
            const EditorHierarchyResult hierarchy =
                rebuildEditorSceneHierarchy(registry);
            if (!hierarchy) {
                diagnostic = hierarchy.diagnostic;
                return false;
            }
            if (newParent != NULL_ENTITY &&
                editorSceneEntityIsDescendant(registry, newParent, source)) {
                diagnostic = "Reparenting would create a hierarchy cycle";
                return false;
            }
            auto* relationships = registry.findPool<RelationshipComponent>();
            const Entity oldParent = relationships->get(source).parent;
            if (oldParent == newParent) return true;

            auto siblingsOf = [&](Entity parent) {
                return parent == NULL_ENTITY
                    ? hierarchy.roots
                    : relationships->get(parent).children;
            };
            std::vector<Entity> oldSiblings = siblingsOf(oldParent);
            std::vector<Entity> newSiblings = siblingsOf(newParent);
            std::erase(oldSiblings, source);
            std::erase(newSiblings, source);
            newSiblings.push_back(source);

            struct RelationValue {
                SceneEntityUuid uuid;
                std::optional<SceneEntityUuid> parent;
                int order = 0;
            };
            const auto capture = [&](Entity entity) -> std::optional<RelationValue> {
                const auto uuid = world.identities().persistentId(entity);
                if (!uuid) return std::nullopt;
                const RelationshipComponent& relationship =
                    relationships->get(entity);
                RelationValue value{ .uuid = *uuid,
                    .order = relationship.siblingOrder };
                if (relationship.parent != NULL_ENTITY) {
                    value.parent = world.identities().persistentId(
                        relationship.parent);
                    if (!value.parent) return std::nullopt;
                }
                return value;
            };

            std::vector<Entity> affected = oldSiblings;
            affected.insert(affected.end(), newSiblings.begin(), newSiblings.end());
            std::ranges::sort(affected);
            const auto uniqueEnd = std::ranges::unique(affected).begin();
            affected.erase(uniqueEnd, affected.end());
            std::vector<RelationValue> before;
            std::vector<RelationValue> after;
            before.reserve(affected.size());
            after.reserve(affected.size());
            for (Entity entity : affected) {
                const auto value = capture(entity);
                if (!value) {
                    diagnostic = "A hierarchy entity has no persistent UUID";
                    return false;
                }
                before.push_back(*value);
                RelationValue replacement = *value;
                if (entity == source) {
                    replacement.parent = newParent == NULL_ENTITY
                        ? std::optional<SceneEntityUuid>{}
                        : world.identities().persistentId(newParent);
                }
                const auto oldFound = std::ranges::find(oldSiblings, entity);
                if (oldFound != oldSiblings.end()) {
                    replacement.order = static_cast<int>(
                        oldFound - oldSiblings.begin());
                }
                const auto newFound = std::ranges::find(newSiblings, entity);
                if (newFound != newSiblings.end()) {
                    replacement.order = static_cast<int>(
                        newFound - newSiblings.begin());
                }
                after.push_back(std::move(replacement));
            }

            struct RelationState {
                SceneWorld* world = nullptr;
                std::vector<RelationValue> before;
                std::vector<RelationValue> after;

                [[nodiscard]] EditorMutationResult write(
                    const std::vector<RelationValue>& expected,
                    const std::vector<RelationValue>& replacement) {
                    auto* pool = world->registry().findPool<RelationshipComponent>();
                    std::vector<Entity> entities;
                    std::vector<Entity> replacementParents;
                    entities.reserve(expected.size());
                    replacementParents.reserve(expected.size());
                    for (const RelationValue& value : expected) {
                        const auto entity = world->identities().resolve(value.uuid);
                        if (!entity || !pool || !pool->has(*entity)) {
                            return EditorMutationResult::failure(
                                "A hierarchy reparent target is unavailable");
                        }
                        const RelationshipComponent& current = pool->get(*entity);
                        std::optional<SceneEntityUuid> currentParent;
                        if (current.parent != NULL_ENTITY) {
                            currentParent = world->identities().persistentId(
                                current.parent);
                        }
                        if (currentParent != value.parent ||
                            current.siblingOrder != value.order) {
                            return EditorMutationResult::failure(
                                "Hierarchy relationships changed outside transaction history");
                        }
                        entities.push_back(*entity);
                        if (replacement[entities.size() - 1].parent) {
                            const auto parent = world->identities().resolve(
                                *replacement[entities.size() - 1].parent);
                            if (!parent) {
                                return EditorMutationResult::failure(
                                    "The new hierarchy parent is unavailable");
                            }
                            replacementParents.push_back(*parent);
                        }
                        else {
                            replacementParents.push_back(NULL_ENTITY);
                        }
                    }
                    for (size_t index = 0; index < entities.size(); ++index) {
                        RelationshipComponent& relationship = pool->get(entities[index]);
                        relationship.parent = replacementParents[index];
                        relationship.siblingOrder = replacement[index].order;
                    }
                    const EditorHierarchyResult rebuilt =
                        rebuildEditorSceneHierarchy(world->registry());
                    if (!rebuilt) {
                        for (size_t index = 0; index < entities.size(); ++index) {
                            RelationshipComponent& relationship = pool->get(entities[index]);
                            relationship.parent = expected[index].parent
                                ? *world->identities().resolve(*expected[index].parent)
                                : NULL_ENTITY;
                            relationship.siblingOrder = expected[index].order;
                        }
                        (void)rebuildEditorSceneHierarchy(world->registry());
                        return EditorMutationResult::failure(rebuilt.diagnostic);
                    }
                    return EditorMutationResult::applied();
                }
            };
            auto state = std::make_shared<RelationState>(RelationState{
                .world = &world,
                .before = std::move(before),
                .after = std::move(after),
            });
            EditorTransaction transaction;
            transaction.label = "Reparent Entity";
            transaction.operations.push_back({
                .target = "hierarchy/parent",
                .apply = [state] { return state->write(state->before, state->after); },
                .revert = [state] { return state->write(state->after, state->before); },
                .estimatedPayloadBytes = sizeof(RelationState) +
                    state->before.size() * sizeof(RelationValue) * 2,
            });
            const EditorTransactionResult result =
                transactions.execute(std::move(transaction));
            if (!result) diagnostic = result.diagnostic;
            return static_cast<bool>(result);
        }

        [[nodiscard]] bool captureClipboard(Entity root,
            ClipboardSnapshot& result) {
            std::vector<Entity> subtree;
            const EditorHierarchyResult hierarchy =
                collectEditorSceneSubtree(world.registry(), root, subtree);
            if (!hierarchy) {
                diagnostic = hierarchy.diagnostic;
                return false;
            }
            result = {};
            const auto rootUuid = world.identities().persistentId(root);
            if (!rootUuid || !captureEntities(world, subtree, *components,
                    result.entities, diagnostic)) {
                if (!rootUuid) diagnostic =
                    "The clipboard root has no persistent UUID";
                return false;
            }
            result.rootUuid = *rootUuid;
            for (const SourceSceneEntity& source : document.document().entities) {
                if (std::ranges::find_if(result.entities,
                        [&source](const EntitySnapshot& runtime) {
                            return runtime.uuid == source.uuid;
                        }) != result.entities.end()) {
                    result.sourceEntities.push_back(source);
                }
            }
            return true;
        }

        [[nodiscard]] Entity cloneClipboard(
            const ClipboardSnapshot& source, std::string label) {
            diagnostic.clear();
            SnapshotBundle bundle;
            if (!captureSelection(world, selection,
                    bundle.beforeSelection, diagnostic)) {
                return NULL_ENTITY;
            }
            bundle.entities = source.entities;
            std::unordered_map<SceneEntityUuid, SceneEntityUuid,
                SceneEntityUuidHash> remap;
            remap.reserve(bundle.entities.size());
            for (EntitySnapshot& entity : bundle.entities) {
                const SceneEntityUuid original = entity.uuid;
                entity.uuid = world.allocateEntityUuid();
                remap.emplace(original, entity.uuid);
            }
            for (EntitySnapshot& entity : bundle.entities) {
                for (ComponentSnapshot& component : entity.components) {
                    for (StablePropertyReference& reference : component.references) {
                        if (!reference.target) continue;
                        if (const auto found = remap.find(*reference.target);
                            found != remap.end()) {
                            reference.target = found->second;
                        }
                    }
                }
            }

            std::unordered_set<std::string> reservedNames;
            const auto* names = world.registry().findPool<NameComponent>();
            if (names) {
                reservedNames.reserve(names->components.size() +
                    bundle.entities.size());
                for (const NameComponent& name : names->components) {
                    reservedNames.insert(name.name);
                }
            }
            std::unordered_map<std::string, uint32_t> nextSuffix;
            nextSuffix.reserve(bundle.entities.size());
            for (EntitySnapshot& entity : bundle.entities) {
                for (ComponentSnapshot& component : entity.components) {
                    if (component.id.value() != CoreNameComponentId) continue;
                    const auto* original = static_cast<const NameComponent*>(
                        component.payload.get());
                    const std::string base = original->name + " Copy";
                    std::string candidate = base;
                    if (reservedNames.contains(candidate)) {
                        uint32_t& suffix = nextSuffix[base];
                        suffix = (std::max)(suffix, 2u);
                        do {
                            candidate = base + " (" +
                                std::to_string(suffix++) + ")";
                        } while (reservedNames.contains(candidate));
                    }
                    reservedNames.insert(candidate);
                    component.payload = std::make_shared<NameComponent>(
                        NameComponent{ std::move(candidate) });
                    break;
                }
            }

            bundle.sourceEntities = source.sourceEntities;
            bool opaquePayload = false;
            for (SourceSceneEntity& entity : bundle.sourceEntities) {
                const auto mapped = remap.find(entity.uuid);
                if (mapped == remap.end()) continue;
                entity.uuid = mapped->second;
                opaquePayload = opaquePayload ||
                    std::ranges::any_of(entity.components,
                        [](const SourceSceneComponent& component) {
                            return !component.known;
                        });
            }
            const SceneEntityUuid newRoot = remap.at(source.rootUuid);
            bundle.afterSelection = exclusiveSelection(newRoot);
            bundle.estimatedBytes = bundle.entities.size() * sizeof(EntitySnapshot);
            for (const EntitySnapshot& entity : bundle.entities) {
                bundle.estimatedBytes += entity.components.size() *
                    sizeof(ComponentSnapshot);
            }
            auto state = std::make_shared<StructuralOperationState>(
                StructuralOperationState{
                    .world = &world,
                    .document = &document,
                    .selection = &selection,
                    .components = components,
                    .snapshot = std::move(bundle),
                    .createDirection = true,
                });
            EditorTransaction transaction;
            transaction.label = std::move(label);
            transaction.operations.push_back(structuralOperation(
                std::move(state), newRoot.toString()));
            const EditorTransactionResult result =
                transactions.execute(std::move(transaction));
            if (!result) {
                diagnostic = result.diagnostic;
                return NULL_ENTITY;
            }
            if (opaquePayload) {
                diagnostic = "Duplicated opaque component payload; unknown entity references remain external because their schema is unavailable";
            }
            return world.identities().resolve(newRoot).value_or(NULL_ENTITY);
        }
    };

    EditorSceneCommandService::EditorSceneCommandService(
        EditorSceneDocumentService& document,
        EditorTransactionService& transactions,
        EditorSelectionState& selection)
        : impl_(std::make_shared<Impl>(document, transactions, selection)) {}

    bool EditorSceneCommandService::ready() const noexcept {
        return impl_ && static_cast<bool>(impl_->components);
    }

    const std::string& EditorSceneCommandService::diagnostic() const noexcept {
        static const std::string unavailable =
            "Editor scene command service is unavailable";
        return impl_ ? impl_->diagnostic : unavailable;
    }

    Entity EditorSceneCommandService::createEmpty(
        std::string_view preferredName, glm::vec3 position) {
        return impl_->create(preferredName, position, std::nullopt);
    }

    Entity EditorSceneCommandService::createModel(
        AssetGuid modelGuid, std::string_view preferredName,
        glm::vec3 position) {
        return impl_->create(preferredName, position, modelGuid);
    }

    bool EditorSceneCommandService::deleteEntity(Entity root) {
        return impl_->erase(root);
    }

    bool EditorSceneCommandService::reorder(
        Entity source, Entity target, bool insertAfter) {
        return impl_->reorder(source, target, insertAfter);
    }

    bool EditorSceneCommandService::reparent(
        Entity source, Entity newParent) {
        return impl_->reparent(source, newParent);
    }

    Entity EditorSceneCommandService::duplicateEntity(Entity root) {
        ClipboardSnapshot snapshot;
        impl_->diagnostic.clear();
        if (!impl_->captureClipboard(root, snapshot)) return NULL_ENTITY;
        return impl_->cloneClipboard(snapshot, "Duplicate Subtree");
    }

    bool EditorSceneCommandService::copyEntity(Entity root) {
        impl_->diagnostic.clear();
        ClipboardSnapshot snapshot;
        if (!impl_->captureClipboard(root, snapshot)) return false;
        impl_->clipboard = std::move(snapshot);
        return true;
    }

    Entity EditorSceneCommandService::paste() {
        if (!impl_->clipboard) {
            impl_->diagnostic = "The scene clipboard is empty";
            return NULL_ENTITY;
        }
        return impl_->cloneClipboard(*impl_->clipboard, "Paste Subtree");
    }

    bool EditorSceneCommandService::canPaste() const noexcept {
        return impl_ && impl_->clipboard.has_value();
    }

} // namespace Iridium
