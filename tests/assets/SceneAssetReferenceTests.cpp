#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/SourceSceneCapture.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/Components.h"

#include <iostream>
#include <string>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "check failed: " #condition \
                << " (line " << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

    constexpr std::string_view sourceScene = R"json({
      "format":"iridium.scene","schemaVersion":1,"name":"Asset references",
      "entities":[{
        "uuid":"019fb7d3-0400-7000-8000-000000000001",
        "components":[
          {"id":"iridium.component.name","version":1,"data":{"value":"Hero Vehicle"}},
          {"id":"iridium.component.transform","version":1,"data":{}},
          {"id":"iridium.component.relationship","version":1,"data":{"parent":null,"siblingOrder":3}},
          {"id":"iridium.component.mesh","version":1,"data":{
            "enabled":true,
            "model":{"assetGuid":"01890f4c-0000-7000-8000-000000000001"},
            "materialOverrides":[{
              "source":{"subassetGuid":"01890f4c-0000-7000-8000-000000000002"},
              "replacement":{"subassetGuid":"01890f4c-0000-7000-8000-000000000003"}
            }]
          }}
        ]
      }]
    })json";

    bool productionRoundTripUsesOnlyStableIdentity() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const auto read = Iridium::readSourceSceneSchema1(
            sourceScene, registries.runtime, registries.source);
        CHECK(read);
        auto staged = Iridium::stageSourceScene(
            *read.document, registries.runtime, registries.source);
        CHECK(staged);
        const Entity entity = staged.staging->world->registry()
            .getPool<MeshComponent>()->entities.front();
        const MeshComponent& mesh = staged.staging->world->registry()
            .getComponent<MeshComponent>(entity);
        CHECK(mesh.requestedAssetGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000001");
        CHECK(mesh.materialOverrides.size() == 1);
        CHECK(mesh.materialOverrides.front().sourceMaterialGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000002");
        CHECK(mesh.materialOverrides.front().materialGuid.toString() ==
            "01890f4c-0000-7000-8000-000000000003");
        CHECK(staged.staging->world->registry()
            .getComponent<RelationshipComponent>(entity).siblingOrder == 3);

        const auto captured = Iridium::captureSourceScene(
            *staged.staging->world, *read.document,
            registries.runtime, registries.source);
        CHECK(captured);
        const auto written = Iridium::writeSourceSceneCanonical(
            *captured.document, registries.runtime, registries.source);
        CHECK(written);
        CHECK(written.bytes->find("assetGuid") != std::string::npos);
        CHECK(written.bytes->find("subassetGuid") != std::string::npos);
        CHECK(written.bytes->find("EntityID") == std::string::npos);
        CHECK(written.bytes->find("meshPath") == std::string::npos);
        CHECK(written.bytes->find("children") == std::string::npos);
        CHECK(written.bytes->find("depth") == std::string::npos);

        const auto reread = Iridium::readSourceSceneSchema1(
            *written.bytes, registries.runtime, registries.source);
        CHECK(reread);
        const auto rewritten = Iridium::writeSourceSceneCanonical(
            *reread.document, registries.runtime, registries.source);
        CHECK(rewritten);
        CHECK(*rewritten.bytes == *written.bytes);
        return true;
    }

    bool malformedStableGuidCannotMutateAnActiveWorld() {
        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        Iridium::SceneWorld active;
        const auto uuid = *Iridium::SceneEntityUuid::parse(
            "019fb7d3-0400-7000-8000-000000000099");
        const Entity existing = active.createEntity(uuid);
        active.registry().addComponent<NameComponent>(existing, "Still active");
        const std::string malformed = R"json({
          "format":"iridium.scene","schemaVersion":1,"name":"Bad",
          "entities":[{"uuid":"019fb7d3-0400-7000-8000-000000000001",
          "components":[{"id":"iridium.component.mesh","version":1,
          "data":{"model":{"assetGuid":"not-a-guid"}}}]}]})json";
        const auto read = Iridium::readSourceSceneSchema1(
            malformed, registries.runtime, registries.source);
        CHECK(!read);
        CHECK(active.registry().aliveCount() == 1);
        CHECK(active.identities().resolve(uuid) == existing);
        CHECK(active.registry().getComponent<NameComponent>(existing).name ==
            "Still active");
        return true;
    }

} // namespace

int main() {
    if (!productionRoundTripUsesOnlyStableIdentity()) return 1;
    std::cout << "[PASS] production stable asset round trip\n";
    if (!malformedStableGuidCannotMutateAnActiveWorld()) return 1;
    std::cout << "[PASS] malformed GUID staging isolation\n";
    return 0;
}
