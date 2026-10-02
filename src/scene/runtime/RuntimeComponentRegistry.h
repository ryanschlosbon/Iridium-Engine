#pragma once

#include "ecs/Entity.h"
#include "scene/runtime/ComponentIdentity.h"

#include <cstdint>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <variant>
#include <vector>

class Registry;

namespace Iridium {

    class SceneIdentityMap;
    class SceneReferenceState;
    class CookedComponentWriter;
    class CookedComponentReader;

    enum class PropertyValueType {
        Boolean,
        Int32,
        Float32,
        String,
        Float32x3,
        Enum,
        EntityReference,
        AssetReference,
        SubassetReference,
        Collection,
    };

    enum class PropertyReferenceKind {
        None,
        Entity,
        Asset,
        Subasset,
    };

    struct EmptyCollectionDefault {
        auto operator<=>(const EmptyCollectionDefault&) const = default;
    };

    using PropertyDefaultValue = std::variant<
        std::monostate,
        std::nullptr_t,
        bool,
        int32_t,
        float,
        std::string,
        std::array<float, 3>,
        EmptyCollectionDefault>;

    enum class CollectionOrdering {
        Preserve,
        SourceSubassetGuid,
    };

    struct PropertyDescriptor {
        PropertyId id;
        PropertyValueType valueType = PropertyValueType::String;
        PropertyReferenceKind referenceKind = PropertyReferenceKind::None;
        uint32_t serializationOrder = 0;
        bool required = false;
        bool nullable = false;
        PropertyDefaultValue defaultValue;
        CollectionOrdering collectionOrdering = CollectionOrdering::Preserve;
    };

    using ResolveComponentReferencesFn = bool (*)(
        Registry&, Entity, const SceneIdentityMap&, SceneReferenceState&);
    using ValidateRuntimeComponentFn = bool (*)(const Registry&, Entity);
    using EncodeCookedComponentFn = bool (*)(
        const Registry&, Entity, CookedComponentWriter&);
    using DecodeCookedComponentFn = bool (*)(
        Registry&, Entity, CookedComponentReader&);

    struct RuntimeComponentDescriptor {
        ComponentTypeId id;
        CookedSectionId cookedSectionId;
        uint32_t currentCookedVersion = 0;
        std::vector<PropertyDescriptor> properties;
        ResolveComponentReferencesFn resolveReferences = nullptr;
        ValidateRuntimeComponentFn postLoadValidate = nullptr;
        EncodeCookedComponentFn encodeCooked = nullptr;
        DecodeCookedComponentFn decodeCooked = nullptr;
    };

    enum class ComponentRegistryError {
        None,
        Frozen,
        NotFrozen,
        InvalidComponentId,
        DuplicateComponentId,
        InvalidCookedSectionId,
        DuplicateCookedSectionId,
        InvalidCookedVersion,
        InvalidPropertyId,
        DuplicatePropertyId,
        DuplicatePropertyOrder,
        InvalidPropertyReference,
        InvalidPropertyDefault,
        InvalidCollectionOrdering,
        MissingCallback,
    };

    struct ComponentRegistryResult {
        ComponentRegistryError error = ComponentRegistryError::None;
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept {
            return error == ComponentRegistryError::None;
        }
    };

    class RuntimeComponentRegistry {
    public:
        [[nodiscard]] ComponentRegistryResult add(
            RuntimeComponentDescriptor descriptor);
        [[nodiscard]] ComponentRegistryResult freezeAndValidate();

        [[nodiscard]] const RuntimeComponentDescriptor* find(
            const ComponentTypeId& id) const noexcept;
        [[nodiscard]] std::span<const RuntimeComponentDescriptor> descriptors()
            const noexcept;
        [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

    private:
        std::vector<RuntimeComponentDescriptor> descriptors_;
        bool frozen_ = false;
    };

    // Stable hash of the frozen cooked contract. Function pointers, RTTI, labels,
    // and editor presentation are deliberately excluded.
    [[nodiscard]] std::string runtimeComponentManifestHash(
        const RuntimeComponentRegistry& registry);

} // namespace Iridium
