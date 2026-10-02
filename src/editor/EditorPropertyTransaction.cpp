#include "editor/EditorPropertyTransaction.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] bool valueMatchesType(
            PropertyValueType type, const EditorPropertyValue& value) {
            switch (type) {
            case PropertyValueType::Boolean:
                return std::holds_alternative<bool>(value);
            case PropertyValueType::Int32:
            case PropertyValueType::Enum:
                return std::holds_alternative<int32_t>(value);
            case PropertyValueType::Float32:
                return std::holds_alternative<float>(value);
            case PropertyValueType::String:
                return std::holds_alternative<std::string>(value);
            case PropertyValueType::Float32x3:
                return std::holds_alternative<glm::vec3>(value);
            default:
                return false;
            }
        }

        [[nodiscard]] EditorMutationResult validateValue(
            PropertyValueType type, bool readOnly,
            const std::optional<float>& minimum,
            const std::optional<float>& maximum,
            size_t enumLabelCount,
            const EditorPropertyValue& value) {
            if (readOnly) {
                return EditorMutationResult::failure(
                    "Property is read-only");
            }
            if (!valueMatchesType(type, value)) {
                return EditorMutationResult::failure(
                    "Property value type does not match its descriptor");
            }
            if (type == PropertyValueType::Int32 ||
                type == PropertyValueType::Enum) {
                const int32_t integer = std::get<int32_t>(value);
                if (minimum && integer < *minimum) {
                    return EditorMutationResult::failure(
                        "Property value is below its minimum");
                }
                if (maximum && integer > *maximum) {
                    return EditorMutationResult::failure(
                        "Property value is above its maximum");
                }
                if (type == PropertyValueType::Enum &&
                    (integer < 0 || static_cast<size_t>(integer) >=
                        enumLabelCount)) {
                    return EditorMutationResult::failure(
                        "Enum property value is outside its labels");
                }
            }
            if (type == PropertyValueType::Float32) {
                const float scalar = std::get<float>(value);
                if (!std::isfinite(scalar) ||
                    (minimum && scalar < *minimum) ||
                    (maximum && scalar > *maximum)) {
                    return EditorMutationResult::failure(
                        "Float property value is outside its valid range");
                }
            }
            if (type == PropertyValueType::Float32x3) {
                const glm::vec3 vector = std::get<glm::vec3>(value);
                if (!std::isfinite(vector.x) || !std::isfinite(vector.y) ||
                    !std::isfinite(vector.z)) {
                    return EditorMutationResult::failure(
                        "Vector property contains a non-finite value");
                }
            }
            return EditorMutationResult::applied();
        }

    } // namespace

    std::optional<EditorPropertyValue> captureEditorPropertyValue(
        const EditorPropertyDescriptor& property,
        const void* component) {
        if (!component || !property.getMutable) return std::nullopt;
        void* value = property.getMutable(const_cast<void*>(component));
        if (!value) return std::nullopt;
        switch (property.valueType) {
        case PropertyValueType::Boolean:
            return *static_cast<const bool*>(value);
        case PropertyValueType::Int32:
            return *static_cast<const int32_t*>(value);
        case PropertyValueType::Float32:
            return *static_cast<const float*>(value);
        case PropertyValueType::String:
            return *static_cast<const std::string*>(value);
        case PropertyValueType::Float32x3:
            return *static_cast<const glm::vec3*>(value);
        case PropertyValueType::Enum: {
            int32_t integer = 0;
            std::memcpy(&integer, value, sizeof(integer));
            return integer;
        }
        default:
            return std::nullopt;
        }
    }

    EditorMutationResult writeEditorPropertyValue(
        const EditorPropertyDescriptor& property,
        void* component,
        const EditorPropertyValue& value) {
        if (!component || !property.getMutable) {
            return EditorMutationResult::failure(
                "Property target is unavailable");
        }
        const EditorMutationResult valid = validateValue(
            property.valueType, property.readOnly, property.minimum,
            property.maximum, property.enumLabels.size(), value);
        if (!valid) return valid;
        void* destination = property.getMutable(component);
        if (!destination) {
            return EditorMutationResult::failure(
                "Property target is null");
        }
        switch (property.valueType) {
        case PropertyValueType::Boolean:
            *static_cast<bool*>(destination) = std::get<bool>(value);
            break;
        case PropertyValueType::Int32:
            *static_cast<int32_t*>(destination) = std::get<int32_t>(value);
            break;
        case PropertyValueType::Float32:
            *static_cast<float*>(destination) = std::get<float>(value);
            break;
        case PropertyValueType::String:
            *static_cast<std::string*>(destination) =
                std::get<std::string>(value);
            break;
        case PropertyValueType::Float32x3:
            *static_cast<glm::vec3*>(destination) = std::get<glm::vec3>(value);
            break;
        case PropertyValueType::Enum: {
            const int32_t integer = std::get<int32_t>(value);
            std::memcpy(destination, &integer, sizeof(integer));
            break;
        }
        default:
            return EditorMutationResult::failure(
                "Property type requires a custom drawer");
        }
        return EditorMutationResult::applied();
    }

    bool sameEditorPropertyValue(const EditorPropertyValue& lhs,
        const EditorPropertyValue& rhs) noexcept {
        if (lhs.index() != rhs.index()) return false;
        switch (lhs.index()) {
        case 0: return std::get<bool>(lhs) == std::get<bool>(rhs);
        case 1: return std::get<int32_t>(lhs) == std::get<int32_t>(rhs);
        case 2: return std::get<float>(lhs) == std::get<float>(rhs);
        case 3: return std::get<std::string>(lhs) ==
            std::get<std::string>(rhs);
        case 4:
            return glm::all(glm::equal(
                std::get<glm::vec3>(lhs), std::get<glm::vec3>(rhs)));
        default:
            return false;
        }
    }

    EditorTransactionOperation makeEditorPropertyOperation(
        Registry& registry, Entity entity,
        const EditorComponentDescriptor& component,
        const EditorPropertyDescriptor& property,
        EditorPropertyValue before,
        EditorPropertyValue after) {
        struct State {
            Registry* registry = nullptr;
            Entity entity = NULL_ENTITY;
            GetEditorComponentFn getComponent = nullptr;
            EditorPropertyDescriptor descriptor;
            EditorPropertyValue before;
            EditorPropertyValue after;

            [[nodiscard]] EditorMutationResult write(
                const EditorPropertyValue& expected,
                const EditorPropertyValue& replacement) {
                void* component = getComponent(*registry, entity);
                if (!component) {
                    return EditorMutationResult::failure(
                        "Property component is no longer available");
                }
                const auto current = captureEditorPropertyValue(
                    descriptor, component);
                if (!current) {
                    return EditorMutationResult::failure(
                        "Property value is no longer available");
                }
                if (sameEditorPropertyValue(*current, replacement)) {
                    return EditorMutationResult::noChange();
                }
                if (!sameEditorPropertyValue(*current, expected)) {
                    return EditorMutationResult::failure(
                        "Property changed outside transaction history");
                }
                return writeEditorPropertyValue(
                    descriptor, component, replacement);
            }
        };
        EditorPropertyDescriptor stableProperty = property;
        stableProperty.getMutable = property.getMutable;
        auto state = std::make_shared<State>(State{
            .registry = &registry,
            .entity = entity,
            .getComponent = component.getMutable,
            .descriptor = std::move(stableProperty),
            .before = std::move(before),
            .after = std::move(after),
        });
        return {
            .target = component.id.value() + "/" + property.id.value(),
            .apply = [state] { return state->write(state->before, state->after); },
            .revert = [state] { return state->write(state->after, state->before); },
            .estimatedPayloadBytes = sizeof(State),
        };
    }

} // namespace Iridium
