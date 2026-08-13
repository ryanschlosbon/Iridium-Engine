#include "editor/EditorComponentRegistry.h"

#include <algorithm>
#include <cmath>

namespace Iridium {
    namespace {

        [[nodiscard]] EditorComponentRegistryResult failure(
            EditorComponentRegistryError error,
            std::string message) {
            return { error, std::move(message) };
        }

        [[nodiscard]] bool isReference(PropertyValueType type) noexcept {
            return type == PropertyValueType::EntityReference ||
                type == PropertyValueType::AssetReference ||
                type == PropertyValueType::SubassetReference;
        }

        [[nodiscard]] bool defaultMatches(
            const EditorPropertyDescriptor& property) noexcept {
            if (std::holds_alternative<std::monostate>(property.defaultValue)) {
                return true;
            }
            if (std::holds_alternative<std::nullptr_t>(property.defaultValue)) {
                return property.nullable && isReference(property.valueType);
            }
            switch (property.valueType) {
            case PropertyValueType::Boolean:
                return std::holds_alternative<bool>(property.defaultValue);
            case PropertyValueType::Int32:
            case PropertyValueType::Enum:
                return std::holds_alternative<int32_t>(property.defaultValue);
            case PropertyValueType::Float32:
                return std::holds_alternative<float>(property.defaultValue);
            case PropertyValueType::String:
                return std::holds_alternative<std::string>(property.defaultValue);
            case PropertyValueType::Float32x3:
                return std::holds_alternative<std::array<float, 3>>(
                    property.defaultValue);
            case PropertyValueType::Collection:
                return std::holds_alternative<EmptyCollectionDefault>(
                    property.defaultValue);
            case PropertyValueType::EntityReference:
            case PropertyValueType::AssetReference:
            case PropertyValueType::SubassetReference:
                return false;
            }
            return false;
        }

        [[nodiscard]] bool bindingSizeMatches(
            const EditorPropertyDescriptor& property) noexcept {
            switch (property.valueType) {
            case PropertyValueType::Boolean:
                return property.valueSize == sizeof(bool);
            case PropertyValueType::Int32:
            case PropertyValueType::Enum:
                return property.valueSize == sizeof(int32_t);
            case PropertyValueType::Float32:
                return property.valueSize == sizeof(float);
            case PropertyValueType::String:
                return property.valueSize == sizeof(std::string);
            case PropertyValueType::Float32x3:
                return property.valueSize == sizeof(float) * 3;
            case PropertyValueType::EntityReference:
                return property.valueSize == sizeof(Entity);
            case PropertyValueType::AssetReference:
            case PropertyValueType::SubassetReference:
            case PropertyValueType::Collection:
                return property.valueSize != 0;
            }
            return false;
        }

    } // namespace

    EditorComponentRegistryResult EditorComponentRegistry::add(
        EditorComponentDescriptor descriptor) {
        if (frozen_) {
            return failure(EditorComponentRegistryError::Frozen,
                "Editor component registry is frozen");
        }
        if (descriptor.id.empty()) {
            return failure(EditorComponentRegistryError::InvalidComponentId,
                "Editor component stable ID is invalid");
        }
        if (descriptor.displayName.empty()) {
            return failure(EditorComponentRegistryError::EmptyDisplayName,
                "Editor component display name cannot be empty");
        }
        if (!std::isfinite(descriptor.preferredBodyHeight) ||
            descriptor.preferredBodyHeight <= 0.0f) {
            return failure(EditorComponentRegistryError::InvalidBodyHeight,
                "Editor component body height must be finite and positive");
        }
        if (!descriptor.has || !descriptor.getMutable) {
            return failure(EditorComponentRegistryError::MissingComponentBinding,
                "Editor component requires has/get bindings");
        }
        if (descriptor.required && descriptor.addable) {
            return failure(EditorComponentRegistryError::InvalidRequiredPolicy,
                "A required component cannot be offered by Add Component");
        }
        if (descriptor.addable && !descriptor.add) {
            return failure(EditorComponentRegistryError::MissingAddCallback,
                "An addable component requires an add callback");
        }
        if (!descriptor.required && !descriptor.remove) {
            return failure(EditorComponentRegistryError::MissingRemoveCallback,
                "A removable component requires a remove callback");
        }
        if (find(descriptor.id)) {
            return failure(EditorComponentRegistryError::DuplicateComponentId,
                "Duplicate editor component stable ID: " +
                    descriptor.id.value());
        }
        if (std::ranges::any_of(descriptors_, [&](const auto& candidate) {
                return candidate.displayOrder == descriptor.displayOrder;
            })) {
            return failure(EditorComponentRegistryError::DuplicateDisplayOrder,
                "Duplicate editor component display order");
        }
        for (size_t index = 0; index < descriptor.properties.size(); ++index) {
            auto& property = descriptor.properties[index];
            if (property.id.empty()) {
                return failure(EditorComponentRegistryError::InvalidPropertyId,
                    "Editor property stable ID is invalid");
            }
            if (property.displayName.empty()) {
                return failure(
                    EditorComponentRegistryError::EmptyPropertyDisplayName,
                    "Editor property display name cannot be empty");
            }
            if (!property.getMutable) {
                return failure(
                    EditorComponentRegistryError::MissingPropertyBinding,
                    "Editor property requires a typed binding");
            }
            if (!bindingSizeMatches(property)) {
                return failure(
                    EditorComponentRegistryError::InvalidPropertyBindingSize,
                    "Editor property binding size does not match its value type");
            }
            if (property.nullable && !isReference(property.valueType)) {
                return failure(
                    EditorComponentRegistryError::InvalidNullableProperty,
                    "Only stable reference properties may be nullable");
            }
            if (!defaultMatches(property)) {
                return failure(
                    EditorComponentRegistryError::InvalidPropertyDefault,
                    "Editor property default does not match its value type");
            }
            if (property.minimum.has_value() != property.maximum.has_value() ||
                (property.minimum &&
                    (!std::isfinite(*property.minimum) ||
                        !std::isfinite(*property.maximum) ||
                        *property.minimum > *property.maximum)) ||
                (property.minimum &&
                    property.valueType != PropertyValueType::Float32 &&
                    property.valueType != PropertyValueType::Int32)) {
                return failure(EditorComponentRegistryError::InvalidPropertyRange,
                    "Editor property range must be a finite ordered pair");
            }
            if (!property.enumLabels.empty() &&
                property.valueType != PropertyValueType::Enum) {
                return failure(EditorComponentRegistryError::InvalidEnumLabels,
                    "Only enum properties may declare enum labels");
            }
            for (size_t previous = 0; previous < index; ++previous) {
                const auto& candidate = descriptor.properties[previous];
                if (candidate.id == property.id) {
                    return failure(
                        EditorComponentRegistryError::DuplicatePropertyId,
                        "Duplicate editor property stable ID: " +
                            property.id.value());
                }
                if (candidate.displayOrder == property.displayOrder) {
                    return failure(
                        EditorComponentRegistryError::DuplicatePropertyOrder,
                        "Duplicate editor property display order");
                }
            }
        }
        std::ranges::sort(descriptor.properties,
            [](const auto& left, const auto& right) {
                if (left.displayOrder != right.displayOrder) {
                    return left.displayOrder < right.displayOrder;
                }
                return left.id < right.id;
            });
        descriptors_.push_back(std::move(descriptor));
        return {};
    }

    EditorComponentRegistryResult
    EditorComponentRegistry::freezeAndValidate() {
        if (frozen_) {
            return failure(EditorComponentRegistryError::Frozen,
                "Editor component registry is already frozen");
        }
        std::ranges::sort(descriptors_, [](const auto& left, const auto& right) {
            if (left.displayOrder != right.displayOrder) {
                return left.displayOrder < right.displayOrder;
            }
            return left.id < right.id;
        });
        frozen_ = true;
        return {};
    }

    const EditorComponentDescriptor* EditorComponentRegistry::find(
        const ComponentTypeId& id) const noexcept {
        const auto found = std::ranges::find_if(descriptors_,
            [&](const auto& descriptor) { return descriptor.id == id; });
        return found == descriptors_.end() ? nullptr : &*found;
    }

    std::span<const EditorComponentDescriptor>
    EditorComponentRegistry::descriptors() const noexcept {
        return frozen_
            ? std::span<const EditorComponentDescriptor>(descriptors_)
            : std::span<const EditorComponentDescriptor>{};
    }

} // namespace Iridium
