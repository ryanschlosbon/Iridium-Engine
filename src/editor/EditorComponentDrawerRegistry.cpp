#include "editor/EditorComponentDrawerRegistry.h"

#include "scene/runtime/CoreComponentIds.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] EditorComponentDrawerRegistryResult failure(
            EditorComponentDrawerRegistryError error,
            std::string message) {
            return { error, std::move(message) };
        }

    } // namespace

    EditorComponentDrawerRegistryResult EditorComponentDrawerRegistry::add(
        EditorComponentDrawerDescriptor descriptor) {
        if (frozen_) {
            return failure(EditorComponentDrawerRegistryError::Frozen,
                "Editor component drawer registry is frozen");
        }
        if (descriptor.componentId.empty()) {
            return failure(EditorComponentDrawerRegistryError::InvalidComponentId,
                "Editor drawer component stable ID is invalid");
        }
        if (!descriptor.draw) {
            return failure(EditorComponentDrawerRegistryError::MissingDrawer,
                "Editor component drawer callback is missing");
        }
        if (find(descriptor.componentId)) {
            return failure(EditorComponentDrawerRegistryError::DuplicateComponentId,
                "Duplicate editor drawer component stable ID: " +
                    descriptor.componentId.value());
        }
        descriptors_.push_back(std::move(descriptor));
        return {};
    }

    EditorComponentDrawerRegistryResult
    EditorComponentDrawerRegistry::freezeAndValidate(
        const EditorComponentRegistry& components) {
        if (frozen_) {
            return failure(EditorComponentDrawerRegistryError::Frozen,
                "Editor component drawer registry is already frozen");
        }
        if (!components.isFrozen()) {
            return failure(EditorComponentDrawerRegistryError::UnknownComponentId,
                "Editor component registry must be frozen before drawers");
        }
        for (const auto& drawer : descriptors_) {
            const auto* component = components.find(drawer.componentId);
            if (!component) {
                return failure(EditorComponentDrawerRegistryError::UnknownComponentId,
                    "Editor drawer references an unknown component stable ID: " +
                        drawer.componentId.value());
            }
            if (!component->visible) {
                return failure(EditorComponentDrawerRegistryError::HiddenComponent,
                    "A hidden editor component cannot register a drawer");
            }
        }
        std::ranges::sort(descriptors_, [](const auto& left, const auto& right) {
            return left.componentId < right.componentId;
        });
        frozen_ = true;
        return {};
    }

    const EditorComponentDrawerDescriptor*
    EditorComponentDrawerRegistry::find(
        const ComponentTypeId& id) const noexcept {
        const auto found = std::ranges::find_if(descriptors_,
            [&](const auto& descriptor) {
                return descriptor.componentId == id;
            });
        return found == descriptors_.end() ? nullptr : &*found;
    }

    std::span<const EditorComponentDrawerDescriptor>
    EditorComponentDrawerRegistry::descriptors() const noexcept {
        return frozen_
            ? std::span<const EditorComponentDrawerDescriptor>(descriptors_)
            : std::span<const EditorComponentDrawerDescriptor>{};
    }

    CoreEditorComponentDrawerRegistryResult
    createCoreEditorComponentDrawerRegistry(
        const EditorComponentRegistry& components,
        CoreEditorComponentDrawerCallbacks callbacks) {
        CoreEditorComponentDrawerRegistryResult result;
        const auto add = [&](std::string_view stableId,
                             DrawEditorComponentFn draw) {
            const auto id = ComponentTypeId::parse(stableId);
            if (!id) {
                return failure(
                    EditorComponentDrawerRegistryError::InvalidComponentId,
                    "Core editor drawer stable ID is invalid");
            }
            return result.registry.add({ *id, std::move(draw) });
        };

        result.status = add(CoreTransformComponentId,
            std::move(callbacks.transform));
        if (!result.status) return result;
        result.status = add(CoreRelationshipComponentId,
            std::move(callbacks.relationship));
        if (!result.status) return result;
        result.status = add(CoreMeshComponentId,
            std::move(callbacks.mesh));
        if (!result.status) return result;
        result.status = add(CoreLightComponentId,
            std::move(callbacks.light));
        if (!result.status) return result;
        result.status = add(CoreSkyComponentId,
            std::move(callbacks.sky));
        if (!result.status) return result;
        result.status = add(CoreReflectionProbeComponentId,
            std::move(callbacks.reflectionProbe));
        if (!result.status) return result;
        result.status = add(CoreBakedLightingSetComponentId,
            std::move(callbacks.bakedLightingSet));
        if (!result.status) return result;
        result.status = result.registry.freezeAndValidate(components);
        return result;
    }

} // namespace Iridium
