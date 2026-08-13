#include "scene/authoring/CoreComponentCodecs.h"
#include "scene/runtime/CoreComponentRegistry.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    bool resolve(Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) { return true; }
    bool validateRuntime(const Registry&, Entity) { return true; }
    bool encode(const Registry&, Entity, Iridium::CookedComponentWriter&) { return true; }
    bool decode(Registry&, Entity, Iridium::CookedComponentReader&) { return true; }
    bool serialize(const Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SourceJson&, std::string&) { return true; }
    bool deserialize(Registry&, Entity, const Iridium::SourceJson&, std::string&) { return true; }
    bool validateSource(const Iridium::SourceJson&, std::string&) { return true; }

    Iridium::CoreRuntimeComponentCallbacks runtimeCallbacks() {
        const Iridium::RuntimeComponentCallbacks value{
            resolve, validateRuntime, encode, decode,
        };
        return { value, value, value, value, value, value, value, value };
    }

    Iridium::CoreSourceComponentCallbacks sourceCallbacks() {
        const Iridium::SourceComponentCallbacks value{
            serialize, deserialize, validateSource,
        };
        auto callbacks = Iridium::CoreSourceComponentCallbacks{
            value, value, value, value, value, value, value, value
        };
        callbacks.light.currentSourceVersion = 2;
        return callbacks;
    }

    bool exactCoreManifestIsFrozen() {
        const auto runtime = Iridium::createRuntimeSceneComponentRegistry(
            runtimeCallbacks());
        CHECK(runtime);
        const auto source = Iridium::createSourceComponentSerializerRegistry(
            runtime.registry, sourceCallbacks());
        CHECK(source);

        constexpr std::array<std::string_view, 8> ids{
            "iridium.component.baked_lighting_set", "iridium.component.light",
            "iridium.component.mesh",
            "iridium.component.name", "iridium.component.reflection_probe",
            "iridium.component.relationship", "iridium.component.sky",
            "iridium.component.transform",
        };
        constexpr std::array<std::string_view, 8> sections{
            "BLS1", "LGT1", "MSH1", "NAM1", "RFP1", "REL1", "SKY1",
            "TRN1",
        };
        CHECK(runtime.registry.descriptors().size() == ids.size());
        for (size_t index = 0; index < ids.size(); ++index) {
            CHECK(runtime.registry.descriptors()[index].id.value() == ids[index]);
            CHECK(runtime.registry.descriptors()[index].cookedSectionId.toString() ==
                sections[index]);
            CHECK(runtime.registry.descriptors()[index].currentCookedVersion ==
                (ids[index] == "iridium.component.light" ? 2u : 1u));
        }
        CHECK(source.registry.codecs().size() == 8);
        CHECK(source.registry.codecs()[0].componentId.value() ==
            "iridium.component.name");
        CHECK(source.registry.codecs()[4].componentId.value() ==
            "iridium.component.light");
        CHECK(source.registry.codecs()[4].currentSourceVersion == 2);
        CHECK(source.registry.sourceName(
            *Iridium::ComponentTypeId::parse("iridium.component.mesh"),
            *Iridium::PropertyId::parse("material_overrides")) ==
            "materialOverrides");
        return true;
    }

    bool exactPropertiesAreStable() {
        const auto runtime = Iridium::createRuntimeSceneComponentRegistry(
            runtimeCallbacks());
        const auto id = [](std::string_view value) {
            return *Iridium::ComponentTypeId::parse(value);
        };
        const auto* transform = runtime.registry.find(id(
            "iridium.component.transform"));
        CHECK(transform && transform->properties.size() == 3);
        CHECK(transform->properties[0].id.value() == "position");
        CHECK(transform->properties[1].id.value() == "rotation");
        CHECK(transform->properties[2].id.value() == "scale");
        CHECK((std::get<std::array<float, 3>>(
            transform->properties[2].defaultValue)[0] == 1.0f));
        const auto* mesh = runtime.registry.find(id("iridium.component.mesh"));
        CHECK(mesh && mesh->properties.size() == 3);
        CHECK(mesh->properties[1].id.value() == "model");
        CHECK(mesh->properties[1].nullable);
        CHECK(mesh->properties[1].referenceKind ==
            Iridium::PropertyReferenceKind::Asset);
        CHECK(mesh->properties[2].collectionOrdering ==
            Iridium::CollectionOrdering::SourceSubassetGuid);
        const auto* relationship = runtime.registry.find(id(
            "iridium.component.relationship"));
        CHECK(relationship->properties[0].nullable);
        CHECK(relationship->properties[0].referenceKind ==
            Iridium::PropertyReferenceKind::Entity);
        const auto* light = runtime.registry.find(id("iridium.component.light"));
        CHECK(light && light->properties.size() == 11);
        CHECK(light->properties[1].id.value() == "color_linear_rec709");
        CHECK(light->properties[2].id.value() == "illuminance_lux");
        CHECK(light->properties[3].id.value() ==
            "luminous_intensity_candela");
        CHECK(light->properties[9].id.value() == "shadow_quality");
        CHECK(light->properties.back().id.value() == "priority");
        const auto* sky = runtime.registry.find(id("iridium.component.sky"));
        CHECK(sky && sky->properties.size() == 19);
        CHECK(sky->properties[1].id.value() == "mode");
        CHECK(sky->properties[6].id.value() == "hdri_environment");
        CHECK(sky->properties[6].referenceKind ==
            Iridium::PropertyReferenceKind::Asset);
        CHECK(sky->properties.back().id.value() == "priority");
        const auto* probe = runtime.registry.find(id(
            "iridium.component.reflection_probe"));
        CHECK(probe && probe->properties.size() == 14);
        CHECK(probe->properties[3].id.value() == "box_extents_meters");
        CHECK(probe->properties[9].id.value() == "capture_resolution");
        CHECK(probe->properties.back().id.value() == "environment");
        CHECK(probe->properties.back().referenceKind ==
            Iridium::PropertyReferenceKind::Asset);
        const auto* baked = runtime.registry.find(id(
            "iridium.component.baked_lighting_set"));
        CHECK(baked && baked->properties.size() == 7);
        CHECK(baked->properties[1].id.value() == "lighting_asset");
        CHECK(baked->properties[1].referenceKind ==
            Iridium::PropertyReferenceKind::Asset);
        return true;
    }

    bool missingInjectedCallbackFailsComposition() {
        auto callbacks = runtimeCallbacks();
        callbacks.mesh.encodeCooked = nullptr;
        const auto runtime = Iridium::createRuntimeSceneComponentRegistry(callbacks);
        CHECK(!runtime);
        CHECK(runtime.status.error == Iridium::ComponentRegistryError::MissingCallback);
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "exact core manifest", exactCoreManifestIsFrozen },
        std::pair{ "exact core properties", exactPropertiesAreStable },
        std::pair{ "callback injection validation", missingInjectedCallbackFailsComposition },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
