#include "editor/EditorComponentRegistry.h"
#include "editor/EditorComponentDrawerRegistry.h"
#include "editor/CoreEditorComponentRegistry.h"
#include "scene/runtime/CoreComponentRegistry.h"
#include "scene/runtime/CoreComponentIds.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

    int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                            \
                      << " CHECK failed: " #condition << '\n';                 \
            ++failures;                                                        \
        }                                                                       \
    } while (false)

    struct FirstComponent {
        int value = 7;
    };

    struct SecondComponent {
        bool enabled = true;
    };

    bool resolveReferences(Registry&, Entity,
        const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) {
        return true;
    }

    bool validateRuntime(const Registry&, Entity) {
        return true;
    }

    bool encodeCooked(const Registry&, Entity,
        Iridium::CookedComponentWriter&) {
        return true;
    }

    bool decodeCooked(Registry&, Entity,
        Iridium::CookedComponentReader&) {
        return true;
    }

    [[nodiscard]] Iridium::ComponentTypeId id(std::string_view value) {
        return *Iridium::ComponentTypeId::parse(value);
    }

    void testValidationAndFreeze() {
        Iridium::EditorComponentRegistry registry;

        auto invalid = Iridium::editorComponentDescriptor<FirstComponent>(
            "INVALID", "Invalid", 0, 100.0f, true, false, true);
        CHECK(registry.add(std::move(invalid)).error ==
            Iridium::EditorComponentRegistryError::InvalidComponentId);

        auto first = Iridium::editorComponentDescriptor<FirstComponent>(
            "test.component.first", "First", 20, 120.0f,
            true, false, true);
        CHECK(registry.add(first));
        CHECK(registry.add(first).error ==
            Iridium::EditorComponentRegistryError::DuplicateComponentId);

        auto duplicateOrder =
            Iridium::editorComponentDescriptor<SecondComponent>(
                "test.component.second", "Second", 20, 80.0f,
                true, false, true);
        CHECK(registry.add(std::move(duplicateOrder)).error ==
            Iridium::EditorComponentRegistryError::DuplicateDisplayOrder);

        auto second = Iridium::editorComponentDescriptor<SecondComponent>(
            "test.component.second", "Second", 10, 80.0f,
            true, true, false);
        CHECK(registry.add(std::move(second)));
        CHECK(registry.descriptors().empty());
        CHECK(registry.freezeAndValidate());
        CHECK(registry.isFrozen());
        CHECK(registry.descriptors().size() == 2);
        CHECK(registry.descriptors()[0].id == id("test.component.second"));
        CHECK(registry.descriptors()[1].id == id("test.component.first"));
        CHECK(registry.find(id("test.component.first")) ==
            &registry.descriptors()[1]);
        CHECK(registry.add(first).error ==
            Iridium::EditorComponentRegistryError::Frozen);
        CHECK(registry.freezeAndValidate().error ==
            Iridium::EditorComponentRegistryError::Frozen);
    }

    void testTypedBindings() {
        Iridium::EditorComponentRegistry descriptors;
        auto component =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.first", "First", 0, 100.0f,
                true, false, true);
        component.properties.push_back(Iridium::editorPropertyDescriptor(
            "value", "Value", 0, Iridium::PropertyValueType::Int32,
            &FirstComponent::value, false, false, int32_t{ 7 },
            0.0f, 100.0f));
        CHECK(descriptors.add(std::move(component)));
        CHECK(descriptors.freezeAndValidate());
        const auto* descriptor = descriptors.find(
            id("test.component.first"));
        CHECK(descriptor != nullptr);
        if (!descriptor) return;

        Registry registry;
        const Entity entity = registry.createEntity();
        CHECK(!descriptor->has(registry, entity));
        CHECK(descriptor->getMutable(registry, entity) == nullptr);
        CHECK(descriptor->add(registry, entity));
        CHECK(!descriptor->add(registry, entity));
        CHECK(descriptor->has(registry, entity));
        auto* value = static_cast<FirstComponent*>(
            descriptor->getMutable(registry, entity));
        CHECK(value != nullptr);
        CHECK(value && value->value == 7);
        CHECK(descriptor->properties.size() == 1);
        auto* propertyValue = static_cast<int*>(
            descriptor->properties[0].getMutable(value));
        CHECK(propertyValue == &value->value);
        if (value) value->value = 11;
        CHECK(registry.getComponent<FirstComponent>(entity).value == 11);
        const auto snapshot = descriptor->capture(registry, entity);
        CHECK(snapshot != nullptr);
        CHECK(descriptor->remove(registry, entity));
        CHECK(!descriptor->remove(registry, entity));
        CHECK(!descriptor->has(registry, entity));
        CHECK(descriptor->restore(registry, entity, snapshot));
        CHECK(descriptor->has(registry, entity));
        CHECK(registry.getComponent<FirstComponent>(entity).value == 11);
        CHECK(!descriptor->restore(registry, entity, snapshot));
        CHECK(descriptor->remove(registry, entity));
    }

    void testPolicyValidation() {
        Iridium::EditorComponentRegistry registry;
        auto requiredAddable =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.required", "Required", 0, 100.0f,
                true, true, true);
        CHECK(registry.add(std::move(requiredAddable)).error ==
            Iridium::EditorComponentRegistryError::InvalidRequiredPolicy);

        auto noBinding = Iridium::editorComponentDescriptor<FirstComponent>(
            "test.component.unbound", "Unbound", 1, 100.0f,
            true, false, false);
        noBinding.has = nullptr;
        CHECK(registry.add(std::move(noBinding)).error ==
            Iridium::EditorComponentRegistryError::MissingComponentBinding);

        auto noRemove = Iridium::editorComponentDescriptor<FirstComponent>(
            "test.component.no_remove", "No Remove", 2, 100.0f,
            true, false, false);
        noRemove.remove = nullptr;
        CHECK(registry.add(std::move(noRemove)).error ==
            Iridium::EditorComponentRegistryError::MissingRemoveCallback);

        auto invalidProperties =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.properties", "Properties", 3, 100.0f,
                true, false, false);
        invalidProperties.properties.push_back(
            Iridium::editorPropertyDescriptor(
                "value", "Value", 0,
                Iridium::PropertyValueType::Int32,
                &FirstComponent::value, false, true));
        CHECK(registry.add(std::move(invalidProperties)).error ==
            Iridium::EditorComponentRegistryError::InvalidNullableProperty);

        auto badRange =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.range", "Range", 4, 100.0f,
                true, false, false);
        badRange.properties.push_back(Iridium::editorPropertyDescriptor(
            "value", "Value", 0, Iridium::PropertyValueType::Int32,
            &FirstComponent::value, false, false, int32_t{ 0 },
            10.0f, 1.0f));
        CHECK(registry.add(std::move(badRange)).error ==
            Iridium::EditorComponentRegistryError::InvalidPropertyRange);

        auto badDefault =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.default", "Default", 5, 100.0f,
                true, false, false);
        badDefault.properties.push_back(Iridium::editorPropertyDescriptor(
            "value", "Value", 0, Iridium::PropertyValueType::Int32,
            &FirstComponent::value, false, false, true));
        CHECK(registry.add(std::move(badDefault)).error ==
            Iridium::EditorComponentRegistryError::InvalidPropertyDefault);

        auto badSize =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.size", "Size", 6, 100.0f,
                true, false, false);
        badSize.properties.push_back(Iridium::editorPropertyDescriptor(
            "value", "Value", 0, Iridium::PropertyValueType::Int32,
            &FirstComponent::value));
        badSize.properties[0].valueSize = 1;
        CHECK(registry.add(std::move(badSize)).error ==
            Iridium::EditorComponentRegistryError::InvalidPropertyBindingSize);

        auto duplicateProperty =
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.duplicate_property", "Duplicate property",
                7, 100.0f, true, false, false);
        duplicateProperty.properties.push_back(
            Iridium::editorPropertyDescriptor(
                "value", "Value", 0,
                Iridium::PropertyValueType::Int32,
                &FirstComponent::value));
        duplicateProperty.properties.push_back(
            Iridium::editorPropertyDescriptor(
                "value", "Value again", 1,
                Iridium::PropertyValueType::Int32,
                &FirstComponent::value));
        CHECK(registry.add(std::move(duplicateProperty)).error ==
            Iridium::EditorComponentRegistryError::DuplicatePropertyId);
    }

    void testDrawerRegistry() {
        Iridium::EditorComponentRegistry components;
        CHECK(components.add(
            Iridium::editorComponentDescriptor<FirstComponent>(
                "test.component.first", "First", 0, 100.0f,
                true, false, true)));
        CHECK(components.add(
            Iridium::editorComponentDescriptor<SecondComponent>(
                "test.component.hidden", "Hidden", 1, 100.0f,
                false, false, false)));
        CHECK(components.freezeAndValidate());

        Iridium::EditorComponentDrawerRegistry drawers;
        CHECK(drawers.add({ {}, [](auto&) {} }).error ==
            Iridium::EditorComponentDrawerRegistryError::InvalidComponentId);
        CHECK(drawers.add({ id("test.component.first"), {} }).error ==
            Iridium::EditorComponentDrawerRegistryError::MissingDrawer);

        int drawCount = 0;
        CHECK(drawers.add({ id("test.component.first"),
            [&drawCount](Iridium::EditorComponentDrawContext& context) {
                CHECK(context.component != nullptr);
                CHECK(context.entity != NULL_ENTITY);
                ++drawCount;
            } }));
        CHECK(drawers.add({ id("test.component.first"), [](auto&) {} })
                .error ==
            Iridium::EditorComponentDrawerRegistryError::DuplicateComponentId);
        CHECK(drawers.descriptors().empty());
        CHECK(drawers.freezeAndValidate(components));
        CHECK(drawers.isFrozen());
        CHECK(drawers.descriptors().size() == 1);
        const auto* drawer = drawers.find(id("test.component.first"));
        CHECK(drawer != nullptr);
        if (drawer) {
            Registry registry;
            const Entity entity = registry.createEntity();
            FirstComponent component;
            Iridium::EditorComponentDrawContext context{
                .component = &component,
                .registry = registry,
                .entity = entity,
                .assetManager = nullptr
            };
            drawer->draw(context);
        }
        CHECK(drawCount == 1);
        CHECK(drawers.add({ id("test.component.hidden"), [](auto&) {} })
                .error ==
            Iridium::EditorComponentDrawerRegistryError::Frozen);
        CHECK(drawers.freezeAndValidate(components).error ==
            Iridium::EditorComponentDrawerRegistryError::Frozen);

        Iridium::EditorComponentDrawerRegistry unknown;
        CHECK(unknown.add({ id("test.component.unknown"), [](auto&) {} }));
        CHECK(unknown.freezeAndValidate(components).error ==
            Iridium::EditorComponentDrawerRegistryError::UnknownComponentId);

        Iridium::EditorComponentDrawerRegistry hidden;
        CHECK(hidden.add({ id("test.component.hidden"), [](auto&) {} }));
        CHECK(hidden.freezeAndValidate(components).error ==
            Iridium::EditorComponentDrawerRegistryError::HiddenComponent);

        Iridium::EditorComponentRegistry unfrozenComponents;
        Iridium::EditorComponentDrawerRegistry requiresFrozenComponents;
        CHECK(requiresFrozenComponents.freezeAndValidate(unfrozenComponents)
                .error ==
            Iridium::EditorComponentDrawerRegistryError::UnknownComponentId);
    }

    void testCoreDrawerRegistration() {
        auto components = Iridium::createCoreEditorComponentRegistry();
        CHECK(components);
        if (!components) return;

        const auto noop = [](Iridium::EditorComponentDrawContext&) {};
        Iridium::CoreEditorComponentDrawerCallbacks callbacks;
        callbacks.transform = noop;
        callbacks.relationship = noop;
        callbacks.mesh = noop;
        callbacks.light = noop;
        callbacks.sky = noop;
        callbacks.reflectionProbe = noop;
        callbacks.bakedLightingSet = noop;
        auto drawers = Iridium::createCoreEditorComponentDrawerRegistry(
            components.registry, std::move(callbacks));
        CHECK(drawers);
        if (!drawers) return;
        CHECK(drawers.registry.descriptors().size() == 7);
        CHECK(drawers.registry.find(id(Iridium::CoreTransformComponentId)) !=
            nullptr);
        CHECK(drawers.registry.find(id(Iridium::CoreRelationshipComponentId)) !=
            nullptr);
        CHECK(drawers.registry.find(id(Iridium::CoreMeshComponentId)) != nullptr);
        CHECK(drawers.registry.find(id(Iridium::CoreLightComponentId)) != nullptr);
        CHECK(drawers.registry.find(id(Iridium::CoreSkyComponentId)) != nullptr);
        CHECK(drawers.registry.find(id(
            Iridium::CoreReflectionProbeComponentId)) != nullptr);
        CHECK(drawers.registry.find(id(
            Iridium::CoreBakedLightingSetComponentId)) != nullptr);
        CHECK(drawers.registry.find(id(Iridium::CoreNameComponentId)) == nullptr);

        Iridium::CoreEditorComponentDrawerCallbacks missing;
        missing.relationship = noop;
        missing.mesh = noop;
        missing.light = noop;
        missing.sky = noop;
        missing.reflectionProbe = noop;
        missing.bakedLightingSet = noop;
        const auto rejected =
            Iridium::createCoreEditorComponentDrawerRegistry(
                components.registry, std::move(missing));
        CHECK(rejected.status.error ==
            Iridium::EditorComponentDrawerRegistryError::MissingDrawer);
    }

    void testCoreRegistration() {
        auto core = Iridium::createCoreEditorComponentRegistry();
        CHECK(core);
        if (!core) return;
        const auto descriptors = core.registry.descriptors();
        CHECK(descriptors.size() == 8);
        CHECK(descriptors[0].id == id(Iridium::CoreNameComponentId));
        CHECK(descriptors[1].id == id(Iridium::CoreTransformComponentId));
        CHECK(descriptors[2].id == id(Iridium::CoreRelationshipComponentId));
        CHECK(descriptors[3].id == id(Iridium::CoreMeshComponentId));
        CHECK(descriptors[4].id == id(Iridium::CoreLightComponentId));
        CHECK(descriptors[5].id == id(Iridium::CoreSkyComponentId));
        CHECK(descriptors[6].id == id(
            Iridium::CoreReflectionProbeComponentId));
        CHECK(descriptors[7].id == id(
            Iridium::CoreBakedLightingSetComponentId));
        CHECK(!descriptors[0].visible && descriptors[0].required);
        CHECK(descriptors[1].visible && descriptors[1].required);
        CHECK(descriptors[2].visible && descriptors[2].required);
        CHECK(descriptors[3].addable && !descriptors[3].required);
        CHECK(descriptors[4].addable && !descriptors[4].required);
        CHECK(descriptors[4].properties.size() == 11);
        CHECK(descriptors[4].properties[1].displayName ==
            "Color (linear Rec.709/D65)");
        CHECK(descriptors[4].properties[2].displayName ==
            "Illuminance (lux)");
        CHECK(descriptors[4].properties[3].displayName ==
            "Luminous intensity (cd)");
        CHECK(descriptors[4].properties[9].enumLabels ==
            std::vector<std::string>({ "Low", "Medium", "High", "Ultra" }));
        CHECK(descriptors[4].properties[10].id.value() == "priority");
        CHECK(descriptors[6].addable && !descriptors[6].required);
        CHECK(descriptors[6].properties.size() == 3);
        CHECK(descriptors[7].addable && !descriptors[7].required);
        CHECK(descriptors[7].properties.size() == 4);

        const Iridium::RuntimeComponentCallbacks callbacks{
            .resolveReferences = resolveReferences,
            .postLoadValidate = validateRuntime,
            .encodeCooked = encodeCooked,
            .decodeCooked = decodeCooked,
        };
        Iridium::CoreRuntimeComponentCallbacks runtimeCallbacks;
        runtimeCallbacks.name = callbacks;
        runtimeCallbacks.transform = callbacks;
        runtimeCallbacks.relationship = callbacks;
        runtimeCallbacks.mesh = callbacks;
        runtimeCallbacks.light = callbacks;
        runtimeCallbacks.sky = callbacks;
        runtimeCallbacks.reflectionProbe = callbacks;
        runtimeCallbacks.bakedLightingSet = callbacks;
        auto runtime = Iridium::createRuntimeSceneComponentRegistry(
            runtimeCallbacks);
        CHECK(runtime);
        if (!runtime) return;
        const auto runtimeDescriptors = runtime.registry.descriptors();
        CHECK(runtimeDescriptors.size() == descriptors.size());
        for (const auto& descriptor : descriptors) {
            const auto* runtimeDescriptor = runtime.registry.find(descriptor.id);
            CHECK(runtimeDescriptor != nullptr);
            if (!runtimeDescriptor) continue;
            CHECK(runtimeDescriptor->properties.size() >=
                descriptor.properties.size());
            for (const auto& property : descriptor.properties) {
                const auto found = std::ranges::find_if(
                    runtimeDescriptor->properties,
                    [&](const auto& candidate) {
                        return candidate.id == property.id;
                    });
                CHECK(found != runtimeDescriptor->properties.end());
                if (found != runtimeDescriptor->properties.end()) {
                    CHECK(found->valueType == property.valueType);
                    CHECK(found->nullable == property.nullable);
                }
            }
        }
    }

} // namespace

int main() {
    testValidationAndFreeze();
    testTypedBindings();
    testPolicyValidation();
    testDrawerRegistry();
    testCoreDrawerRegistration();
    testCoreRegistration();
    if (failures != 0) {
        std::cerr << failures << " editor component registry checks failed\n";
        return 1;
    }
    std::cout << "Editor component registry tests passed\n";
    return 0;
}
