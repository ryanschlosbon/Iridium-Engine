#include "scene/runtime/RuntimeComponentRegistry.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    bool resolveReferences(
        Registry&, Entity, const Iridium::SceneIdentityMap&,
        Iridium::SceneReferenceState&) {
        return true;
    }

    bool validateComponent(const Registry&, Entity) {
        return true;
    }

    bool encodeComponent(const Registry&, Entity,
        Iridium::CookedComponentWriter&) {
        return true;
    }

    bool decodeComponent(Registry&, Entity,
        Iridium::CookedComponentReader&) {
        return true;
    }

    Iridium::RuntimeComponentDescriptor descriptor(
        std::string_view id,
        std::string_view section,
        uint32_t version = 1) {
        return {
            .id = *Iridium::ComponentTypeId::parse(id),
            .cookedSectionId = *Iridium::CookedSectionId::parse(section),
            .currentCookedVersion = version,
            .properties = {},
            .resolveReferences = resolveReferences,
            .postLoadValidate = validateComponent,
            .encodeCooked = encodeComponent,
            .decodeCooked = decodeComponent,
        };
    }

    Iridium::PropertyDescriptor property(
        std::string_view id,
        uint32_t order,
        Iridium::PropertyValueType type =
            Iridium::PropertyValueType::String,
        Iridium::PropertyReferenceKind reference =
            Iridium::PropertyReferenceKind::None) {
        return {
            .id = *Iridium::PropertyId::parse(id),
            .valueType = type,
            .referenceKind = reference,
            .serializationOrder = order,
        };
    }

    bool stableIdentityGrammarIsStrict() {
        CHECK(Iridium::ComponentTypeId::isValid("iridium.component.name"));
        CHECK(Iridium::ComponentTypeId::isValid("studio.vendor.wind_2d"));
        CHECK(!Iridium::ComponentTypeId::isValid("name"));
        CHECK(!Iridium::ComponentTypeId::isValid("Iridium.component.name"));
        CHECK(!Iridium::ComponentTypeId::isValid("iridium_core.component.name"));
        CHECK(!Iridium::ComponentTypeId::isValid("iridium..name"));
        CHECK(!Iridium::ComponentTypeId::isValid("iridium.-name"));
        CHECK(!Iridium::ComponentTypeId::isValid("iridium.component.name."));
        CHECK(!Iridium::ComponentTypeId::isValid("iridium.component.nam\xc3\xa9"));

        CHECK(Iridium::PropertyId::isValid("material_overrides"));
        CHECK(Iridium::PropertyId::isValid("f0"));
        CHECK(!Iridium::PropertyId::isValid("materialOverrides"));
        CHECK(!Iridium::PropertyId::isValid("_position"));
        CHECK(!Iridium::PropertyId::isValid("position-x"));

        const auto section = Iridium::CookedSectionId::parse("TRN1");
        CHECK(section);
        CHECK(section->toString() == "TRN1");
        CHECK(!Iridium::CookedSectionId::parse("trn1"));
        CHECK(!Iridium::CookedSectionId::parse("TRN"));
        CHECK(!Iridium::CookedSectionId::parse("TR_1"));
        return true;
    }

    bool registryFreezesInCanonicalIdOrder() {
        Iridium::RuntimeComponentRegistry registry;
        CHECK(registry.add(descriptor("iridium.component.transform", "TRN1")));
        CHECK(registry.add(descriptor("iridium.component.name", "NAM1")));
        CHECK(registry.descriptors().empty());
        CHECK(!registry.find(*Iridium::ComponentTypeId::parse(
            "iridium.component.name")));
        CHECK(registry.freezeAndValidate());
        CHECK(registry.isFrozen());
        CHECK(registry.descriptors().size() == 2);
        CHECK(registry.descriptors()[0].id.value() == "iridium.component.name");
        CHECK(registry.descriptors()[1].id.value() ==
            "iridium.component.transform");
        CHECK(registry.find(*Iridium::ComponentTypeId::parse(
            "iridium.component.transform")) == &registry.descriptors()[1]);
        CHECK(!registry.find(*Iridium::ComponentTypeId::parse(
            "studio.vendor.missing")));
        CHECK(registry.freezeAndValidate());
        CHECK(registry.add(descriptor("iridium.component.mesh", "MSH1")).error ==
            Iridium::ComponentRegistryError::Frozen);
        return true;
    }

    bool duplicateIdentitiesAreRejected() {
        Iridium::RuntimeComponentRegistry registry;
        CHECK(registry.add(descriptor("iridium.component.name", "NAM1")));
        CHECK(registry.add(descriptor("iridium.component.name", "NAM2")).error ==
            Iridium::ComponentRegistryError::DuplicateComponentId);
        CHECK(registry.add(descriptor("iridium.component.mesh", "NAM1")).error ==
            Iridium::ComponentRegistryError::DuplicateCookedSectionId);
        CHECK(registry.descriptors().empty());
        return true;
    }

    bool descriptorValidationRejectsAmbiguity() {
        {
            Iridium::RuntimeComponentRegistry registry;
            auto value = descriptor("iridium.component.name", "NAM1");
            value.encodeCooked = nullptr;
            CHECK(registry.add(std::move(value)).error ==
                Iridium::ComponentRegistryError::MissingCallback);
        }
        {
            Iridium::RuntimeComponentRegistry registry;
            auto value = descriptor("iridium.component.name", "NAM1", 0);
            CHECK(registry.add(std::move(value)).error ==
                Iridium::ComponentRegistryError::InvalidCookedVersion);
        }
        {
            Iridium::RuntimeComponentRegistry registry;
            auto value = descriptor("iridium.component.transform", "TRN1");
            value.properties = {
                property("position", 0), property("position", 1),
            };
            CHECK(registry.add(std::move(value)).error ==
                Iridium::ComponentRegistryError::DuplicatePropertyId);
        }
        {
            Iridium::RuntimeComponentRegistry registry;
            auto value = descriptor("iridium.component.transform", "TRN1");
            value.properties = {
                property("position", 0), property("rotation", 0),
            };
            CHECK(registry.add(std::move(value)).error ==
                Iridium::ComponentRegistryError::DuplicatePropertyOrder);
        }
        {
            Iridium::RuntimeComponentRegistry registry;
            auto value = descriptor("iridium.component.relationship", "REL1");
            value.properties = { property("parent", 0,
                Iridium::PropertyValueType::EntityReference,
                Iridium::PropertyReferenceKind::None) };
            CHECK(registry.add(std::move(value)).error ==
                Iridium::ComponentRegistryError::InvalidPropertyReference);
        }
        return true;
    }

    bool propertyEnumerationIsCanonical() {
        auto value = descriptor("iridium.component.transform", "TRN1");
        value.properties = {
            property("scale", 2),
            property("position", 0),
            property("rotation", 1),
        };
        Iridium::RuntimeComponentRegistry registry;
        CHECK(registry.add(std::move(value)));
        CHECK(registry.freezeAndValidate());
        const auto& properties = registry.descriptors().front().properties;
        CHECK(properties.size() == 3);
        CHECK(properties[0].id.value() == "position");
        CHECK(properties[1].id.value() == "rotation");
        CHECK(properties[2].id.value() == "scale");
        return true;
    }

    bool cookedManifestIsCanonicalAndContractSensitive() {
        auto firstDescriptor = descriptor(
            "iridium.component.transform", "TRN1");
        firstDescriptor.properties = {
            property("scale", 2), property("position", 0),
            property("rotation", 1),
        };
        auto secondDescriptor = descriptor(
            "iridium.component.transform", "TRN1");
        secondDescriptor.properties = {
            property("rotation", 1), property("scale", 2),
            property("position", 0),
        };
        Iridium::RuntimeComponentRegistry first;
        Iridium::RuntimeComponentRegistry second;
        CHECK(first.add(std::move(firstDescriptor)));
        CHECK(second.add(std::move(secondDescriptor)));
        CHECK(first.freezeAndValidate());
        CHECK(second.freezeAndValidate());
        const std::string hash = Iridium::runtimeComponentManifestHash(first);
        CHECK(hash.size() == 64);
        CHECK(hash == Iridium::runtimeComponentManifestHash(second));

        auto changedDescriptor = descriptor(
            "iridium.component.transform", "TRN1", 2);
        changedDescriptor.properties = {
            property("position", 0), property("rotation", 1),
            property("scale", 2),
        };
        Iridium::RuntimeComponentRegistry changed;
        CHECK(changed.add(std::move(changedDescriptor)));
        CHECK(changed.freezeAndValidate());
        CHECK(hash != Iridium::runtimeComponentManifestHash(changed));
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    const std::array tests{
        TestCase{ "stable identity grammar", stableIdentityGrammarIsStrict },
        TestCase{ "canonical frozen registry", registryFreezesInCanonicalIdOrder },
        TestCase{ "duplicate identity rejection", duplicateIdentitiesAreRejected },
        TestCase{ "descriptor validation", descriptorValidationRejectsAmbiguity },
        TestCase{ "canonical property enumeration", propertyEnumerationIsCanonical },
        TestCase{ "cooked manifest contract", cookedManifestIsCanonicalAndContractSensitive },
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        if (!test.run()) {
            std::cerr << "[FAIL] " << test.name << '\n';
            return 1;
        }
        ++passed;
        std::cout << "[PASS] " << test.name << '\n';
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return 0;
}
