#include "scene/authoring/SourceSceneCapture.h"

#include <algorithm>
#include <exception>
#include <string>
#include <unordered_map>

namespace Iridium {
    namespace {

        [[nodiscard]] SceneDiagnostic captureError(
            std::string code,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt,
            std::optional<ComponentTypeId> component = std::nullopt) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = ScenePhase::Save,
                .entity = entity,
                .component = std::move(component),
                .message = std::move(message),
            };
        }

        [[nodiscard]] const SourceSceneComponent* findComponent(
            const SourceSceneEntity* entity,
            const ComponentTypeId& id) {
            if (!entity) return nullptr;
            const auto found = std::ranges::find_if(entity->components,
                [&](const SourceSceneComponent& component) {
                    return component.id == id;
                });
            return found == entity->components.end() ? nullptr : &*found;
        }

    } // namespace

    SourceSceneCaptureResult captureSourceScene(
        const SceneWorld& world,
        const SourceSceneDocument& previousDocument,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry) {
        SourceSceneCaptureResult result;
        if (!runtimeRegistry.isFrozen() || !sourceRegistry.isFrozen()) {
            result.diagnostics.push_back(captureError(
                "scene.capture.registry_not_frozen",
                "Scene component registries must be frozen before capture"));
            return result;
        }
        const SceneIdentityResult identityStatus =
            world.identities().validate(world.registry());
        if (!identityStatus) {
            result.diagnostics.push_back(captureError(
                "scene.capture.identity_invalid", identityStatus.message));
            return result;
        }

        std::unordered_map<SceneEntityUuid, const SourceSceneEntity*,
            SceneEntityUuidHash> previousEntities;
        previousEntities.reserve(previousDocument.entities.size());
        for (const SourceSceneEntity& entity : previousDocument.entities) {
            previousEntities.emplace(entity.uuid, &entity);
        }

        SourceSceneDocument captured;
        captured.name = previousDocument.name.empty()
            ? "Untitled"
            : previousDocument.name;
        captured.extensions = previousDocument.extensions;
        captured.unknownFields = previousDocument.unknownFields;
        captured.entities.reserve(world.identities().size());

        for (const Entity entity : world.registry().aliveEntities()) {
            const std::optional<SceneEntityUuid> uuid =
                world.identities().persistentId(entity);
            if (!uuid) {
                result.diagnostics.push_back(captureError(
                    "scene.capture.identity_missing",
                    "Live scene entity has no persistent UUID"));
                continue;
            }
            const auto previousFound = previousEntities.find(*uuid);
            const SourceSceneEntity* previous = previousFound == previousEntities.end()
                ? nullptr
                : previousFound->second;

            SourceSceneEntity capturedEntity;
            capturedEntity.uuid = *uuid;
            if (previous) {
                capturedEntity.extensions = previous->extensions;
                capturedEntity.unknownFields = previous->unknownFields;
            }

            for (const SourceComponentCodec& codec : sourceRegistry.codecs()) {
                const SourceSceneComponent* old = findComponent(
                    previous, codec.componentId);
                SourceJson data = old ? old->data : SourceJson(nullptr);
                std::string message;
                try {
                    if (!codec.serializeSource(world.registry(), entity,
                            world.identities(), data, message)) {
                        result.diagnostics.push_back(captureError(
                            "scene.capture.component_failed",
                            message.empty()
                                ? "Component source serialization failed"
                                : std::move(message),
                            *uuid, codec.componentId));
                        continue;
                    }
                }
                catch (const std::exception& exception) {
                    result.diagnostics.push_back(captureError(
                        "scene.capture.component_exception", exception.what(),
                        *uuid, codec.componentId));
                    continue;
                }
                // Null is the explicit generic signal that this entity does not
                // currently own the component.
                if (data.is_null()) continue;

                SourceSceneComponent component;
                component.id = codec.componentId;
                component.version = codec.currentSourceVersion;
                component.data = std::move(data);
                component.known = true;
                if (old) component.unknownEnvelopeFields =
                    old->unknownEnvelopeFields;
                capturedEntity.components.push_back(std::move(component));
            }

            if (previous) {
                for (const SourceSceneComponent& component : previous->components) {
                    if (!component.known) {
                        capturedEntity.components.push_back(component);
                    }
                }
            }
            captured.entities.push_back(std::move(capturedEntity));
        }

        std::ranges::sort(captured.entities,
            [](const SourceSceneEntity& lhs, const SourceSceneEntity& rhs) {
                return lhs.uuid < rhs.uuid;
            });
        sortSceneDiagnostics(result.diagnostics);
        if (!hasSceneErrors(result.diagnostics)) {
            result.document = std::move(captured);
        }
        return result;
    }

} // namespace Iridium
