#include "scene/Components.h"
#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneEnvelopeMigrator.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/runtime/CookedScene.h"

#include <array>
#include <cmath>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    constexpr std::string_view uuid1 =
        "019fb7d3-0510-7000-8000-000000000001";
    constexpr std::string_view uuid2 =
        "019fb7d3-0510-7000-8000-000000000002";
    constexpr std::string_view uuid3 =
        "019fb7d3-0510-7000-8000-000000000003";

    [[nodiscard]] std::string sceneWithEntities(std::string_view entities) {
        return R"json({"format":"iridium.scene","schemaVersion":1,"name":"Light v2","entities":)json" +
            std::string(entities) + "}";
    }

    template <typename StagedScene>
    [[nodiscard]] const LightComponent& light(
        const StagedScene& staged, std::string_view uuid) {
        const Entity entity = *staged.world->identities().resolve(
            *Iridium::SceneEntityUuid::parse(uuid));
        return staged.world->registry().getComponent<LightComponent>(entity);
    }

    [[nodiscard]] bool near(float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 1.0e-6f;
    }

    bool v1MigrationIsOrderedExplicitAndLossless() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string input = sceneWithEntities(R"json([
          {"uuid":")json" + std::string(uuid1) + R"json(","components":[
            {"id":"iridium.component.relationship","version":1,"data":{
              "parent":null,"siblingOrder":0}},
            {"id":"iridium.component.light","version":1,"data":{
              "type":0,"color":[0.2,0.4,0.8],"intensity":120000.0,
              "range":100.0,"radius":0.25,"innerCone":5.0,
              "outerCone":30.0,"castsShadows":false,"vendorLegacy":7}}]},
          {"uuid":")json" + std::string(uuid2) + R"json(","components":[
            {"id":"iridium.component.relationship","version":1,"data":{
              "parent":null,"siblingOrder":1}},
            {"id":"iridium.component.light","version":1,"data":{
              "type":1,"color":[1.0,0.5,0.25],"intensity":250.0}}]},
          {"uuid":")json" + std::string(uuid3) + R"json(","components":[
            {"id":"iridium.component.relationship","version":1,"data":{
              "parent":null,"siblingOrder":2}},
            {"id":"iridium.component.light","version":1,"data":{
              "type":2,"color":[0.1,0.2,0.3],"intensity":800.0,
              "innerCone":15.0,"outerCone":35.0}}]}
        ])json");

        const auto read = Iridium::readSourceSceneSchema1(
            input, registries.runtime, registries.source);
        if (!read) {
            for (const auto& diagnostic : read.diagnostics) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
        }
        CHECK(read);
        CHECK(read.diagnostics.size() == 6);
        for (size_t index = 0; index < read.diagnostics.size(); index += 2) {
            const auto& colorNotice = read.diagnostics[index];
            const auto& intensityNotice = read.diagnostics[index + 1];
            CHECK(colorNotice.severity == Iridium::SceneDiagnosticSeverity::Warning);
            CHECK(colorNotice.phase == Iridium::ScenePhase::ComponentMigration);
            CHECK(colorNotice.code == "light.v1_color_assumed_linear_rec709");
            CHECK(colorNotice.propertyPath.ends_with(
                "/data/colorLinearRec709"));
            CHECK(colorNotice.migrationFrom == 1);
            CHECK(colorNotice.migrationTo == 2);
            CHECK(intensityNotice.code == "light.v1_intensity_unit_adopted");
        }
        CHECK(read.diagnostics[1].propertyPath.ends_with(
            "/data/illuminanceLux"));
        CHECK(read.diagnostics[3].propertyPath.ends_with(
            "/data/luminousIntensityCandela"));
        CHECK(read.diagnostics[5].propertyPath.ends_with(
            "/data/luminousIntensityCandela"));
        for (const auto& entity : read.document->entities) {
            CHECK(entity.components.at(1).version == 2);
        }
        const auto& migrated = read.document->entities.at(0).components.at(1).data;
        CHECK(migrated.at("vendorLegacy") == 7);
        CHECK(!migrated.contains("color"));
        CHECK(!migrated.contains("intensity"));
        CHECK(migrated.at("illuminanceLux") == 120000.0f);
        CHECK(migrated.at("luminousIntensityCandela") == 1.0f);

        auto staged = Iridium::stageSourceScene(
            *read.document, registries.runtime, registries.source);
        CHECK(staged);
        const auto& directional = light(*staged.staging, uuid1);
        const auto& point = light(*staged.staging, uuid2);
        const auto& spot = light(*staged.staging, uuid3);
        CHECK(directional.type == LightType::Directional);
        CHECK(near(directional.colorLinearRec709.x, 0.2f));
        CHECK(near(directional.illuminanceLux, 120000.0f));
        CHECK(near(directional.luminousIntensityCandela, 1.0f));
        CHECK(!directional.castsShadows);
        CHECK(point.type == LightType::Point);
        CHECK(near(point.illuminanceLux, 1.0f));
        CHECK(near(point.luminousIntensityCandela, 250.0f));
        CHECK(spot.type == LightType::Spot);
        CHECK(near(spot.luminousIntensityCandela, 800.0f));

        const auto written = Iridium::writeSourceSceneCanonical(
            *read.document, registries.runtime, registries.source);
        CHECK(written);
        const auto canonical = Iridium::SourceJson::parse(*written.bytes);
        const auto& canonicalLight = canonical.at("entities").at(0)
            .at("components").at(1);
        CHECK(canonicalLight.at("version") == 2);
        CHECK(canonicalLight.at("data").contains("colorLinearRec709"));
        CHECK(canonicalLight.at("data").at("vendorLegacy") == 7);
        return true;
    }

    bool v2RoundTripsAndCooksDeterministically() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string input = sceneWithEntities(R"json([
          {"uuid":")json" + std::string(uuid1) + R"json(","components":[
            {"id":"iridium.component.light","version":2,"data":{
              "type":2,"colorLinearRec709":[0.25,0.5,1.0],
              "illuminanceLux":90000.0,"luminousIntensityCandela":1250.0,
              "rangeMeters":32.0,"sourceRadiusMeters":0.15,
              "innerConeDegrees":18.0,"outerConeDegrees":42.0,
              "castsShadows":true,"shadowQuality":3,"priority":17,
              "studioHint":"hero"}}]}
        ])json");
        const auto read = Iridium::readSourceSceneSchema1(
            input, registries.runtime, registries.source);
        CHECK(read);
        CHECK(read.diagnostics.empty());
        const auto written = Iridium::writeSourceSceneCanonical(
            *read.document, registries.runtime, registries.source);
        CHECK(written);
        CHECK(Iridium::SourceJson::parse(*written.bytes).at("entities").at(0)
            .at("components").at(0).at("data").at("studioHint") == "hero");
        const auto reread = Iridium::readSourceSceneSchema1(
            *written.bytes, registries.runtime, registries.source);
        CHECK(reread);
        CHECK(reread.document->entities.at(0).components.at(0).data.at(
            "studioHint") == "hero");

        auto cookDocument = *reread.document;
        cookDocument.entities.at(0).components.at(0).data.erase("studioHint");
        auto staged = Iridium::stageSourceScene(
            std::move(cookDocument), registries.runtime, registries.source);
        CHECK(staged);
        const auto& value = light(*staged.staging, uuid1);
        CHECK(value.type == LightType::Spot);
        CHECK(near(value.luminousIntensityCandela, 1250.0f));
        CHECK(value.shadowQuality == LightShadowQuality::Ultra);
        CHECK(value.priority == 17);

        const Iridium::CookedSceneCompileInput cookInput{
            .sceneAssetGuid = *Iridium::AssetGuid::parse(
                "01890f4c-0510-7000-8000-000000000001"),
            .sourceContentHash = std::string(64, 'a'),
            .canonicalContentHash = std::string(64, 'b'),
            .target = { .platform = "windows-x64", .profile = "debug",
                .qualityPolicy = "high" },
        };
        const auto first = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, cookInput);
        const auto second = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, cookInput);
        CHECK(first && second);
        const auto firstBlob = Iridium::serializeCookedArtifact(*first.artifact);
        const auto secondBlob = Iridium::serializeCookedArtifact(*second.artifact);
        CHECK(firstBlob.bytes == secondBlob.bytes);
        const auto loaded = Iridium::stageCookedScene(
            firstBlob.bytes, registries.runtime, {
                .expectedSceneAssetGuid = first.artifact->assetGuid,
                .expectedTarget = first.artifact->target,
                .expectedCookKey = first.artifact->cookKey,
                .expectedArtifactHash = firstBlob.artifactHash,
            });
        CHECK(loaded);
        const auto& loadedValue = light(*loaded.staging, uuid1);
        CHECK(loadedValue.type == LightType::Spot);
        CHECK(near(loadedValue.colorLinearRec709.z, 1.0f));
        CHECK(near(loadedValue.illuminanceLux, 90000.0f));
        CHECK(near(loadedValue.luminousIntensityCandela, 1250.0f));
        CHECK(near(loadedValue.rangeMeters, 32.0f));
        CHECK(near(loadedValue.sourceRadiusMeters, 0.15f));
        CHECK(near(loadedValue.innerConeDegrees, 18.0f));
        CHECK(near(loadedValue.outerConeDegrees, 42.0f));
        CHECK(loadedValue.castsShadows);
        CHECK(loadedValue.shadowQuality == LightShadowQuality::Ultra);
        CHECK(loadedValue.priority == 17);
        return true;
    }

    bool v0EnvelopeContinuesThroughLightV2() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        constexpr std::array<uint8_t, 16> sceneGuid{
            0x01, 0x9f, 0xb7, 0xd3, 0x05, 0x10, 0x70, 0x00,
            0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa,
        };
        const auto envelope = Iridium::migrateSourceSceneV0(R"json({
          "Scene":"Legacy light","Entities":[{
            "EntityID":4,"Name":"Legacy point",
            "LightComponent":{"type":1,"color":[0.3,0.6,0.9],
              "intensity":450.0,"range":22.0,"castsShadows":true}
          }]
        })json", sceneGuid);
        CHECK(envelope);
        const auto read = Iridium::readSourceSceneSchema1(
            envelope.value->dump(), registries.runtime, registries.source);
        CHECK(read);
        CHECK(std::ranges::any_of(read.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "light.v1_color_assumed_linear_rec709";
        }));
        CHECK(std::ranges::any_of(read.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "light.v1_intensity_unit_adopted";
        }));
        const auto component = std::ranges::find_if(
            read.document->entities.at(0).components,
            [](const auto& value) {
                return value.id.value() == "iridium.component.light";
            });
        CHECK(component != read.document->entities.at(0).components.end());
        CHECK(component->version == 2);
        auto staged = Iridium::stageSourceScene(
            *read.document, registries.runtime, registries.source);
        CHECK(staged);
        const auto* lights = staged.staging->world->registry()
            .findPool<LightComponent>();
        CHECK(lights && lights->components.size() == 1);
        CHECK(lights->components[0].type == LightType::Point);
        CHECK(near(lights->components[0].luminousIntensityCandela, 450.0f));
        CHECK(near(lights->components[0].rangeMeters, 22.0f));
        return true;
    }

    bool invalidValuesAndLegacyAreaAreHandledExplicitly() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const auto invalid = [&](std::string_view data) {
            const std::string input = sceneWithEntities("[{\"uuid\":\"" +
                std::string(uuid1) +
                "\",\"components\":[{\"id\":\"iridium.component.light\"," 
                "\"version\":2,\"data\":" + std::string(data) + "}]}]");
            return Iridium::readSourceSceneSchema1(
                input, registries.runtime, registries.source);
        };
        CHECK(!invalid(R"json({"colorLinearRec709":[-0.1,1,1]})json"));
        CHECK(!invalid(R"json({"luminousIntensityCandela":-1})json"));
        CHECK(!invalid(R"json({"innerConeDegrees":50,"outerConeDegrees":40})json"));
        CHECK(!invalid(R"json({"shadowQuality":4})json"));

        const std::string areaSource = sceneWithEntities(R"json([
          {"uuid":")json" + std::string(uuid1) + R"json(","components":[
            {"id":"iridium.component.light","version":1,"data":{
              "type":3,"color":[1,1,1],"intensity":100}}]}
        ])json");
        const auto area = Iridium::readSourceSceneSchema1(
            areaSource, registries.runtime, registries.source);
        CHECK(area);
        CHECK(std::ranges::any_of(area.diagnostics, [](const auto& diagnostic) {
            return diagnostic.code == "light.area_unsupported" &&
                diagnostic.severity == Iridium::SceneDiagnosticSeverity::Warning;
        }));
        auto staged = Iridium::stageSourceScene(
            *area.document, registries.runtime, registries.source);
        CHECK(staged);
        CHECK(light(*staged.staging, uuid1).type == LightType::Area);
        const auto compiled = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, {
                .sceneAssetGuid = *Iridium::AssetGuid::parse(
                    "01890f4c-0510-7000-8000-000000000002"),
                .sourceContentHash = std::string(64, 'a'),
                .canonicalContentHash = std::string(64, 'b'),
                .target = { .platform = "windows-x64", .profile = "debug",
                    .qualityPolicy = "high" },
            });
        CHECK(!compiled);
        CHECK(std::ranges::any_of(compiled.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code == "scene.cook.component_encode";
            }));
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "v1 migration", v1MigrationIsOrderedExplicitAndLossless },
        std::pair{ "v2 round trip and cook", v2RoundTripsAndCooksDeterministically },
        std::pair{ "v0 envelope to v2", v0EnvelopeContinuesThroughLightV2 },
        std::pair{ "invalid values and Area", invalidValuesAndLegacyAreaAreHandledExplicitly },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
