#include "scene/authoring/SourceSceneEnvelopeMigrator.h"

#include "scene/SceneEntityUuid.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Iridium {
    namespace {

        [[nodiscard]] SceneDiagnostic migrationDiagnostic(
            SceneDiagnosticSeverity severity,
            std::string code,
            std::string path,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt) {
            return {
                .severity = severity,
                .code = std::move(code),
                .phase = ScenePhase::EnvelopeMigration,
                .entity = entity,
                .propertyPath = std::move(path),
                .migrationFrom = 0,
                .migrationTo = 1,
                .message = std::move(message),
            };
        }

        [[nodiscard]] bool readLegacyId(const SourceJson& value, uint32_t& id) {
            if (value.is_number_unsigned()) {
                const uint64_t number = value.get<uint64_t>();
                if (number <= std::numeric_limits<uint32_t>::max()) {
                    id = static_cast<uint32_t>(number);
                    return true;
                }
            }
            if (value.is_number_integer()) {
                const int64_t number = value.get<int64_t>();
                if (number >= 0 &&
                    static_cast<uint64_t>(number) <=
                        std::numeric_limits<uint32_t>::max()) {
                    id = static_cast<uint32_t>(number);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isCanonicalUuidV7(std::string_view text) {
            if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
                text[18] != '-' || text[23] != '-' || text[14] != '7' ||
                (text[19] != '8' && text[19] != '9' &&
                 text[19] != 'a' && text[19] != 'b')) {
                return false;
            }
            bool nonzero = false;
            for (size_t index = 0; index < text.size(); ++index) {
                if (index == 8 || index == 13 || index == 18 || index == 23) continue;
                const char value = text[index];
                if (!((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'))) return false;
                if (value != '0') nonzero = true;
            }
            return nonzero;
        }

        [[nodiscard]] bool hasRemovedPathField(
            const SourceJson& entity,
            std::string& path) {
            if (entity.contains("MeshComponent") &&
                entity.at("MeshComponent").is_object()) {
                const SourceJson& mesh = entity.at("MeshComponent");
                for (std::string_view name :
                    { "meshPath", "currentMeshPath", "requestedMeshPath" }) {
                    if (mesh.contains(std::string(name))) {
                        path = "/MeshComponent/" + std::string(name);
                        return true;
                    }
                }
            }
            if (entity.contains("Mesh") && entity.at("Mesh").is_object() &&
                entity.at("Mesh").contains("FilePath")) {
                path = "/Mesh/FilePath";
                return true;
            }
            return false;
        }

        [[nodiscard]] SourceJson component(
            std::string_view id, SourceJson data) {
            return SourceJson{
                { "id", id },
                { "version", 1 },
                { "data", std::move(data) },
            };
        }

    } // namespace

    SourceSceneEnvelopeMigrationResult migrateSourceSceneV0(
        std::string_view bytes,
        std::span<const uint8_t, 16> sceneAssetGuid,
        SourceJsonParseOptions options) {
        SourceSceneEnvelopeMigrationResult result;
        SourceJsonParseResult parsed = parseSourceJsonStrict(bytes, options);
        result.diagnostics = std::move(parsed.diagnostics);
        if (!parsed) return result;
        const SourceJson& root = *parsed.value;
        if (!root.is_object() || !root.contains("Entities") ||
            !root.at("Entities").is_array()) {
            result.diagnostics.push_back(migrationDiagnostic(
                SceneDiagnosticSeverity::Error, "scene.v0.envelope",
                {}, "Version-0 scene must contain an Entities array"));
            return result;
        }

        struct LegacyEntity {
            uint32_t id = 0;
            SceneEntityUuid uuid;
            const SourceJson* value = nullptr;
            std::optional<uint32_t> parent;
            uint32_t siblingOrder = 0;
        };
        std::vector<LegacyEntity> entities;
        entities.reserve(root.at("Entities").size());
        std::unordered_map<uint32_t, size_t> byId;
        for (size_t index = 0; index < root.at("Entities").size(); ++index) {
            const SourceJson& entity = root.at("Entities").at(index);
            const std::string base = "/Entities/" + std::to_string(index);
            if (!entity.is_object() || !entity.contains("EntityID")) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error, "scene.v0.entity_id",
                    base + "/EntityID",
                    "Every version-0 entity requires an integer EntityID"));
                continue;
            }
            uint32_t id = 0;
            if (!readLegacyId(entity.at("EntityID"), id) ||
                !byId.emplace(id, index).second) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error,
                    "scene.v0.duplicate_or_invalid_entity_id",
                    base + "/EntityID",
                    "Version-0 EntityID must be a unique uint32"));
                continue;
            }
            std::string removedPath;
            if (hasRemovedPathField(entity, removedPath)) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error,
                    "scene.v0.path_identity_removed", base + removedPath,
                    "Path-based mesh identity cannot be migrated; assign an asset GUID in an M3-compatible editor first"));
            }
            const SceneEntityUuid uuid = deriveSceneEntityUuidV5(
                sceneAssetGuid, "legacy-entity/" + std::to_string(id));
            LegacyEntity legacy{
                .id = id,
                .uuid = uuid,
                .value = &entity,
                .siblingOrder = static_cast<uint32_t>(index),
            };
            if (entity.contains("RelationshipComponent")) {
                const SourceJson& relationship = entity.at("RelationshipComponent");
                if (!relationship.is_object()) {
                    result.diagnostics.push_back(migrationDiagnostic(
                        SceneDiagnosticSeverity::Error,
                        "scene.v0.relationship_type",
                        base + "/RelationshipComponent",
                        "Version-0 RelationshipComponent must be an object", uuid));
                }
                else {
                    if (relationship.contains("parent") &&
                        !relationship.at("parent").is_null()) {
                        uint32_t parent = 0;
                        if (!readLegacyId(relationship.at("parent"), parent)) {
                            result.diagnostics.push_back(migrationDiagnostic(
                                SceneDiagnosticSeverity::Error,
                                "scene.v0.parent_id",
                                base + "/RelationshipComponent/parent",
                                "Version-0 parent must be null or a uint32 EntityID",
                                uuid));
                        }
                        else legacy.parent = parent;
                    }
                    if (relationship.contains("siblingOrder") &&
                        !readLegacyId(relationship.at("siblingOrder"),
                            legacy.siblingOrder)) {
                        result.diagnostics.push_back(migrationDiagnostic(
                            SceneDiagnosticSeverity::Error,
                            "scene.v0.sibling_order",
                            base + "/RelationshipComponent/siblingOrder",
                            "Version-0 siblingOrder must be a nonnegative uint32",
                            uuid));
                    }
                }
            }
            entities.push_back(legacy);
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (const LegacyEntity& entity : entities) {
            if (entity.parent && !byId.contains(*entity.parent)) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error, "scene.v0.missing_parent",
                    "/Entities", "Version-0 parent EntityID does not exist",
                    entity.uuid));
            }
        }
        enum class Visit : uint8_t { Unseen, Visiting, Complete };
        std::vector<Visit> visits(entities.size(), Visit::Unseen);
        const auto visit = [&](const auto& self, size_t index) -> void {
            if (visits[index] == Visit::Complete) return;
            if (visits[index] == Visit::Visiting) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error, "scene.v0.hierarchy_cycle",
                    "/Entities", "Version-0 relationship hierarchy contains a cycle",
                    entities[index].uuid));
                return;
            }
            visits[index] = Visit::Visiting;
            if (entities[index].parent) {
                const auto found = byId.find(*entities[index].parent);
                if (found != byId.end()) self(self, found->second);
            }
            visits[index] = Visit::Complete;
        };
        for (size_t index = 0; index < entities.size(); ++index) visit(visit, index);
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::map<std::optional<uint32_t>, std::set<uint32_t>> siblingOrders;
        std::unordered_map<uint32_t, std::set<uint32_t>> derivedChildren;
        for (const LegacyEntity& entity : entities) {
            if (!siblingOrders[entity.parent].insert(entity.siblingOrder).second) {
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Error,
                    "scene.v0.duplicate_sibling_order", "/Entities",
                    "Version-0 sibling order is duplicated beneath one parent",
                    entity.uuid));
            }
            if (entity.parent) derivedChildren[*entity.parent].insert(entity.id);
        }
        for (size_t index = 0; index < entities.size(); ++index) {
            const LegacyEntity& legacy = entities[index];
            const SourceJson& input = *legacy.value;
            if (!input.contains("RelationshipComponent") ||
                !input.at("RelationshipComponent").is_object()) continue;
            const SourceJson& relationship = input.at("RelationshipComponent");
            if (relationship.contains("children")) {
                std::set<uint32_t> declaredChildren;
                if (!relationship.at("children").is_array()) {
                    result.diagnostics.push_back(migrationDiagnostic(
                        SceneDiagnosticSeverity::Error,
                        "scene.v0.children_type",
                        "/Entities/" + std::to_string(index) +
                            "/RelationshipComponent/children",
                        "Version-0 children must be an array of EntityIDs",
                        legacy.uuid));
                }
                else {
                    for (const SourceJson& childValue :
                        relationship.at("children")) {
                        uint32_t child = 0;
                        if (!readLegacyId(childValue, child) ||
                            !declaredChildren.insert(child).second) {
                            result.diagnostics.push_back(migrationDiagnostic(
                                SceneDiagnosticSeverity::Error,
                                "scene.v0.children_id",
                                "/Entities/" + std::to_string(index) +
                                    "/RelationshipComponent/children",
                                "Version-0 children contains an invalid or duplicate EntityID",
                                legacy.uuid));
                            break;
                        }
                    }
                    if (declaredChildren != derivedChildren[legacy.id]) {
                        result.diagnostics.push_back(migrationDiagnostic(
                            SceneDiagnosticSeverity::Error,
                            "scene.v0.children_contradiction",
                            "/Entities/" + std::to_string(index) +
                                "/RelationshipComponent/children",
                            "Version-0 children contradict the authoritative parent links",
                            legacy.uuid));
                    }
                }
            }
            if (relationship.contains("depth")) {
                uint32_t declaredDepth = 0;
                if (!readLegacyId(relationship.at("depth"), declaredDepth)) {
                    result.diagnostics.push_back(migrationDiagnostic(
                        SceneDiagnosticSeverity::Error, "scene.v0.depth_type",
                        "/Entities/" + std::to_string(index) +
                            "/RelationshipComponent/depth",
                        "Version-0 depth must be a nonnegative uint32",
                        legacy.uuid));
                }
                else {
                    uint32_t derivedDepth = 0;
                    std::optional<uint32_t> parent = legacy.parent;
                    while (parent) {
                        ++derivedDepth;
                        parent = entities[byId.at(*parent)].parent;
                    }
                    if (declaredDepth != derivedDepth) {
                        result.diagnostics.push_back(migrationDiagnostic(
                            SceneDiagnosticSeverity::Error,
                            "scene.v0.depth_contradiction",
                            "/Entities/" + std::to_string(index) +
                                "/RelationshipComponent/depth",
                            "Version-0 depth contradicts the authoritative parent links",
                            legacy.uuid));
                    }
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        SourceJson output = SourceJson::object();
        output["format"] = "iridium.scene";
        output["schemaVersion"] = 1;
        output["name"] = root.contains("Scene") && root.at("Scene").is_string()
            ? root.at("Scene")
            : SourceJson("Untitled Scene");
        output["entities"] = SourceJson::array();

        for (size_t index = 0; index < entities.size(); ++index) {
            const LegacyEntity& legacy = entities[index];
            const SourceJson& input = *legacy.value;
            SourceJson entity = SourceJson::object();
            entity["uuid"] = legacy.uuid.toString();
            entity["components"] = SourceJson::array();
            const std::string name = input.contains("Name") &&
                input.at("Name").is_string()
                ? input.at("Name").get<std::string>()
                : "Entity " + std::to_string(legacy.id);
            entity["components"].push_back(component(
                "iridium.component.name", SourceJson{ { "value", name } }));

            if (input.contains("TransformComponent")) {
                entity["components"].push_back(component(
                    "iridium.component.transform",
                    input.at("TransformComponent")));
            }

            SourceJson relationship = SourceJson::object();
            relationship["parent"] = legacy.parent
                ? SourceJson(entities[byId.at(*legacy.parent)].uuid.toString())
                : SourceJson(nullptr);
            relationship["siblingOrder"] = legacy.siblingOrder;
            entity["components"].push_back(component(
                "iridium.component.relationship", std::move(relationship)));

            if (input.contains("MeshComponent")) {
                const SourceJson& oldMesh = input.at("MeshComponent");
                if (!oldMesh.is_object()) {
                    result.diagnostics.push_back(migrationDiagnostic(
                        SceneDiagnosticSeverity::Error, "scene.v0.mesh_type",
                        "/Entities/" + std::to_string(index) + "/MeshComponent",
                        "Version-0 MeshComponent must be an object", legacy.uuid));
                }
                else {
                    SourceJson mesh = SourceJson::object();
                    mesh["enabled"] = oldMesh.value("enabled", true);
                    if (!oldMesh.contains("assetGuid") ||
                        !oldMesh.at("assetGuid").is_string() ||
                        !isCanonicalUuidV7(
                            oldMesh.at("assetGuid").get<std::string>())) {
                        result.diagnostics.push_back(migrationDiagnostic(
                            SceneDiagnosticSeverity::Error, "scene.v0.asset_guid",
                            "/Entities/" + std::to_string(index) +
                                "/MeshComponent/assetGuid",
                            "Version-0 mesh assetGuid must be a canonical UUIDv7",
                            legacy.uuid));
                    }
                    else {
                        mesh["model"] = SourceJson{
                            { "assetGuid", oldMesh.at("assetGuid") },
                        };
                    }
                    mesh["materialOverrides"] = SourceJson::array();
                    if (oldMesh.contains("materialOverrides")) {
                        if (!oldMesh.at("materialOverrides").is_array()) {
                            result.diagnostics.push_back(migrationDiagnostic(
                                SceneDiagnosticSeverity::Error,
                                "scene.v0.material_overrides",
                                "/Entities/" + std::to_string(index) +
                                    "/MeshComponent/materialOverrides",
                                "Version-0 materialOverrides must be an array",
                                legacy.uuid));
                        }
                        else for (const SourceJson& oldOverride :
                            oldMesh.at("materialOverrides")) {
                            if (!oldOverride.is_object() ||
                                !oldOverride.contains("sourceMaterialGuid") ||
                                !oldOverride.contains("materialGuid") ||
                                !oldOverride.at("sourceMaterialGuid").is_string() ||
                                !oldOverride.at("materialGuid").is_string() ||
                                !isCanonicalUuidV7(oldOverride.at(
                                    "sourceMaterialGuid").get<std::string>()) ||
                                !isCanonicalUuidV7(oldOverride.at(
                                    "materialGuid").get<std::string>())) {
                                result.diagnostics.push_back(migrationDiagnostic(
                                    SceneDiagnosticSeverity::Error,
                                    "scene.v0.material_override_guid",
                                    "/Entities/" + std::to_string(index) +
                                        "/MeshComponent/materialOverrides",
                                    "Version-0 material override GUIDs must be canonical UUIDv7 values",
                                    legacy.uuid));
                            }
                            else {
                                mesh["materialOverrides"].push_back(SourceJson{
                                    { "source", SourceJson{
                                        { "subassetGuid", oldOverride.at(
                                            "sourceMaterialGuid") } } },
                                    { "replacement", SourceJson{
                                        { "subassetGuid", oldOverride.at(
                                            "materialGuid") } } },
                                });
                            }
                        }
                    }
                    entity["components"].push_back(component(
                        "iridium.component.mesh", std::move(mesh)));
                }
            }
            if (input.contains("LightComponent")) {
                entity["components"].push_back(component(
                    "iridium.component.light", input.at("LightComponent")));
            }

            static const std::set<std::string, std::less<>> knownFields{
                "EntityID", "Name", "TransformComponent", "MeshComponent",
                "LightComponent", "RelationshipComponent",
            };
            SourceJson legacyUnknown = SourceJson::object();
            for (auto iterator = input.begin(); iterator != input.end(); ++iterator) {
                if (!knownFields.contains(iterator.key())) {
                    legacyUnknown[iterator.key()] = *iterator;
                }
            }
            if (!legacyUnknown.empty()) {
                entity["extensions"] = SourceJson{
                    { "iridium.legacy.v0", std::move(legacyUnknown) },
                };
                result.diagnostics.push_back(migrationDiagnostic(
                    SceneDiagnosticSeverity::Warning,
                    "scene.v0.unknown_entity_fields",
                    "/Entities/" + std::to_string(index),
                    "Unknown version-0 entity fields were preserved as a legacy extension",
                    legacy.uuid));
            }
            output["entities"].push_back(std::move(entity));
        }

        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }
        result.value = std::move(output);
        sortSceneDiagnostics(result.diagnostics);
        return result;
    }

} // namespace Iridium
