#include "scene/runtime/RuntimeComponentRegistry.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Iridium {
    namespace {

        [[nodiscard]] ComponentRegistryResult validateReference(
            const PropertyDescriptor& property) {
            const bool referenceType =
                property.valueType == PropertyValueType::EntityReference ||
                property.valueType == PropertyValueType::AssetReference ||
                property.valueType == PropertyValueType::SubassetReference;
            const PropertyReferenceKind expected =
                property.valueType == PropertyValueType::EntityReference
                    ? PropertyReferenceKind::Entity
                : property.valueType == PropertyValueType::AssetReference
                    ? PropertyReferenceKind::Asset
                : property.valueType == PropertyValueType::SubassetReference
                    ? PropertyReferenceKind::Subasset
                    : PropertyReferenceKind::None;
            if (referenceType !=
                    (property.referenceKind != PropertyReferenceKind::None) ||
                property.referenceKind != expected) {
                return { ComponentRegistryError::InvalidPropertyReference,
                    "Property reference kind does not match its value type" };
            }
            return {};
        }

        [[nodiscard]] ComponentRegistryResult validateDefault(
            const PropertyDescriptor& property) {
            const bool none = std::holds_alternative<std::monostate>(
                property.defaultValue);
            if (none) return {};
            if (property.required) {
                return { ComponentRegistryError::InvalidPropertyDefault,
                    "Required property cannot also declare a default" };
            }
            if (std::holds_alternative<std::nullptr_t>(property.defaultValue)) {
                return property.nullable
                    ? ComponentRegistryResult{}
                    : ComponentRegistryResult{
                        ComponentRegistryError::InvalidPropertyDefault,
                        "Null default requires a nullable property" };
            }
            bool matches = false;
            switch (property.valueType) {
            case PropertyValueType::Boolean:
                matches = std::holds_alternative<bool>(property.defaultValue); break;
            case PropertyValueType::Int32:
            case PropertyValueType::Enum:
                matches = std::holds_alternative<int32_t>(property.defaultValue); break;
            case PropertyValueType::Float32:
                matches = std::holds_alternative<float>(property.defaultValue); break;
            case PropertyValueType::String:
                matches = std::holds_alternative<std::string>(property.defaultValue); break;
            case PropertyValueType::Float32x3:
                matches = std::holds_alternative<std::array<float, 3>>(
                    property.defaultValue); break;
            case PropertyValueType::Collection:
                matches = std::holds_alternative<EmptyCollectionDefault>(
                    property.defaultValue); break;
            case PropertyValueType::EntityReference:
            case PropertyValueType::AssetReference:
            case PropertyValueType::SubassetReference:
                matches = false; break;
            }
            return matches
                ? ComponentRegistryResult{}
                : ComponentRegistryResult{
                    ComponentRegistryError::InvalidPropertyDefault,
                    "Property default does not match its value type" };
        }

        [[nodiscard]] ComponentRegistryResult validateDescriptor(
            const RuntimeComponentDescriptor& descriptor) {
            if (descriptor.id.empty()) {
                return { ComponentRegistryError::InvalidComponentId,
                    "Runtime component ID is empty or invalid" };
            }
            if (descriptor.cookedSectionId.empty()) {
                return { ComponentRegistryError::InvalidCookedSectionId,
                    "Runtime component cooked section ID is empty" };
            }
            if (descriptor.currentCookedVersion == 0) {
                return { ComponentRegistryError::InvalidCookedVersion,
                    "Runtime component cooked version zero is reserved" };
            }
            if (!descriptor.resolveReferences || !descriptor.postLoadValidate ||
                !descriptor.encodeCooked || !descriptor.decodeCooked) {
                return { ComponentRegistryError::MissingCallback,
                    "Runtime component descriptor is missing a required callback" };
            }

            std::unordered_set<PropertyId, PropertyIdHash> propertyIds;
            std::unordered_set<uint32_t> propertyOrders;
            for (const PropertyDescriptor& property : descriptor.properties) {
                if (property.id.empty()) {
                    return { ComponentRegistryError::InvalidPropertyId,
                        "Runtime property ID is empty or invalid" };
                }
                if (!propertyIds.insert(property.id).second) {
                    return { ComponentRegistryError::DuplicatePropertyId,
                        "Runtime component contains a duplicate property ID" };
                }
                if (!propertyOrders.insert(property.serializationOrder).second) {
                    return { ComponentRegistryError::DuplicatePropertyOrder,
                        "Runtime component contains a duplicate property order" };
                }
                if (const ComponentRegistryResult result =
                        validateReference(property); !result) {
                    return result;
                }
                if (const ComponentRegistryResult result =
                        validateDefault(property); !result) {
                    return result;
                }
                if (property.collectionOrdering != CollectionOrdering::Preserve &&
                    property.valueType != PropertyValueType::Collection) {
                    return { ComponentRegistryError::InvalidCollectionOrdering,
                        "Collection ordering requires a collection property" };
                }
            }
            return {};
        }

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& bytes, Integer value) {
            using Unsigned = std::make_unsigned_t<Integer>;
            const Unsigned bits = static_cast<Unsigned>(value);
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bytes.push_back(static_cast<std::byte>(bits >> (index * 8)));
            }
        }

        void appendString(std::vector<std::byte>& bytes, std::string_view value) {
            appendInteger<uint64_t>(bytes, value.size());
            bytes.insert(bytes.end(),
                reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data() + value.size()));
        }

        void appendDefault(std::vector<std::byte>& bytes,
            const PropertyDefaultValue& value) {
            appendInteger<uint8_t>(bytes, static_cast<uint8_t>(value.index()));
            std::visit([&](const auto& field) {
                using T = std::decay_t<decltype(field)>;
                if constexpr (std::is_same_v<T, bool>) {
                    appendInteger<uint8_t>(bytes, field ? 1 : 0);
                }
                else if constexpr (std::is_same_v<T, int32_t>) {
                    appendInteger(bytes, field);
                }
                else if constexpr (std::is_same_v<T, float>) {
                    appendInteger(bytes, std::bit_cast<uint32_t>(field));
                }
                else if constexpr (std::is_same_v<T, std::string>) {
                    appendString(bytes, field);
                }
                else if constexpr (std::is_same_v<T, std::array<float, 3>>) {
                    for (float element : field) {
                        appendInteger(bytes, std::bit_cast<uint32_t>(element));
                    }
                }
            }, value);
        }

    } // namespace

    ComponentRegistryResult RuntimeComponentRegistry::add(
        RuntimeComponentDescriptor descriptor) {
        if (frozen_) {
            return { ComponentRegistryError::Frozen,
                "Runtime component registry is already frozen" };
        }
        if (const ComponentRegistryResult result = validateDescriptor(descriptor);
            !result) {
            return result;
        }
        for (const RuntimeComponentDescriptor& existing : descriptors_) {
            if (existing.id == descriptor.id) {
                return { ComponentRegistryError::DuplicateComponentId,
                    "Runtime component ID is already registered" };
            }
            if (existing.cookedSectionId == descriptor.cookedSectionId) {
                return { ComponentRegistryError::DuplicateCookedSectionId,
                    "Runtime cooked section ID is already registered" };
            }
        }
        std::ranges::sort(descriptor.properties,
            [](const PropertyDescriptor& lhs, const PropertyDescriptor& rhs) {
                if (lhs.serializationOrder != rhs.serializationOrder) {
                    return lhs.serializationOrder < rhs.serializationOrder;
                }
                return lhs.id < rhs.id;
            });
        descriptors_.push_back(std::move(descriptor));
        return {};
    }

    ComponentRegistryResult RuntimeComponentRegistry::freezeAndValidate() {
        if (frozen_) return {};
        std::ranges::sort(descriptors_,
            [](const RuntimeComponentDescriptor& lhs,
                const RuntimeComponentDescriptor& rhs) {
                return lhs.id < rhs.id;
            });
        frozen_ = true;
        return {};
    }

    const RuntimeComponentDescriptor* RuntimeComponentRegistry::find(
        const ComponentTypeId& id) const noexcept {
        if (!frozen_) return nullptr;
        const auto found = std::ranges::lower_bound(descriptors_, id, {},
            &RuntimeComponentDescriptor::id);
        return found != descriptors_.end() && found->id == id
            ? &*found
            : nullptr;
    }

    std::span<const RuntimeComponentDescriptor>
    RuntimeComponentRegistry::descriptors() const noexcept {
        return frozen_
            ? std::span<const RuntimeComponentDescriptor>(descriptors_)
            : std::span<const RuntimeComponentDescriptor>();
    }

    std::string runtimeComponentManifestHash(
        const RuntimeComponentRegistry& registry) {
        if (!registry.isFrozen()) return {};
        std::vector<std::byte> bytes;
        appendString(bytes, "IridiumRuntimeComponentManifest/v1");
        appendInteger<uint32_t>(bytes,
            static_cast<uint32_t>(registry.descriptors().size()));
        for (const RuntimeComponentDescriptor& descriptor :
            registry.descriptors()) {
            appendString(bytes, descriptor.id.value());
            appendInteger(bytes, descriptor.cookedSectionId.value());
            appendInteger(bytes, descriptor.currentCookedVersion);
            appendInteger<uint32_t>(bytes,
                static_cast<uint32_t>(descriptor.properties.size()));
            for (const PropertyDescriptor& property : descriptor.properties) {
                appendString(bytes, property.id.value());
                appendInteger<uint8_t>(bytes,
                    static_cast<uint8_t>(property.valueType));
                appendInteger<uint8_t>(bytes,
                    static_cast<uint8_t>(property.referenceKind));
                appendInteger(bytes, property.serializationOrder);
                appendInteger<uint8_t>(bytes, property.required ? 1 : 0);
                appendInteger<uint8_t>(bytes, property.nullable ? 1 : 0);
                appendInteger<uint8_t>(bytes,
                    static_cast<uint8_t>(property.collectionOrdering));
                appendDefault(bytes, property.defaultValue);
            }
        }
        return sha256(bytes);
    }

} // namespace Iridium
