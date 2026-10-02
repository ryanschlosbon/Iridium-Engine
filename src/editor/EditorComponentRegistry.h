#pragma once

#include "ecs/Registry.h"
#include "scene/runtime/RuntimeComponentRegistry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Iridium {

    using HasEditorComponentFn = bool (*)(const Registry&, Entity);
    using GetEditorComponentFn = void* (*)(Registry&, Entity);
    using AddEditorComponentFn = bool (*)(Registry&, Entity);
    using RemoveEditorComponentFn = bool (*)(Registry&, Entity);
    using EditorComponentSnapshot = std::shared_ptr<const void>;
    using CaptureEditorComponentFn = EditorComponentSnapshot (*)(
        const Registry&, Entity);
    using RestoreEditorComponentFn = bool (*)(
        Registry&, Entity, const EditorComponentSnapshot&);
    using GetEditorPropertyFn = std::function<void*(void*)>;

    struct EditorPropertyDescriptor {
        PropertyId id;
        std::string displayName;
        uint32_t displayOrder = 0;
        PropertyValueType valueType = PropertyValueType::String;
        bool readOnly = false;
        bool nullable = false;
        PropertyDefaultValue defaultValue;
        std::optional<float> minimum;
        std::optional<float> maximum;
        std::vector<std::string> enumLabels;
        size_t valueSize = 0;
        GetEditorPropertyFn getMutable;
    };

    struct EditorComponentDescriptor {
        ComponentTypeId id;
        std::string displayName;
        uint32_t displayOrder = 0;
        float preferredBodyHeight = 180.0f;
        bool visible = true;
        bool required = false;
        bool addable = false;
        std::vector<EditorPropertyDescriptor> properties;
        HasEditorComponentFn has = nullptr;
        GetEditorComponentFn getMutable = nullptr;
        AddEditorComponentFn add = nullptr;
        RemoveEditorComponentFn remove = nullptr;
        CaptureEditorComponentFn capture = nullptr;
        RestoreEditorComponentFn restore = nullptr;
    };

    enum class EditorComponentRegistryError {
        None,
        Frozen,
        NotFrozen,
        InvalidComponentId,
        DuplicateComponentId,
        EmptyDisplayName,
        DuplicateDisplayOrder,
        InvalidBodyHeight,
        MissingComponentBinding,
        InvalidRequiredPolicy,
        MissingAddCallback,
        MissingRemoveCallback,
        InvalidPropertyId,
        DuplicatePropertyId,
        EmptyPropertyDisplayName,
        DuplicatePropertyOrder,
        MissingPropertyBinding,
        InvalidPropertyBindingSize,
        InvalidPropertyRange,
        InvalidEnumLabels,
        InvalidPropertyDefault,
        InvalidNullableProperty,
    };

    struct EditorComponentRegistryResult {
        EditorComponentRegistryError error =
            EditorComponentRegistryError::None;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == EditorComponentRegistryError::None;
        }
    };

    class EditorComponentRegistry {
    public:
        [[nodiscard]] EditorComponentRegistryResult add(
            EditorComponentDescriptor descriptor);
        [[nodiscard]] EditorComponentRegistryResult freezeAndValidate();

        [[nodiscard]] const EditorComponentDescriptor* find(
            const ComponentTypeId& id) const noexcept;
        [[nodiscard]] std::span<const EditorComponentDescriptor> descriptors()
            const noexcept;
        [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    private:
        std::vector<EditorComponentDescriptor> descriptors_;
        bool frozen_ = false;
    };

    template <typename T>
    [[nodiscard]] EditorComponentDescriptor editorComponentDescriptor(
        std::string_view stableId,
        std::string displayName,
        uint32_t displayOrder,
        float preferredBodyHeight,
        bool visible,
        bool required,
        bool addable) {
        EditorComponentDescriptor descriptor;
        if (const auto id = ComponentTypeId::parse(stableId)) {
            descriptor.id = *id;
        }
        descriptor.displayName = std::move(displayName);
        descriptor.displayOrder = displayOrder;
        descriptor.preferredBodyHeight = preferredBodyHeight;
        descriptor.visible = visible;
        descriptor.required = required;
        descriptor.addable = addable;
        descriptor.has = [](const Registry& registry, Entity entity) {
            const auto* pool = registry.findPool<T>();
            return pool && pool->has(entity);
        };
        descriptor.getMutable = [](Registry& registry, Entity entity) -> void* {
            auto* pool = registry.findPool<T>();
            return pool && pool->has(entity) ? &pool->get(entity) : nullptr;
        };
        descriptor.add = [](Registry& registry, Entity entity) {
            auto* pool = registry.findPool<T>();
            if (pool && pool->has(entity)) return false;
            registry.addComponent<T>(entity);
            return true;
        };
        descriptor.remove = [](Registry& registry, Entity entity) {
            auto* pool = registry.findPool<T>();
            if (!pool || !pool->has(entity)) return false;
            pool->remove(entity);
            return true;
        };
        descriptor.capture = [](const Registry& registry, Entity entity)
            -> EditorComponentSnapshot {
            const auto* pool = registry.findPool<T>();
            return pool && pool->has(entity)
                ? std::make_shared<T>(pool->get(entity)) : nullptr;
        };
        descriptor.restore = [](Registry& registry, Entity entity,
            const EditorComponentSnapshot& snapshot) {
            if (!snapshot) return false;
            auto* pool = registry.findPool<T>();
            if (pool && pool->has(entity)) return false;
            registry.addComponent<T>(entity,
                *static_cast<const T*>(snapshot.get()));
            return true;
        };
        return descriptor;
    }

    template <typename Component, typename Value>
    [[nodiscard]] EditorPropertyDescriptor editorPropertyDescriptor(
        std::string_view stableId,
        std::string displayName,
        uint32_t displayOrder,
        PropertyValueType valueType,
        Value Component::* member,
        bool readOnly = false,
        bool nullable = false,
        PropertyDefaultValue defaultValue = {},
        std::optional<float> minimum = std::nullopt,
        std::optional<float> maximum = std::nullopt,
        std::vector<std::string> enumLabels = {}) {
        EditorPropertyDescriptor descriptor;
        if (const auto id = PropertyId::parse(stableId)) {
            descriptor.id = *id;
        }
        descriptor.displayName = std::move(displayName);
        descriptor.displayOrder = displayOrder;
        descriptor.valueType = valueType;
        descriptor.readOnly = readOnly;
        descriptor.nullable = nullable;
        descriptor.defaultValue = std::move(defaultValue);
        descriptor.minimum = minimum;
        descriptor.maximum = maximum;
        descriptor.enumLabels = std::move(enumLabels);
        descriptor.valueSize = sizeof(Value);
        descriptor.getMutable = [member](void* component) -> void* {
            if (!component) return nullptr;
            return &(static_cast<Component*>(component)->*member);
        };
        return descriptor;
    }

} // namespace Iridium
