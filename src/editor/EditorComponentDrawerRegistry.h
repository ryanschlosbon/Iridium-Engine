#pragma once

#include "editor/EditorComponentRegistry.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    class AssetManager;

    struct EditorComponentDrawContext {
        void* component = nullptr;
        Registry& registry;
        Entity entity = NULL_ENTITY;
        AssetManager* assetManager = nullptr;
    };

    using DrawEditorComponentFn =
        std::function<void(EditorComponentDrawContext&)>;

    struct EditorComponentDrawerDescriptor {
        ComponentTypeId componentId;
        DrawEditorComponentFn draw;
    };

    enum class EditorComponentDrawerRegistryError {
        None,
        Frozen,
        InvalidComponentId,
        DuplicateComponentId,
        UnknownComponentId,
        HiddenComponent,
        MissingDrawer,
    };

    struct EditorComponentDrawerRegistryResult {
        EditorComponentDrawerRegistryError error =
            EditorComponentDrawerRegistryError::None;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == EditorComponentDrawerRegistryError::None;
        }
    };

    class EditorComponentDrawerRegistry {
    public:
        [[nodiscard]] EditorComponentDrawerRegistryResult add(
            EditorComponentDrawerDescriptor descriptor);
        [[nodiscard]] EditorComponentDrawerRegistryResult freezeAndValidate(
            const EditorComponentRegistry& components);

        [[nodiscard]] const EditorComponentDrawerDescriptor* find(
            const ComponentTypeId& id) const noexcept;
        [[nodiscard]] std::span<const EditorComponentDrawerDescriptor>
            descriptors() const noexcept;
        [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    private:
        std::vector<EditorComponentDrawerDescriptor> descriptors_;
        bool frozen_ = false;
    };

    struct CoreEditorComponentDrawerCallbacks {
        DrawEditorComponentFn transform;
        DrawEditorComponentFn relationship;
        DrawEditorComponentFn mesh;
        DrawEditorComponentFn light;
        DrawEditorComponentFn sky;
        DrawEditorComponentFn reflectionProbe;
        DrawEditorComponentFn bakedLightingSet;
    };

    struct CoreEditorComponentDrawerRegistryResult {
        EditorComponentDrawerRegistry registry;
        EditorComponentDrawerRegistryResult status;

        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(status) && registry.isFrozen();
        }
    };

    [[nodiscard]] CoreEditorComponentDrawerRegistryResult
        createCoreEditorComponentDrawerRegistry(
            const EditorComponentRegistry& components,
            CoreEditorComponentDrawerCallbacks callbacks);

} // namespace Iridium
