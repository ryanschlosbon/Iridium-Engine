#include "scene/authoring/SourceSceneLoadTransaction.h"

#include <array>
#include <exception>
#include <string>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] SceneDiagnostic loadError(
            std::string code,
            ScenePhase phase,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt,
            std::optional<ComponentTypeId> component = std::nullopt,
            std::string propertyPath = {}) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = phase,
                .entity = entity,
                .component = std::move(component),
                .propertyPath = std::move(propertyPath),
                .message = std::move(message),
            };
        }

        [[nodiscard]] std::optional<std::array<uint8_t, 16>> uuidBytes(
            std::string_view text) {
            if (text.size() != 36) return std::nullopt;
            const auto nibble = [](char value) -> int {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                return -1;
            };
            std::array<uint8_t, 16> bytes{};
            size_t output = 0;
            for (size_t index = 0; index < text.size();) {
                if (text[index] == '-') { ++index; continue; }
                if (index + 1 >= text.size() || output >= bytes.size()) {
                    return std::nullopt;
                }
                const int high = nibble(text[index]);
                const int low = nibble(text[index + 1]);
                if (high < 0 || low < 0) return std::nullopt;
                bytes[output++] = static_cast<uint8_t>((high << 4) | low);
                index += 2;
            }
            return output == bytes.size()
                ? std::optional<std::array<uint8_t, 16>>(bytes)
                : std::nullopt;
        }

        void addReference(
            SceneWorld& world,
            SceneEntityUuid owner,
            const ComponentTypeId& component,
            std::string path,
            StableReferenceKind kind,
            bool required,
            std::string_view target,
            std::vector<SceneDiagnostic>& diagnostics) {
            const auto bytes = uuidBytes(target);
            if (!bytes || !world.references().add({
                .key = { owner, component, std::move(path) },
                .kind = kind,
                .target = *bytes,
                .required = required,
            })) {
                diagnostics.push_back(loadError(
                    "scene.reference.duplicate_or_invalid",
                    ScenePhase::ReferenceResolution,
                    "Stable reference is invalid or duplicated", owner, component));
            }
        }

        void collectReferences(
            SceneWorld& world,
            const SourceSceneEntity& entity,
            const SourceSceneComponent& component,
            const RuntimeComponentDescriptor& descriptor,
            const ComponentSerializerRegistry& sourceRegistry,
            std::vector<SceneDiagnostic>& diagnostics) {
            for (const PropertyDescriptor& property : descriptor.properties) {
                const std::string name(sourceRegistry.sourceName(
                    component.id, property.id));
                if (!component.data.contains(name) ||
                    component.data.at(name).is_null()) continue;
                const SourceJson& value = component.data.at(name);
                if (property.referenceKind == PropertyReferenceKind::Entity) {
                    addReference(world, entity.uuid, component.id, name,
                        StableReferenceKind::Entity, property.required,
                        value.get<std::string>(), diagnostics);
                }
                else if (property.referenceKind == PropertyReferenceKind::Asset) {
                    addReference(world, entity.uuid, component.id, name,
                        StableReferenceKind::Asset,
                        property.required,
                        value.at("assetGuid").get<std::string>(), diagnostics);
                }
                else if (property.referenceKind == PropertyReferenceKind::Subasset) {
                    addReference(world, entity.uuid, component.id, name,
                        StableReferenceKind::Subasset,
                        property.required,
                        value.at("subassetGuid").get<std::string>(), diagnostics);
                }
                if (property.collectionOrdering ==
                    CollectionOrdering::SourceSubassetGuid) {
                    for (size_t index = 0; index < value.size(); ++index) {
                        const SourceJson& entry = value.at(index);
                        const std::string base = name + "/" + std::to_string(index);
                        addReference(world, entity.uuid, component.id,
                            base + "/source", StableReferenceKind::Subasset,
                            true,
                            entry.at("source").at("subassetGuid").get<std::string>(),
                            diagnostics);
                        addReference(world, entity.uuid, component.id,
                            base + "/replacement", StableReferenceKind::Subasset,
                            true,
                            entry.at("replacement").at("subassetGuid").get<std::string>(),
                            diagnostics);
                    }
                }
            }
        }

    } // namespace

    SourceSceneStageResult stageSourceScene(
        SourceSceneDocument document,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry) {
        SourceSceneStageResult result;
        if (!runtimeRegistry.isFrozen() || !sourceRegistry.isFrozen()) {
            result.diagnostics.push_back(loadError(
                "scene.load.registry_not_frozen", ScenePhase::Deserialize,
                "Scene component registries must be frozen before staging"));
            return result;
        }

        auto staging = std::make_unique<StagedSourceScene>();
        staging->world = std::make_unique<SceneWorld>();
        staging->document = std::move(document);

        try {
            for (const SourceSceneEntity& entity : staging->document.entities) {
                (void)staging->world->createEntity(entity.uuid);
            }
        }
        catch (const std::exception& exception) {
            result.diagnostics.push_back(loadError(
                "scene.load.identity_creation", ScenePhase::Identity,
                exception.what()));
        }
        if (hasSceneErrors(result.diagnostics)) return result;

        for (const SourceSceneEntity& entity : staging->document.entities) {
            const std::optional<Entity> handle =
                staging->world->identities().resolve(entity.uuid);
            if (!handle) {
                result.diagnostics.push_back(loadError(
                    "scene.load.identity_missing", ScenePhase::Identity,
                    "Staging identity map lost a validated entity", entity.uuid));
                continue;
            }
            for (const SourceSceneComponent& component : entity.components) {
                if (!component.known) continue;
                const RuntimeComponentDescriptor* runtime =
                    runtimeRegistry.find(component.id);
                const SourceComponentCodec* codec = sourceRegistry.find(component.id);
                if (!runtime || !codec) {
                    result.diagnostics.push_back(loadError(
                        "scene.load.codec_missing", ScenePhase::Deserialize,
                        "Known component is absent from the frozen registries",
                        entity.uuid, component.id));
                    continue;
                }
                collectReferences(*staging->world, entity, component, *runtime,
                    sourceRegistry, result.diagnostics);
                std::string message;
                try {
                    if (!codec->deserializeLocal(staging->world->registry(),
                        *handle, component.data, message)) {
                        result.diagnostics.push_back(loadError(
                            "scene.load.deserialize_failed", ScenePhase::Deserialize,
                            message.empty() ? "Component-local deserialization failed"
                                : std::move(message), entity.uuid, component.id));
                    }
                }
                catch (const std::exception& exception) {
                    result.diagnostics.push_back(loadError(
                        "scene.load.deserialize_exception", ScenePhase::Deserialize,
                        exception.what(), entity.uuid, component.id));
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (SceneReferenceRecord& reference : staging->world->references().records()) {
            if (reference.kind == StableReferenceKind::Entity) {
                const SceneEntityUuid target(reference.target);
                if (!staging->world->identities().resolve(target)) {
                    if (reference.required) {
                        result.diagnostics.push_back(loadError(
                            "scene.load.required_entity_reference_missing",
                            ScenePhase::ReferenceResolution,
                            "Required entity reference target is absent from the staged scene",
                            reference.key.owner, reference.key.component,
                            reference.key.propertyPath));
                    }
                    else {
                        result.diagnostics.push_back({
                            .severity = SceneDiagnosticSeverity::Warning,
                            .code = "scene.load.optional_entity_reference_unresolved",
                            .phase = ScenePhase::ReferenceResolution,
                            .entity = reference.key.owner,
                            .component = reference.key.component,
                            .propertyPath = reference.key.propertyPath,
                            .message = "Optional entity reference remains unresolved",
                        });
                    }
                }
                else reference.resolution = StableReferenceResolution::Resolved;
            }
            else {
                reference.resolution = StableReferenceResolution::Pending;
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (const SourceSceneEntity& entity : staging->document.entities) {
            const Entity handle = *staging->world->identities().resolve(entity.uuid);
            for (const SourceSceneComponent& component : entity.components) {
                if (!component.known) continue;
                const RuntimeComponentDescriptor* runtime =
                    runtimeRegistry.find(component.id);
                if (!runtime->resolveReferences(staging->world->registry(), handle,
                    staging->world->identities(), staging->world->references())) {
                    result.diagnostics.push_back(loadError(
                        "scene.load.reference_callback_failed",
                        ScenePhase::ReferenceResolution,
                        "Component reference resolution failed",
                        entity.uuid, component.id));
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (const SourceSceneEntity& entity : staging->document.entities) {
            const Entity handle = *staging->world->identities().resolve(entity.uuid);
            for (const SourceSceneComponent& component : entity.components) {
                if (!component.known) continue;
                const RuntimeComponentDescriptor* runtime =
                    runtimeRegistry.find(component.id);
                if (!runtime->postLoadValidate(
                    staging->world->registry(), handle)) {
                    result.diagnostics.push_back(loadError(
                        "scene.load.post_validation_failed", ScenePhase::PostLoad,
                        "Component post-load validation failed",
                        entity.uuid, component.id));
                }
            }
        }
        if (!staging->world->identities().validate(staging->world->registry())) {
            result.diagnostics.push_back(loadError(
                "scene.load.identity_post_validation", ScenePhase::PostLoad,
                "Staging identity map failed post-load validation"));
        }
        sortSceneDiagnostics(result.diagnostics);
        if (!hasSceneErrors(result.diagnostics)) result.staging = std::move(staging);
        return result;
    }

    void commitStagedSourceScene(
        SceneWorld& active,
        StagedSourceScene& staging) {
        if (!staging.world) {
            throw std::invalid_argument("Cannot commit an empty staged scene");
        }
        active.swapState(*staging.world);
    }

} // namespace Iridium
