#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/SourceSceneCapture.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/Components.h"
#include "scene/runtime/CookedScene.h"

#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    bool completeCoreRoundTripUsesStableReferences() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Core",
          "vendorTop":{"preserve":true},"entities":[
            {"uuid":"019fb7d3-0300-7000-8000-000000000001",
             "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Root","vendorName":5}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
              {"id":"iridium.component.sky","version":1,"data":{
               "enabled":true,"mode":1,
               "hdriEnvironment":{"assetGuid":"01890f4c-0000-7000-8000-000000000004"},
               "hdriLightingIntensity":1.25,"hdriBackgroundIntensity":0.75,
               "hdriRotationDegrees":47.5,"hdriVisibleToCamera":true,
               "hdriAffectsLighting":true,"priority":7}},
              {"id":"iridium.component.reflection_probe","version":1,"data":{
               "enabled":true,"shape":1,"boxExtentsMeters":[8,4,6],
               "blendDistanceMeters":1.5,"intensity":1.2,"priority":3,
               "updateMode":1,"parallaxMode":1,"captureResolution":1024,
               "captureNearMeters":0.25,"captureFarMeters":250,
               "captureSky":true,
               "environment":{"assetGuid":"01890f4c-0000-7000-8000-000000000005"}}},
              {"id":"iridium.component.baked_lighting_set","version":1,"data":{
               "enabled":true,
               "lightingAsset":{"assetGuid":"01890f4c-0000-7000-8000-000000000006"},
               "diffuseIntensity":1.5,"specularIntensity":0.75,
               "applyLightmaps":true,"applyProbeVolumes":true,
               "applyVisibility":false}},
              {"id":"iridium.component.mesh","version":1,"data":{"enabled":true,
                "model":{"assetGuid":"01890f4c-0000-7000-8000-000000000001"},
                "materialOverrides":[{"source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000002"},
                "replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000003"}}]}}
             ]},
            {"uuid":"019fb7d3-0300-7000-8000-000000000002",
             "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Child"}},
              {"id":"iridium.component.transform","version":1,"data":{"position":[1,2,3]}},
              {"id":"iridium.component.relationship","version":1,"data":{
                "parent":"019fb7d3-0300-7000-8000-000000000001","siblingOrder":0}},
              {"id":"iridium.component.light","version":1,"data":{"type":2,"innerCone":15,"outerCone":40}}
             ]}
          ]})json";

        const auto read = Iridium::readSourceSceneSchema1(input,
            registries.runtime, registries.source);
        CHECK(read);
        auto staged = Iridium::stageSourceScene(*read.document,
            registries.runtime, registries.source);
        CHECK(staged);
        const auto rootUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0300-7000-8000-000000000001");
        const auto childUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0300-7000-8000-000000000002");
        const Entity root = *staged.staging->world->identities().resolve(rootUuid);
        const Entity child = *staged.staging->world->identities().resolve(childUuid);
        const auto& childRelationship = staged.staging->world->registry()
            .getComponent<RelationshipComponent>(child);
        CHECK(childRelationship.parent == root);
        CHECK(childRelationship.depth == 1);
        CHECK(staged.staging->world->registry()
            .getComponent<RelationshipComponent>(root).children ==
                std::vector<Entity>{ child });
        CHECK(staged.staging->world->registry().getComponent<MeshComponent>(root)
            .requestedAssetGuid.toString() ==
                "01890f4c-0000-7000-8000-000000000001");
        const auto& sourceSky = staged.staging->world->registry()
            .getComponent<Iridium::SkyComponent>(root);
        CHECK(sourceSky.mode == Iridium::SkyMode::Hdri);
        CHECK(sourceSky.requestedEnvironmentAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000004");
        CHECK(sourceSky.hdri.lightingIntensity == 1.25f);
        CHECK(sourceSky.hdri.backgroundIntensity == 0.75f);
        CHECK(sourceSky.hdri.rotationDegrees == 47.5f);
        CHECK(sourceSky.priority == 7);
        const auto& sourceProbe = staged.staging->world->registry()
            .getComponent<Iridium::ReflectionProbeComponent>(root);
        CHECK(sourceProbe.shape == Iridium::ReflectionProbeShape::Box);
        CHECK(sourceProbe.boxExtentsMeters == glm::vec3(8.0f, 4.0f, 6.0f));
        CHECK(sourceProbe.captureResolution == 1024);
        CHECK(sourceProbe.requestedEnvironmentAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000005");
        const auto& sourceBaked = staged.staging->world->registry()
            .getComponent<Iridium::BakedLightingSetComponent>(root);
        CHECK(sourceBaked.requestedLightingAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000006");
        CHECK(sourceBaked.diffuseIntensity == 1.5f);
        CHECK(sourceBaked.specularIntensity == 0.75f);
        CHECK(!sourceBaked.applyVisibility);
        CHECK(staged.staging->world->references().records().size() == 7);

        staged.staging->world->registry().getComponent<NameComponent>(root).name =
            "Root Edited";
        const auto captured = Iridium::captureSourceScene(
            *staged.staging->world, *read.document,
            registries.runtime, registries.source);
        CHECK(captured);
        const auto written = Iridium::writeSourceSceneCanonical(
            *captured.document, registries.runtime, registries.source);
        if (!written) {
            for (const auto& diagnostic : written.diagnostics) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
        }
        CHECK(written);
        const auto reread = Iridium::readSourceSceneSchema1(*written.bytes,
            registries.runtime, registries.source);
        CHECK(reread);
        CHECK(reread.document->unknownFields.at("vendorTop").at("preserve"));
        CHECK(reread.document->entities[0].components[0].data.at("value") ==
            "Root Edited");
        CHECK(reread.document->entities[0].components[0].data.at("vendorName") == 5);
        return true;
    }

    bool removedPathFieldIsRejected() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        const std::string input = R"json({"format":"iridium.scene",
          "schemaVersion":1,"name":"Rejected","entities":[{
          "uuid":"019fb7d3-0300-7000-8000-000000000001","components":[{
          "id":"iridium.component.mesh","version":1,"data":{
          "meshPath":"assets/model.gltf"}}]}]})json";
        CHECK(!Iridium::readSourceSceneSchema1(input,
            registries.runtime, registries.source));
        return true;
    }

    bool invalidReflectionProbeDomainIsRejected() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        const std::string input = R"json({"format":"iridium.scene",
          "schemaVersion":1,"name":"Rejected Probe","entities":[{
          "uuid":"019fb7d3-0300-7000-8000-000000000001","components":[{
          "id":"iridium.component.reflection_probe","version":1,"data":{
          "shape":1,"boxExtentsMeters":[5,0,5],"captureResolution":768,
          "captureNearMeters":1,"captureFarMeters":0.5}}]}]})json";
        CHECK(!Iridium::readSourceSceneSchema1(input,
            registries.runtime, registries.source));
        return true;
    }

    bool invalidBakedLightingDomainIsRejected() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        const std::string input = R"json({"format":"iridium.scene",
          "schemaVersion":1,"name":"Rejected Baked Lighting","entities":[{
          "uuid":"019fb7d3-0300-7000-8000-000000000001","components":[{
          "id":"iridium.component.baked_lighting_set","version":1,"data":{
          "diffuseIntensity":-1.0}}]}]})json";
        CHECK(!Iridium::readSourceSceneSchema1(input,
            registries.runtime, registries.source));
        return true;
    }

    bool coreComponentsCookAndLoadSemantically() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string input = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Cooked Core",
          "entities":[
            {"uuid":"019fb7d3-0300-7000-8000-000000000001",
             "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Root"}},
              {"id":"iridium.component.transform","version":1,"data":{}},
              {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":0}},
              {"id":"iridium.component.sky","version":1,"data":{
               "enabled":true,"mode":1,
               "hdriEnvironment":{"assetGuid":"01890f4c-0000-7000-8000-000000000004"},
               "hdriLightingIntensity":2.0,"hdriBackgroundIntensity":0.5,
               "hdriRotationDegrees":135.0,"hdriVisibleToCamera":false,
               "hdriAffectsLighting":true,"priority":12}},
              {"id":"iridium.component.reflection_probe","version":1,"data":{
               "enabled":true,"shape":0,"sphereRadiusMeters":12.0,
               "blendDistanceMeters":2.5,"intensity":1.5,"priority":8,
               "updateMode":0,"parallaxMode":0,"captureResolution":2048,
               "captureNearMeters":0.5,"captureFarMeters":500.0,
               "captureSky":false,
               "environment":{"assetGuid":"01890f4c-0000-7000-8000-000000000005"}}},
              {"id":"iridium.component.baked_lighting_set","version":1,"data":{
               "enabled":true,
               "lightingAsset":{"assetGuid":"01890f4c-0000-7000-8000-000000000006"},
               "diffuseIntensity":2.0,"specularIntensity":0.25,
               "applyLightmaps":true,"applyProbeVolumes":false,
               "applyVisibility":true}},
              {"id":"iridium.component.mesh","version":1,"data":{"enabled":true,
               "model":{"assetGuid":"01890f4c-0000-7000-8000-000000000001"},
               "materialOverrides":[{"source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000002"},
               "replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000003"}}]}}
             ]},
            {"uuid":"019fb7d3-0300-7000-8000-000000000002",
             "components":[
              {"id":"iridium.component.name","version":1,"data":{"value":"Child"}},
              {"id":"iridium.component.transform","version":1,"data":{"position":[1,2,3]}},
              {"id":"iridium.component.relationship","version":1,"data":{
               "parent":"019fb7d3-0300-7000-8000-000000000001","siblingOrder":4}},
              {"id":"iridium.component.light","version":1,"data":{"type":2,
               "color":[0.5,0.75,1.0],"intensity":8,"range":20,"radius":1,
               "innerCone":15,"outerCone":40,"castsShadows":false}}
             ]}
          ]})json";
        const auto source = Iridium::readSourceSceneSchema1(input,
            registries.runtime, registries.source);
        CHECK(source);
        auto staged = Iridium::stageSourceScene(*source.document,
            registries.runtime, registries.source);
        CHECK(staged);
        const auto dependency = [](std::string_view guid) {
            return Iridium::AssetDependency{
                .type = Iridium::AssetDependencyType::Asset,
                .assetGuid = *Iridium::AssetGuid::parse(guid),
                .artifactHash = std::string(64, 'd'),
            };
        };
        const auto sceneGuid = *Iridium::AssetGuid::parse(
            "01890f4c-0000-7000-8000-000000000010");
        Iridium::CookedSceneCompileInput cook{
            .sceneAssetGuid = sceneGuid,
            .sourceContentHash = std::string(64, 'a'),
            .canonicalContentHash = std::string(64, 'b'),
            .target = { .platform = "windows-x64", .profile = "release",
                .qualityPolicy = "high" },
            .dependencies = {
                dependency("01890f4c-0000-7000-8000-000000000001"),
                dependency("01890f4c-0000-7000-8000-000000000002"),
                dependency("01890f4c-0000-7000-8000-000000000003"),
                dependency("01890f4c-0000-7000-8000-000000000004"),
                dependency("01890f4c-0000-7000-8000-000000000005"),
                dependency("01890f4c-0000-7000-8000-000000000006"),
            },
        };
        const auto compiled = Iridium::compileCookedScene(*staged.staging,
            registries.runtime, registries.source, std::move(cook));
        CHECK(compiled);
        const auto blob = Iridium::serializeCookedArtifact(*compiled.artifact);
        auto loaded = Iridium::stageCookedScene(blob.bytes, registries.runtime, {
            .expectedSceneAssetGuid = sceneGuid,
            .assetAvailability = [](const Iridium::AssetDependency&) {
                return Iridium::CookedAssetAvailability::Available;
            },
        });
        if (!loaded) {
            for (const auto& diagnostic : loaded.diagnostics) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
        }
        CHECK(loaded);
        const auto rootUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0300-7000-8000-000000000001");
        const auto childUuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0300-7000-8000-000000000002");
        const Entity root = *loaded.staging->world->identities().resolve(rootUuid);
        const Entity child = *loaded.staging->world->identities().resolve(childUuid);
        CHECK(loaded.staging->world->registry().getComponent<NameComponent>(root)
            .name == "Root");
        CHECK(loaded.staging->world->registry()
            .getComponent<RelationshipComponent>(child).parent == root);
        CHECK(loaded.staging->world->registry()
            .getComponent<RelationshipComponent>(child).depth == 1);
        CHECK(loaded.staging->world->registry()
            .getComponent<RelationshipComponent>(child).siblingOrder == 4);
        CHECK(loaded.staging->world->registry().getComponent<MeshComponent>(root)
            .materialOverrides.size() == 1);
        const auto& cookedSky = loaded.staging->world->registry()
            .getComponent<Iridium::SkyComponent>(root);
        CHECK(cookedSky.mode == Iridium::SkyMode::Hdri);
        CHECK(cookedSky.hdri.environmentAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000004");
        CHECK(cookedSky.requestedEnvironmentAssetGuid ==
            cookedSky.hdri.environmentAssetGuid);
        CHECK(cookedSky.hdri.lightingIntensity == 2.0f);
        CHECK(cookedSky.hdri.backgroundIntensity == 0.5f);
        CHECK(cookedSky.hdri.rotationDegrees == 135.0f);
        CHECK(!cookedSky.hdri.visibleToCamera);
        CHECK(cookedSky.hdri.affectsLighting);
        CHECK(cookedSky.priority == 12);
        const auto& cookedProbe = loaded.staging->world->registry()
            .getComponent<Iridium::ReflectionProbeComponent>(root);
        CHECK(cookedProbe.shape == Iridium::ReflectionProbeShape::Sphere);
        CHECK(cookedProbe.sphereRadiusMeters == 12.0f);
        CHECK(cookedProbe.blendDistanceMeters == 2.5f);
        CHECK(cookedProbe.intensity == 1.5f);
        CHECK(cookedProbe.priority == 8);
        CHECK(cookedProbe.updateMode ==
            Iridium::ReflectionProbeUpdateMode::Baked);
        CHECK(cookedProbe.parallaxMode ==
            Iridium::ReflectionProbeParallaxMode::None);
        CHECK(cookedProbe.captureResolution == 2048);
        CHECK(cookedProbe.captureNearMeters == 0.5f);
        CHECK(cookedProbe.captureFarMeters == 500.0f);
        CHECK(!cookedProbe.captureSky);
        CHECK(cookedProbe.environmentAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000005");
        const auto& cookedBaked = loaded.staging->world->registry()
            .getComponent<Iridium::BakedLightingSetComponent>(root);
        CHECK(cookedBaked.lightingAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000006");
        CHECK(cookedBaked.requestedLightingAssetGuid ==
            cookedBaked.lightingAssetGuid);
        CHECK(cookedBaked.diffuseIntensity == 2.0f);
        CHECK(cookedBaked.specularIntensity == 0.25f);
        CHECK(!cookedBaked.applyProbeVolumes);
        CHECK(cookedProbe.requestedEnvironmentAssetGuid ==
            cookedProbe.environmentAssetGuid);
        CHECK(loaded.staging->world->registry().getComponent<LightComponent>(child)
            .castsShadows == false);
        CHECK(loaded.staging->world->references().records().size() == 7);

        auto optionalMissing = Iridium::stageCookedScene(blob.bytes,
            registries.runtime, {
                .assetAvailability = [](const Iridium::AssetDependency& value) {
                    return value.assetGuid && value.assetGuid->toString().ends_with(
                        "000000000001")
                        ? Iridium::CookedAssetAvailability::Missing
                        : Iridium::CookedAssetAvailability::Available;
                },
            });
        CHECK(optionalMissing);
        const auto meshId = *Iridium::ComponentTypeId::parse(
            "iridium.component.mesh");
        const auto* modelReference = optionalMissing.staging->world->references()
            .find({ rootUuid, meshId, "model" });
        CHECK(modelReference);
        CHECK(modelReference->resolution ==
            Iridium::StableReferenceResolution::Failed);

        CHECK(!Iridium::stageCookedScene(blob.bytes, registries.runtime, {
            .assetAvailability = [](const Iridium::AssetDependency&) {
                return Iridium::CookedAssetAvailability::Missing;
            },
        }));
        CHECK(Iridium::stageCookedScene(blob.bytes, registries.runtime, {
            .assetAvailability = [](const Iridium::AssetDependency&) {
                return Iridium::CookedAssetAvailability::Pending;
            },
        }));
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "core stable-reference round trip",
            completeCoreRoundTripUsesStableReferences },
        std::pair{ "removed path rejection", removedPathFieldIsRejected },
        std::pair{ "invalid reflection probe rejection",
            invalidReflectionProbeDomainIsRejected },
        std::pair{ "invalid baked lighting rejection",
            invalidBakedLightingDomainIsRejected },
        std::pair{ "core cooked semantic load",
            coreComponentsCookAndLoadSemantically },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) { std::cerr << "[FAIL] " << name << '\n'; return 1; }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
