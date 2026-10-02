#include "scene/authoring/SourceSceneDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] std::string pointerToken(std::string_view token) {
            std::string result;
            for (char value : token) {
                if (value == '~') result += "~0";
                else if (value == '/') result += "~1";
                else result.push_back(value);
            }
            return result;
        }

        [[nodiscard]] SceneDiagnostic error(
            std::string code,
            ScenePhase phase,
            std::string path,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt,
            std::optional<ComponentTypeId> component = std::nullopt,
            std::optional<uint32_t> componentVersion = std::nullopt) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = phase,
                .entity = entity,
                .component = std::move(component),
                .componentVersion = componentVersion,
                .propertyPath = std::move(path),
                .message = std::move(message),
            };
        }

        [[nodiscard]] bool readUint32(
            const SourceJson& value, uint32_t& output) {
            if (!value.is_number_unsigned()) return false;
            const uint64_t number = value.get<uint64_t>();
            if (number > std::numeric_limits<uint32_t>::max()) return false;
            output = static_cast<uint32_t>(number);
            return true;
        }

        [[nodiscard]] SourceJson canonicalValue(const SourceJson& value) {
            if (value.is_object()) {
                SourceJson output = SourceJson::object();
                std::vector<std::string> keys;
                keys.reserve(value.size());
                for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
                    keys.push_back(iterator.key());
                }
                std::ranges::sort(keys);
                for (const std::string& key : keys) {
                    output[key] = canonicalValue(value.at(key));
                }
                return output;
            }
            if (value.is_array()) {
                SourceJson output = SourceJson::array();
                for (const SourceJson& element : value) {
                    output.push_back(canonicalValue(element));
                }
                return output;
            }
            return value;
        }

        [[nodiscard]] bool canonicalUuidV7(std::string_view text) {
            if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
                text[18] != '-' || text[23] != '-' || text[14] != '7' ||
                (text[19] != '8' && text[19] != '9' &&
                 text[19] != 'a' && text[19] != 'b')) return false;
            for (size_t index = 0; index < text.size(); ++index) {
                if (index == 8 || index == 13 || index == 18 || index == 23) continue;
                const char value = text[index];
                if (!((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'))) return false;
            }
            return true;
        }

        [[nodiscard]] bool finiteFloat32(const SourceJson& value) {
            if (!value.is_number()) return false;
            const double number = value.get<double>();
            return std::isfinite(number) &&
                std::abs(number) <= std::numeric_limits<float>::max();
        }

        void validateKnownProperties(
            const SourceSceneComponent& component,
            const RuntimeComponentDescriptor& descriptor,
            const ComponentSerializerRegistry& sourceRegistry,
            std::string_view path,
            SceneEntityUuid entity,
            std::vector<SceneDiagnostic>& diagnostics) {
            for (const PropertyDescriptor& property : descriptor.properties) {
                const std::string name(sourceRegistry.sourceName(
                    component.id, property.id));
                const std::string propertyPath = std::string(path) + "/" +
                    pointerToken(name);
                if (!component.data.contains(name)) {
                    if (property.required) diagnostics.push_back(error(
                        "scene.property.required", ScenePhase::Deserialize,
                        propertyPath, "Required component property is absent",
                        entity, component.id, component.version));
                    continue;
                }
                const SourceJson& value = component.data.at(name);
                if (value.is_null()) {
                    if (!property.nullable) diagnostics.push_back(error(
                        "scene.property.null_not_allowed", ScenePhase::Deserialize,
                        propertyPath, "Property is not nullable", entity,
                        component.id, component.version));
                    continue;
                }
                bool valid = false;
                switch (property.valueType) {
                case PropertyValueType::Boolean: valid = value.is_boolean(); break;
                case PropertyValueType::Int32:
                case PropertyValueType::Enum:
                    valid = value.is_number_integer() &&
                        value.get<int64_t>() >= std::numeric_limits<int32_t>::min() &&
                        value.get<int64_t>() <= std::numeric_limits<int32_t>::max();
                    break;
                case PropertyValueType::Float32: valid = finiteFloat32(value); break;
                case PropertyValueType::String: valid = value.is_string(); break;
                case PropertyValueType::Float32x3:
                    valid = value.is_array() && value.size() == 3 &&
                        std::ranges::all_of(value, finiteFloat32);
                    break;
                case PropertyValueType::EntityReference:
                    valid = value.is_string() &&
                        SceneEntityUuid::parse(value.get<std::string>()).has_value() &&
                        SceneEntityUuid::parse(value.get<std::string>())->toString() ==
                            value.get<std::string>();
                    break;
                case PropertyValueType::AssetReference:
                    valid = value.is_object() && value.size() == 1 &&
                        value.contains("assetGuid") &&
                        value.at("assetGuid").is_string() &&
                        canonicalUuidV7(value.at("assetGuid").get<std::string>());
                    break;
                case PropertyValueType::SubassetReference:
                    valid = value.is_object() && value.size() == 1 &&
                        value.contains("subassetGuid") &&
                        value.at("subassetGuid").is_string() &&
                        canonicalUuidV7(value.at("subassetGuid").get<std::string>());
                    break;
                case PropertyValueType::Collection: valid = value.is_array(); break;
                }
                if (!valid) diagnostics.push_back(error(
                    "scene.property.type_or_domain", ScenePhase::Deserialize,
                    propertyPath, "Property value does not match its registered type or domain",
                    entity, component.id, component.version));

                if (valid && property.collectionOrdering ==
                    CollectionOrdering::SourceSubassetGuid) {
                    std::unordered_set<std::string> sources;
                    for (size_t index = 0; index < value.size(); ++index) {
                        const SourceJson& entry = value.at(index);
                        const bool entryValid = entry.is_object() &&
                            entry.contains("source") && entry.contains("replacement") &&
                            entry.at("source").is_object() &&
                            entry.at("replacement").is_object() &&
                            entry.at("source").contains("subassetGuid") &&
                            entry.at("replacement").contains("subassetGuid") &&
                            entry.at("source").at("subassetGuid").is_string() &&
                            entry.at("replacement").at("subassetGuid").is_string() &&
                            canonicalUuidV7(entry.at("source").at(
                                "subassetGuid").get<std::string>()) &&
                            canonicalUuidV7(entry.at("replacement").at(
                                "subassetGuid").get<std::string>());
                        if (!entryValid || !sources.insert(entryValid
                            ? entry.at("source").at("subassetGuid").get<std::string>()
                            : std::string()).second) {
                            diagnostics.push_back(error(
                                "scene.property.collection_entry",
                                ScenePhase::Deserialize, propertyPath + "/" +
                                    std::to_string(index),
                                "Collection entry is invalid or has a duplicate source identity",
                                entity, component.id, component.version));
                        }
                    }
                }
            }
        }

        void copyUnknownFields(
            const SourceJson& source,
            std::initializer_list<std::string_view> known,
            SourceJson& destination) {
            destination = SourceJson::object();
            for (auto iterator = source.begin(); iterator != source.end(); ++iterator) {
                const bool isKnown = std::ranges::any_of(known,
                    [&](std::string_view value) { return value == iterator.key(); });
                if (!isKnown) destination[iterator.key()] = *iterator;
            }
        }

        [[nodiscard]] const SourceSceneComponent* relationshipOf(
            const SourceSceneEntity& entity) {
            const auto found = std::ranges::find_if(entity.components,
                [](const SourceSceneComponent& component) {
                    return component.id.value() ==
                        "iridium.component.relationship";
                });
            return found == entity.components.end() ? nullptr : &*found;
        }

        struct HierarchyNode {
            std::optional<SceneEntityUuid> parent;
            uint32_t siblingOrder = 0;
        };

        [[nodiscard]] std::vector<size_t> validateAndOrderHierarchy(
            const SourceSceneDocument& document,
            std::vector<SceneDiagnostic>& diagnostics,
            ScenePhase phase) {
            std::unordered_map<SceneEntityUuid, size_t, SceneEntityUuidHash> indices;
            indices.reserve(document.entities.size());
            for (size_t index = 0; index < document.entities.size(); ++index) {
                const SceneEntityUuid uuid = document.entities[index].uuid;
                if (!uuid.isSupported() || !indices.emplace(uuid, index).second) {
                    diagnostics.push_back(error(
                        "scene.entity.duplicate_or_invalid_uuid", phase,
                        "/entities/" + std::to_string(index) + "/uuid",
                        "Entity UUID is invalid or duplicated", uuid));
                }
            }

            std::vector<HierarchyNode> nodes(document.entities.size());
            for (size_t index = 0; index < document.entities.size(); ++index) {
                const SourceSceneEntity& entity = document.entities[index];
                const SourceSceneComponent* relationship = relationshipOf(entity);
                if (!relationship) continue;
                const std::string base = "/entities/" + std::to_string(index) +
                    "/components/" + relationship->id.value() + "/data";
                if (!relationship->data.is_object()) {
                    diagnostics.push_back(error(
                        "scene.relationship.data_type", phase, base,
                        "Relationship data must be an object", entity.uuid,
                        relationship->id, relationship->version));
                    continue;
                }
                if (relationship->data.contains("parent") &&
                    !relationship->data.at("parent").is_null()) {
                    const SourceJson& parent = relationship->data.at("parent");
                    if (!parent.is_string()) {
                        diagnostics.push_back(error(
                            "scene.relationship.parent_type", phase,
                            base + "/parent",
                            "Relationship parent must be null or a canonical entity UUID",
                            entity.uuid, relationship->id, relationship->version));
                    }
                    else {
                        const std::string text = parent.get<std::string>();
                        const auto parsed = SceneEntityUuid::parse(text);
                        if (!parsed || parsed->toString() != text) {
                            diagnostics.push_back(error(
                                "scene.relationship.parent_uuid", phase,
                                base + "/parent",
                                "Relationship parent is not a canonical supported UUID",
                                entity.uuid, relationship->id, relationship->version));
                        }
                        else {
                            nodes[index].parent = *parsed;
                        }
                    }
                }
                if (relationship->data.contains("siblingOrder")) {
                    if (!readUint32(relationship->data.at("siblingOrder"),
                        nodes[index].siblingOrder)) {
                        diagnostics.push_back(error(
                            "scene.relationship.sibling_order", phase,
                            base + "/siblingOrder",
                            "Relationship siblingOrder must be a nonnegative uint32",
                            entity.uuid, relationship->id, relationship->version));
                    }
                }
            }

            std::map<std::string, std::map<uint32_t, size_t>> siblingOrders;
            for (size_t index = 0; index < nodes.size(); ++index) {
                const HierarchyNode& node = nodes[index];
                if (node.parent) {
                    if (*node.parent == document.entities[index].uuid) {
                        diagnostics.push_back(error(
                            "scene.hierarchy.self_parent", phase, "/entities",
                            "Entity cannot be its own parent",
                            document.entities[index].uuid));
                    }
                    else if (!indices.contains(*node.parent)) {
                        diagnostics.push_back(error(
                            "scene.hierarchy.missing_parent", phase, "/entities",
                            "Relationship parent does not exist in this scene",
                            document.entities[index].uuid));
                    }
                }
                const std::string group = node.parent
                    ? node.parent->toString()
                    : std::string();
                if (!siblingOrders[group].emplace(
                    node.siblingOrder, index).second) {
                    diagnostics.push_back(error(
                        "scene.hierarchy.duplicate_sibling_order", phase,
                        "/entities",
                        "Sibling order must be unique for every parent",
                        document.entities[index].uuid));
                }
            }

            enum class Visit : uint8_t { Unseen, Visiting, Complete };
            std::vector<Visit> visits(nodes.size(), Visit::Unseen);
            const auto visit = [&](const auto& self, size_t index) -> void {
                if (visits[index] == Visit::Complete) return;
                if (visits[index] == Visit::Visiting) {
                    diagnostics.push_back(error(
                        "scene.hierarchy.cycle", phase, "/entities",
                        "Relationship hierarchy contains a cycle",
                        document.entities[index].uuid));
                    return;
                }
                visits[index] = Visit::Visiting;
                if (nodes[index].parent) {
                    const auto found = indices.find(*nodes[index].parent);
                    if (found != indices.end()) self(self, found->second);
                }
                visits[index] = Visit::Complete;
            };
            for (size_t index = 0; index < nodes.size(); ++index) visit(visit, index);

            if (hasSceneErrors(diagnostics)) return {};

            std::map<std::optional<SceneEntityUuid>, std::vector<size_t>> children;
            for (size_t index = 0; index < nodes.size(); ++index) {
                children[nodes[index].parent].push_back(index);
            }
            for (auto& [parent, values] : children) {
                std::ranges::sort(values, [&](size_t lhs, size_t rhs) {
                    if (nodes[lhs].siblingOrder != nodes[rhs].siblingOrder) {
                        return nodes[lhs].siblingOrder < nodes[rhs].siblingOrder;
                    }
                    return document.entities[lhs].uuid < document.entities[rhs].uuid;
                });
            }

            std::vector<size_t> ordered;
            ordered.reserve(document.entities.size());
            const auto append = [&](const auto& self,
                std::optional<SceneEntityUuid> parent) -> void {
                const auto found = children.find(parent);
                if (found == children.end()) return;
                for (size_t index : found->second) {
                    ordered.push_back(index);
                    self(self, document.entities[index].uuid);
                }
            };
            append(append, std::nullopt);
            return ordered;
        }

        [[nodiscard]] SourceJson canonicalComponentData(
            const SourceSceneComponent& component,
            const RuntimeComponentRegistry& runtimeRegistry,
            const ComponentSerializerRegistry& sourceRegistry) {
            if (!component.known) return canonicalValue(component.data);
            const RuntimeComponentDescriptor* descriptor =
                runtimeRegistry.find(component.id);
            if (!descriptor || !component.data.is_object()) {
                return canonicalValue(component.data);
            }

            SourceJson output = SourceJson::object();
            std::unordered_set<std::string> knownNames;
            for (const PropertyDescriptor& property : descriptor->properties) {
                const std::string name(sourceRegistry.sourceName(
                    component.id, property.id));
                knownNames.insert(name);
                if (component.data.contains(name)) {
                    SourceJson canonical = canonicalValue(component.data.at(name));
                    if (property.collectionOrdering ==
                        CollectionOrdering::SourceSubassetGuid &&
                        canonical.is_array()) {
                        std::vector<SourceJson> entries(
                            canonical.begin(), canonical.end());
                        std::ranges::sort(entries,
                            [](const SourceJson& lhs, const SourceJson& rhs) {
                                return lhs.at("source").at("subassetGuid")
                                    .get<std::string>() <
                                    rhs.at("source").at("subassetGuid")
                                    .get<std::string>();
                            });
                        canonical = SourceJson::array();
                        for (SourceJson& entry : entries) {
                            canonical.push_back(std::move(entry));
                        }
                    }
                    output[name] = std::move(canonical);
                }
            }
            std::vector<std::string> unknownNames;
            for (auto iterator = component.data.begin();
                iterator != component.data.end(); ++iterator) {
                if (!knownNames.contains(iterator.key())) {
                    unknownNames.push_back(iterator.key());
                }
            }
            std::ranges::sort(unknownNames);
            for (const std::string& name : unknownNames) {
                output[name] = canonicalValue(component.data.at(name));
            }
            return output;
        }

        void appendUnknownObject(
            SourceJson& destination,
            const SourceJson& unknown,
            std::initializer_list<std::string_view> reserved,
            std::vector<SceneDiagnostic>& diagnostics,
            std::string_view path) {
            if (!unknown.is_object()) {
                diagnostics.push_back(error(
                    "scene.unknown_store.type", ScenePhase::Save,
                    std::string(path), "Unknown-field store must be an object"));
                return;
            }
            std::vector<std::string> names;
            for (auto iterator = unknown.begin(); iterator != unknown.end(); ++iterator) {
                const bool collision = std::ranges::any_of(reserved,
                    [&](std::string_view value) { return value == iterator.key(); });
                if (collision) {
                    diagnostics.push_back(error(
                        "scene.unknown_store.reserved_collision", ScenePhase::Save,
                        std::string(path) + "/" + pointerToken(iterator.key()),
                        "Unknown field collides with a reserved schema field"));
                }
                else names.push_back(iterator.key());
            }
            std::ranges::sort(names);
            for (const std::string& name : names) {
                destination[name] = canonicalValue(unknown.at(name));
            }
        }

    } // namespace

    SourceSceneReadResult readSourceSceneSchema1(
        std::string_view bytes,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry,
        SourceJsonParseOptions options) {
        SourceSceneReadResult result;
        if (!runtimeRegistry.isFrozen() || !sourceRegistry.isFrozen()) {
            result.diagnostics.push_back(error(
                "scene.registry.not_frozen", ScenePhase::Parse, {},
                "Runtime and source component registries must be frozen"));
            return result;
        }

        SourceJsonParseResult parsed = parseSourceJsonStrict(bytes, options);
        result.diagnostics = std::move(parsed.diagnostics);
        if (!parsed) return result;
        const SourceJson& root = *parsed.value;
        if (!root.is_object()) {
            result.diagnostics.push_back(error(
                "scene.envelope.type", ScenePhase::Parse, {},
                "Source scene root must be an object"));
            return result;
        }
        if (!root.contains("format") || !root.at("format").is_string() ||
            root.at("format").get<std::string_view>() != kSourceSceneFormat) {
            result.diagnostics.push_back(error(
                "scene.envelope.format", ScenePhase::Parse, "/format",
                "Source scene format must be iridium.scene"));
        }
        uint32_t schemaVersion = 0;
        if (!root.contains("schemaVersion") ||
            !readUint32(root.at("schemaVersion"), schemaVersion)) {
            result.diagnostics.push_back(error(
                "scene.envelope.schema_version_type", ScenePhase::Parse,
                "/schemaVersion", "Source scene schemaVersion must be a uint32"));
        }
        else if (schemaVersion != kCurrentSourceSceneSchemaVersion) {
            result.diagnostics.push_back(error(
                schemaVersion > kCurrentSourceSceneSchemaVersion
                    ? "scene.envelope.future_version"
                    : "scene.envelope.migration_required",
                ScenePhase::EnvelopeMigration, "/schemaVersion",
                "Source scene schema version is not supported by the schema-1 reader"));
        }
        if (!root.contains("name") || !root.at("name").is_string()) {
            result.diagnostics.push_back(error(
                "scene.envelope.name", ScenePhase::Parse, "/name",
                "Source scene name must be a string"));
        }
        if (!root.contains("entities") || !root.at("entities").is_array()) {
            result.diagnostics.push_back(error(
                "scene.envelope.entities", ScenePhase::Parse, "/entities",
                "Source scene entities must be an array"));
        }
        if (root.contains("extensions") && !root.at("extensions").is_object()) {
            result.diagnostics.push_back(error(
                "scene.envelope.extensions", ScenePhase::Parse, "/extensions",
                "Source scene extensions must be an object"));
        }
        if (hasSceneErrors(result.diagnostics)) return result;

        SourceSceneDocument document;
        document.name = root.at("name").get<std::string>();
        if (root.contains("extensions")) document.extensions = root.at("extensions");
        copyUnknownFields(root,
            { "format", "schemaVersion", "name", "entities", "extensions" },
            document.unknownFields);

        const SourceJson& entities = root.at("entities");
        document.entities.reserve(entities.size());
        std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> entityIds;
        for (size_t entityIndex = 0; entityIndex < entities.size(); ++entityIndex) {
            const SourceJson& entityJson = entities.at(entityIndex);
            const std::string entityPath = "/entities/" +
                std::to_string(entityIndex);
            if (!entityJson.is_object()) {
                result.diagnostics.push_back(error(
                    "scene.entity.type", ScenePhase::Parse, entityPath,
                    "Scene entity must be an object"));
                continue;
            }
            if (!entityJson.contains("uuid") ||
                !entityJson.at("uuid").is_string()) {
                result.diagnostics.push_back(error(
                    "scene.entity.uuid_type", ScenePhase::Identity,
                    entityPath + "/uuid", "Entity UUID must be a string"));
                continue;
            }
            const std::string uuidText = entityJson.at("uuid").get<std::string>();
            const auto uuid = SceneEntityUuid::parse(uuidText);
            if (!uuid || uuid->toString() != uuidText) {
                result.diagnostics.push_back(error(
                    "scene.entity.uuid", ScenePhase::Identity,
                    entityPath + "/uuid",
                    "Entity UUID must be canonical lowercase UUIDv5 or UUIDv7"));
                continue;
            }
            if (!entityIds.insert(*uuid).second) {
                result.diagnostics.push_back(error(
                    "scene.entity.duplicate_uuid", ScenePhase::Identity,
                    entityPath + "/uuid", "Entity UUID is duplicated", *uuid));
                continue;
            }
            if (!entityJson.contains("components") ||
                !entityJson.at("components").is_array()) {
                result.diagnostics.push_back(error(
                    "scene.entity.components", ScenePhase::Parse,
                    entityPath + "/components",
                    "Entity components must be an array", *uuid));
                continue;
            }
            if (entityJson.contains("extensions") &&
                !entityJson.at("extensions").is_object()) {
                result.diagnostics.push_back(error(
                    "scene.entity.extensions", ScenePhase::Parse,
                    entityPath + "/extensions",
                    "Entity extensions must be an object", *uuid));
                continue;
            }

            SourceSceneEntity entity;
            entity.uuid = *uuid;
            if (entityJson.contains("extensions")) {
                entity.extensions = entityJson.at("extensions");
            }
            copyUnknownFields(entityJson,
                { "uuid", "components", "extensions" }, entity.unknownFields);
            std::unordered_set<std::string> componentIds;
            const SourceJson& components = entityJson.at("components");
            for (size_t componentIndex = 0;
                componentIndex < components.size(); ++componentIndex) {
                const SourceJson& componentJson = components.at(componentIndex);
                const std::string componentPath = entityPath + "/components/" +
                    std::to_string(componentIndex);
                if (!componentJson.is_object()) {
                    result.diagnostics.push_back(error(
                        "scene.component.type", ScenePhase::Parse, componentPath,
                        "Component envelope must be an object", *uuid));
                    continue;
                }
                if (!componentJson.contains("id") ||
                    !componentJson.at("id").is_string()) {
                    result.diagnostics.push_back(error(
                        "scene.component.id_type", ScenePhase::Parse,
                        componentPath + "/id",
                        "Component ID must be a string", *uuid));
                    continue;
                }
                const std::string idText = componentJson.at("id").get<std::string>();
                const auto id = ComponentTypeId::parse(idText);
                if (!id) {
                    result.diagnostics.push_back(error(
                        "scene.component.id", ScenePhase::Parse,
                        componentPath + "/id",
                        "Component ID does not follow the stable ID grammar", *uuid));
                    continue;
                }
                if (!componentIds.insert(idText).second) {
                    result.diagnostics.push_back(error(
                        "scene.component.duplicate_id", ScenePhase::Parse,
                        componentPath + "/id",
                        "Component ID is duplicated on this entity", *uuid, *id));
                    continue;
                }
                uint32_t version = 0;
                if (!componentJson.contains("version") ||
                    !readUint32(componentJson.at("version"), version) ||
                    version == 0) {
                    result.diagnostics.push_back(error(
                        "scene.component.version", ScenePhase::Parse,
                        componentPath + "/version",
                        "Component version must be a nonzero uint32", *uuid, *id));
                    continue;
                }
                if (!componentJson.contains("data") ||
                    !componentJson.at("data").is_object()) {
                    result.diagnostics.push_back(error(
                        "scene.component.data", ScenePhase::Parse,
                        componentPath + "/data",
                        "Component data must be an object", *uuid, *id, version));
                    continue;
                }

                SourceSceneComponent component;
                component.id = *id;
                component.version = version;
                component.data = componentJson.at("data");
                copyUnknownFields(componentJson,
                    { "id", "version", "data" },
                    component.unknownEnvelopeFields);
                const SourceComponentCodec* codec = sourceRegistry.find(*id);
                component.known = codec != nullptr;
                if (codec) {
                    SourceMigrationResult migrated = sourceRegistry.migrateToCurrent(
                        *id, version, component.data);
                    if (!migrated) {
                        SceneDiagnostic diagnostic = error(
                            migrated.status.error == SourceRegistryError::MigrationFailed
                                ? "scene.component.migration_failed"
                                : "scene.component.unsupported_version",
                            ScenePhase::ComponentMigration,
                            componentPath + "/data", migrated.status.message,
                            *uuid, *id, version);
                        diagnostic.migrationFrom = version;
                        diagnostic.migrationTo = codec->currentSourceVersion;
                        result.diagnostics.push_back(std::move(diagnostic));
                        continue;
                    }
                    component.data = std::move(migrated.data);
                    component.version = codec->currentSourceVersion;
                    for (const SourceMigrationNotice& notice : migrated.notices) {
                        SceneDiagnostic diagnostic{
                            .severity = SceneDiagnosticSeverity::Warning,
                            .code = notice.code,
                            .phase = ScenePhase::ComponentMigration,
                            .entity = *uuid,
                            .component = *id,
                            .componentVersion = component.version,
                            .propertyPath = componentPath + "/data" +
                                notice.propertyPath,
                            .migrationFrom = version,
                            .migrationTo = codec->currentSourceVersion,
                            .message = notice.message,
                        };
                        result.diagnostics.push_back(std::move(diagnostic));
                    }
                    std::string validationError;
                    if (!codec->validateLocal(component.data, validationError)) {
                        result.diagnostics.push_back(error(
                            "scene.component.validation_failed",
                            ScenePhase::Deserialize, componentPath + "/data",
                            validationError.empty()
                                ? "Component-local validation failed"
                                : std::move(validationError),
                            *uuid, *id, component.version));
                        continue;
                    }
                    if (const RuntimeComponentDescriptor* descriptor =
                        runtimeRegistry.find(*id)) {
                        validateKnownProperties(component, *descriptor,
                            sourceRegistry, componentPath + "/data", *uuid,
                            result.diagnostics);
                    }
                }
                entity.components.push_back(std::move(component));
            }
            document.entities.push_back(std::move(entity));
        }

        const std::vector<size_t> canonicalOrder = validateAndOrderHierarchy(
            document, result.diagnostics, ScenePhase::Hierarchy);
        sortSceneDiagnostics(result.diagnostics);
        if (!hasSceneErrors(result.diagnostics)) {
            std::vector<SourceSceneEntity> orderedEntities;
            orderedEntities.reserve(document.entities.size());
            for (size_t index : canonicalOrder) {
                orderedEntities.push_back(std::move(document.entities[index]));
            }
            document.entities = std::move(orderedEntities);
            result.document = std::move(document);
        }
        return result;
    }

    SourceSceneWriteResult writeSourceSceneCanonical(
        const SourceSceneDocument& document,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry) {
        SourceSceneWriteResult result;
        if (!runtimeRegistry.isFrozen() || !sourceRegistry.isFrozen()) {
            result.diagnostics.push_back(error(
                "scene.registry.not_frozen", ScenePhase::Save, {},
                "Runtime and source component registries must be frozen"));
            return result;
        }
        const std::vector<size_t> entityOrder = validateAndOrderHierarchy(
            document, result.diagnostics, ScenePhase::Save);
        for (const SourceSceneEntity& entity : document.entities) {
            for (const SourceSceneComponent& component : entity.components) {
                if (!component.known) continue;
                if (const RuntimeComponentDescriptor* descriptor =
                    runtimeRegistry.find(component.id)) {
                    validateKnownProperties(component, *descriptor,
                        sourceRegistry, "/entities/" + entity.uuid.toString() +
                            "/components/" + component.id.value() + "/data",
                        entity.uuid, result.diagnostics);
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        SourceJson root = SourceJson::object();
        root["format"] = kSourceSceneFormat;
        root["schemaVersion"] = kCurrentSourceSceneSchemaVersion;
        root["name"] = document.name;
        root["entities"] = SourceJson::array();

        for (size_t entityIndex : entityOrder) {
            const SourceSceneEntity& entity = document.entities[entityIndex];
            SourceJson entityJson = SourceJson::object();
            entityJson["uuid"] = entity.uuid.toString();
            entityJson["components"] = SourceJson::array();

            std::vector<const SourceSceneComponent*> components;
            components.reserve(entity.components.size());
            std::unordered_set<std::string> componentIds;
            for (const SourceSceneComponent& component : entity.components) {
                if (!componentIds.insert(component.id.value()).second) {
                    result.diagnostics.push_back(error(
                        "scene.component.duplicate_id", ScenePhase::Save,
                        "/entities/" + entity.uuid.toString() + "/components",
                        "Component ID is duplicated on this entity", entity.uuid,
                        component.id, component.version));
                }
                components.push_back(&component);
            }
            std::ranges::sort(components,
                [&](const SourceSceneComponent* lhs,
                    const SourceSceneComponent* rhs) {
                    const SourceComponentCodec* lhsCodec =
                        sourceRegistry.find(lhs->id);
                    const SourceComponentCodec* rhsCodec =
                        sourceRegistry.find(rhs->id);
                    if (static_cast<bool>(lhsCodec) != static_cast<bool>(rhsCodec)) {
                        return lhsCodec != nullptr;
                    }
                    if (lhsCodec && rhsCodec &&
                        lhsCodec->sourceOrder != rhsCodec->sourceOrder) {
                        return lhsCodec->sourceOrder < rhsCodec->sourceOrder;
                    }
                    return lhs->id < rhs->id;
                });

            for (const SourceSceneComponent* component : components) {
                SourceJson componentJson = SourceJson::object();
                componentJson["id"] = component->id.value();
                componentJson["version"] = component->version;
                componentJson["data"] = canonicalComponentData(
                    *component, runtimeRegistry, sourceRegistry);
                appendUnknownObject(componentJson,
                    component->unknownEnvelopeFields,
                    { "id", "version", "data" }, result.diagnostics,
                    "/entities/components");
                entityJson["components"].push_back(std::move(componentJson));
            }
            if (!entity.extensions.empty()) {
                entityJson["extensions"] = canonicalValue(entity.extensions);
            }
            appendUnknownObject(entityJson, entity.unknownFields,
                { "uuid", "components", "extensions" }, result.diagnostics,
                "/entities");
            root["entities"].push_back(std::move(entityJson));
        }

        if (!document.extensions.empty()) {
            root["extensions"] = canonicalValue(document.extensions);
        }
        appendUnknownObject(root, document.unknownFields,
            { "format", "schemaVersion", "name", "entities", "extensions" },
            result.diagnostics, {});
        sortSceneDiagnostics(result.diagnostics);
        if (!hasSceneErrors(result.diagnostics)) {
            result.bytes = root.dump(2, ' ', false,
                SourceJson::error_handler_t::strict) + "\n";
        }
        return result;
    }

} // namespace Iridium
