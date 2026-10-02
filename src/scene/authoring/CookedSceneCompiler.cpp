#include "scene/authoring/CookedSceneCompiler.h"

#include "assets/cooker/CookKey.h"
#include "scene/runtime/CookedComponentIO.h"
#include "scene/runtime/CookedScene.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Iridium {
    namespace {

        constexpr uint32_t kEndianMarker = 0x01020304u;
        constexpr uint32_t kSceneHeaderSize = 112;
        constexpr uint32_t kTypeDirectoryRecordSize = 16;

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& bytes, Integer value) {
            using Unsigned = std::make_unsigned_t<Integer>;
            const Unsigned bits = static_cast<Unsigned>(value);
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bytes.push_back(static_cast<std::byte>(bits >> (index * 8)));
            }
        }

        void appendString(std::vector<std::byte>& bytes, std::string_view value) {
            appendInteger<uint32_t>(bytes, static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(),
                reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data() + value.size()));
        }

        [[nodiscard]] SceneDiagnostic cookError(std::string code,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt,
            std::optional<ComponentTypeId> component = std::nullopt) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = ScenePhase::Cook,
                .entity = entity,
                .component = std::move(component),
                .message = std::move(message),
            };
        }

        [[nodiscard]] bool canonicalHash(std::string_view hash) {
            return hash.size() == 64 && std::ranges::all_of(hash, [](char value) {
                return (value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f');
            });
        }

        [[nodiscard]] std::array<std::byte, 32> hashBytes(
            std::string_view hash) {
            const auto nibble = [](char value) -> uint8_t {
                return value <= '9' ? static_cast<uint8_t>(value - '0')
                    : static_cast<uint8_t>(value - 'a' + 10);
            };
            std::array<std::byte, 32> result{};
            for (size_t index = 0; index < result.size(); ++index) {
                result[index] = static_cast<std::byte>(
                    (nibble(hash[index * 2]) << 4) |
                    nibble(hash[index * 2 + 1]));
            }
            return result;
        }

        struct EntityCookRecord {
            const SourceSceneEntity* source = nullptr;
            Entity handle = NULL_ENTITY;
            std::optional<SceneEntityUuid> parent;
            int32_t siblingOrder = 0;
            std::vector<std::pair<uint32_t, uint32_t>> bindings;
        };

        [[nodiscard]] const SourceSceneComponent* findComponent(
            const SourceSceneEntity& entity, const ComponentTypeId& id) {
            const auto found = std::ranges::find_if(entity.components,
                [&](const SourceSceneComponent& component) {
                    return component.id == id;
                });
            return found == entity.components.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool validateStrictSource(
            const StagedSourceScene& source,
            const RuntimeComponentRegistry& runtimeRegistry,
            const ComponentSerializerRegistry& sourceRegistry,
            std::vector<SceneDiagnostic>& diagnostics) {
            for (const SourceSceneEntity& entity : source.document.entities) {
                for (const SourceSceneComponent& component : entity.components) {
                    const RuntimeComponentDescriptor* runtime =
                        runtimeRegistry.find(component.id);
                    const SourceComponentCodec* codec = sourceRegistry.find(
                        component.id);
                    if (!component.known || !runtime || !codec) {
                        diagnostics.push_back(cookError(
                            "scene.cook.unknown_component",
                            "Unknown source components cannot be cooked",
                            entity.uuid, component.id));
                        continue;
                    }
                    if (component.version != codec->currentSourceVersion) {
                        diagnostics.push_back(cookError(
                            "scene.cook.component_version",
                            "Component is not at the current source schema version",
                            entity.uuid, component.id));
                    }
                    if (!component.unknownEnvelopeFields.empty()) {
                        diagnostics.push_back(cookError(
                            "scene.cook.unknown_component_envelope",
                            "Unknown component envelope fields cannot be cooked",
                            entity.uuid, component.id));
                    }
                    std::set<std::string> knownProperties;
                    for (const SourcePropertyBinding& property : codec->properties) {
                        knownProperties.insert(property.sourceName);
                    }
                    if (!component.data.is_object()) {
                        diagnostics.push_back(cookError(
                            "scene.cook.invalid_component_data",
                            "Cooked component source data must be an object",
                            entity.uuid, component.id));
                        continue;
                    }
                    for (const auto& [name, value] : component.data.items()) {
                        (void)value;
                        if (!knownProperties.contains(name)) {
                            diagnostics.push_back(cookError(
                                "scene.cook.unknown_property",
                                "Unknown source component properties cannot be cooked: " +
                                    name, entity.uuid, component.id));
                        }
                    }
                }
            }
            return !hasSceneErrors(diagnostics);
        }

        [[nodiscard]] std::vector<EntityCookRecord> canonicalEntities(
            const StagedSourceScene& source,
            std::vector<SceneDiagnostic>& diagnostics) {
            std::vector<EntityCookRecord> records;
            if (!source.world) {
                diagnostics.push_back(cookError("scene.cook.world_missing",
                    "Cook requires a validated staged source world"));
                return records;
            }
            const auto relationshipId = ComponentTypeId::parse(
                "iridium.component.relationship");
            std::unordered_map<SceneEntityUuid, size_t, SceneEntityUuidHash> byUuid;
            records.reserve(source.document.entities.size());
            for (const SourceSceneEntity& entity : source.document.entities) {
                const std::optional<Entity> handle =
                    source.world->identities().resolve(entity.uuid);
                if (!handle || !byUuid.emplace(entity.uuid, records.size()).second) {
                    diagnostics.push_back(cookError("scene.cook.identity_mismatch",
                        "Source document and staged identity map disagree", entity.uuid));
                    continue;
                }
                EntityCookRecord record{ .source = &entity, .handle = *handle };
                if (relationshipId) {
                    if (const SourceSceneComponent* relationship =
                        findComponent(entity, *relationshipId)) {
                        try {
                            const SourceJson& parent = relationship->data.at("parent");
                            if (!parent.is_null()) {
                                record.parent = SceneEntityUuid::parse(
                                    parent.get<std::string>());
                                if (!record.parent) throw std::runtime_error(
                                    "Relationship parent UUID is invalid");
                            }
                            const uint32_t sibling = relationship->data.value(
                                "siblingOrder", uint32_t{ 0 });
                            if (sibling > static_cast<uint32_t>(
                                    (std::numeric_limits<int32_t>::max)())) {
                                throw std::runtime_error(
                                    "Relationship sibling order exceeds int32");
                            }
                            record.siblingOrder = static_cast<int32_t>(sibling);
                        }
                        catch (const std::exception& exception) {
                            diagnostics.push_back(cookError(
                                "scene.cook.relationship_invalid", exception.what(),
                                entity.uuid, *relationshipId));
                        }
                    }
                }
                records.push_back(std::move(record));
            }
            if (hasSceneErrors(diagnostics)) return {};

            std::vector<std::vector<size_t>> children(records.size());
            std::vector<size_t> roots;
            for (size_t index = 0; index < records.size(); ++index) {
                if (!records[index].parent) roots.push_back(index);
                else {
                    const auto parent = byUuid.find(*records[index].parent);
                    if (parent == byUuid.end() || parent->second == index) {
                        diagnostics.push_back(cookError(
                            "scene.cook.parent_invalid",
                            "Relationship parent is missing or self-referential",
                            records[index].source->uuid, *relationshipId));
                    }
                    else children[parent->second].push_back(index);
                }
            }
            if (hasSceneErrors(diagnostics)) return {};
            const auto less = [&](size_t lhs, size_t rhs) {
                if (records[lhs].siblingOrder != records[rhs].siblingOrder) {
                    return records[lhs].siblingOrder < records[rhs].siblingOrder;
                }
                return records[lhs].source->uuid < records[rhs].source->uuid;
            };
            std::ranges::sort(roots, less);
            for (auto& values : children) std::ranges::sort(values, less);

            std::vector<EntityCookRecord> ordered;
            ordered.reserve(records.size());
            std::vector<uint8_t> visited(records.size(), 0);
            const auto visit = [&](auto&& self, size_t index) -> void {
                if (visited[index] != 0) return;
                visited[index] = 1;
                ordered.push_back(std::move(records[index]));
                for (size_t child : children[index]) self(self, child);
            };
            for (size_t root : roots) visit(visit, root);
            if (ordered.size() != records.size()) {
                diagnostics.push_back(cookError("scene.cook.hierarchy_cycle",
                    "Scene hierarchy contains a cycle"));
                return {};
            }
            return ordered;
        }

    } // namespace

    CookedSceneCompileResult compileCookedScene(
        const StagedSourceScene& source,
        const RuntimeComponentRegistry& runtimeRegistry,
        const ComponentSerializerRegistry& sourceRegistry,
        CookedSceneCompileInput input) {
        CookedSceneCompileResult result;
        if (!runtimeRegistry.isFrozen() || !sourceRegistry.isFrozen()) {
            result.diagnostics.push_back(cookError("scene.cook.registry_not_frozen",
                "Scene component registries must be frozen before cooking"));
            return result;
        }
        if (input.sceneAssetGuid.isNil() ||
            input.sourceSceneSchemaVersion != kCurrentSourceSceneSchemaVersion ||
            input.cookPolicy != "strict" ||
            !canonicalHash(input.sourceContentHash) ||
            !canonicalHash(input.canonicalContentHash)) {
            result.diagnostics.push_back(cookError("scene.cook.input_invalid",
                "Cook identity, hashes, schema, or strict policy are invalid"));
            return result;
        }
        const std::string manifestHash =
            runtimeComponentManifestHash(runtimeRegistry);
        if (!canonicalHash(manifestHash) ||
            !validateStrictSource(source, runtimeRegistry, sourceRegistry,
                result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::ranges::sort(input.dependencies);
        std::unordered_map<AssetGuid, uint32_t, AssetGuidHash> dependencyIndices;
        for (uint32_t index = 0; index < input.dependencies.size(); ++index) {
            const AssetDependency& dependency = input.dependencies[index];
            if ((dependency.type != AssetDependencyType::Asset &&
                    dependency.type != AssetDependencyType::OptionalAsset) ||
                !dependency.assetGuid || dependency.assetGuid->isNil() ||
                !canonicalHash(dependency.artifactHash) ||
                !dependencyIndices.emplace(*dependency.assetGuid, index).second) {
                result.diagnostics.push_back(cookError(
                    "scene.cook.dependency_invalid",
                    "Scene dependencies must be unique cooked asset GUIDs with hashes"));
            }
        }
        std::vector<EntityCookRecord> entities = canonicalEntities(
            source, result.diagnostics);
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::unordered_map<Entity, uint32_t> entityIndices;
        for (uint32_t index = 0; index < entities.size(); ++index) {
            entityIndices.emplace(entities[index].handle, index);
        }
        std::vector<std::string> strings;
        std::map<std::string, uint32_t, std::less<>> stringIndices;
        const auto intern = [&](std::string_view value) -> uint32_t {
            const auto found = stringIndices.find(value);
            if (found != stringIndices.end()) return found->second;
            const uint32_t index = static_cast<uint32_t>(strings.size());
            strings.emplace_back(value);
            stringIndices.emplace(strings.back(), index);
            return index;
        };
        for (const RuntimeComponentDescriptor& descriptor :
            runtimeRegistry.descriptors()) intern(descriptor.id.value());

        std::vector<CookSection> componentSections;
        std::vector<uint32_t> componentRecordCounts;
        uint32_t typeIndex = 0;
        for (const RuntimeComponentDescriptor& descriptor :
            runtimeRegistry.descriptors()) {
            std::vector<std::byte> sectionBytes;
            appendInteger<uint32_t>(sectionBytes, 0);
            uint32_t recordCount = 0;
            for (uint32_t entityIndex = 0; entityIndex < entities.size();
                ++entityIndex) {
                if (!findComponent(*entities[entityIndex].source, descriptor.id)) {
                    continue;
                }
                std::vector<std::byte> payload;
                CookedComponentWriter writer(payload, {
                    .internString = intern,
                    .entityIndex = [&](Entity value) -> std::optional<uint32_t> {
                        const auto found = entityIndices.find(value);
                        return found == entityIndices.end()
                            ? std::nullopt
                            : std::optional<uint32_t>(found->second);
                    },
                    .dependencyIndex = [&](AssetGuid value)
                        -> std::optional<uint32_t> {
                        const auto found = dependencyIndices.find(value);
                        return found == dependencyIndices.end()
                            ? std::nullopt
                            : std::optional<uint32_t>(found->second);
                    },
                });
                bool encoded = false;
                try {
                    encoded = descriptor.encodeCooked(source.world->registry(),
                        entities[entityIndex].handle, writer);
                }
                catch (const std::exception& exception) {
                    result.diagnostics.push_back(cookError(
                        "scene.cook.component_exception", exception.what(),
                        entities[entityIndex].source->uuid, descriptor.id));
                }
                if (!encoded || !writer.valid() ||
                    payload.size() > (std::numeric_limits<uint32_t>::max)()) {
                    result.diagnostics.push_back(cookError(
                        "scene.cook.component_encode",
                        writer.error().empty()
                            ? "Cooked component encoding failed"
                            : writer.error(), entities[entityIndex].source->uuid,
                        descriptor.id));
                    continue;
                }
                appendInteger(sectionBytes, entityIndex);
                appendInteger<uint32_t>(sectionBytes,
                    static_cast<uint32_t>(payload.size()));
                sectionBytes.insert(sectionBytes.end(), payload.begin(), payload.end());
                entities[entityIndex].bindings.emplace_back(typeIndex, recordCount);
                ++recordCount;
            }
            for (size_t index = 0; index < sizeof(uint32_t); ++index) {
                sectionBytes[index] = static_cast<std::byte>(
                    recordCount >> (index * 8));
            }
            componentRecordCounts.push_back(recordCount);
            componentSections.push_back({
                .id = descriptor.cookedSectionId.value(),
                .schemaVersion = descriptor.currentCookedVersion,
                .alignment = 8,
                .bytes = std::move(sectionBytes),
            });
            ++typeIndex;
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::vector<std::byte> stringBytes;
        appendInteger<uint32_t>(stringBytes,
            static_cast<uint32_t>(strings.size()));
        for (const std::string& value : strings) appendString(stringBytes, value);

        std::vector<std::byte> entityBytes;
        uint32_t bindingCount = 0;
        for (const EntityCookRecord& entity : entities) {
            bindingCount += static_cast<uint32_t>(entity.bindings.size());
        }
        appendInteger<uint32_t>(entityBytes,
            static_cast<uint32_t>(entities.size()));
        appendInteger(entityBytes, bindingCount);
        uint32_t bindingStart = 0;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash> uuidIndices;
        for (uint32_t index = 0; index < entities.size(); ++index) {
            uuidIndices.emplace(entities[index].source->uuid, index);
        }
        for (const EntityCookRecord& entity : entities) {
            for (uint8_t value : entity.source->uuid.bytes()) {
                entityBytes.push_back(static_cast<std::byte>(value));
            }
            appendInteger(entityBytes, entity.parent
                ? uuidIndices.at(*entity.parent)
                : kNullCookedSceneIndex);
            appendInteger(entityBytes, entity.siblingOrder);
            appendInteger(entityBytes, bindingStart);
            appendInteger<uint32_t>(entityBytes,
                static_cast<uint32_t>(entity.bindings.size()));
            bindingStart += static_cast<uint32_t>(entity.bindings.size());
        }
        for (const EntityCookRecord& entity : entities) {
            for (const auto& [type, record] : entity.bindings) {
                appendInteger(entityBytes, type);
                appendInteger(entityBytes, record);
            }
        }

        const uint64_t headerBytesSize = kSceneHeaderSize +
            runtimeRegistry.descriptors().size() * kTypeDirectoryRecordSize;
        uint64_t totalDecodedSize = headerBytesSize + stringBytes.size() +
            entityBytes.size();
        for (const CookSection& section : componentSections) {
            totalDecodedSize += section.bytes.size();
        }
        std::vector<std::byte> sceneHeader;
        appendInteger(sceneHeader, kCookedSceneHeaderSection);
        appendInteger(sceneHeader, kRuntimeSceneSchemaVersion);
        appendInteger(sceneHeader, kEndianMarker);
        appendInteger(sceneHeader, kSceneHeaderSize);
        for (uint8_t value : input.sceneAssetGuid.bytes()) {
            sceneHeader.push_back(static_cast<std::byte>(value));
        }
        appendInteger(sceneHeader, totalDecodedSize);
        appendInteger<uint32_t>(sceneHeader,
            static_cast<uint32_t>(entities.size()));
        appendInteger<uint32_t>(sceneHeader,
            static_cast<uint32_t>(runtimeRegistry.descriptors().size()));
        appendInteger<uint32_t>(sceneHeader,
            static_cast<uint32_t>(strings.size()));
        appendInteger<uint32_t>(sceneHeader,
            static_cast<uint32_t>(input.dependencies.size()));
        appendInteger<uint64_t>(sceneHeader, kSceneHeaderSize);
        appendInteger<uint64_t>(sceneHeader,
            runtimeRegistry.descriptors().size() * kTypeDirectoryRecordSize);
        const auto manifestBytes = hashBytes(manifestHash);
        sceneHeader.insert(sceneHeader.end(), manifestBytes.begin(),
            manifestBytes.end());
        appendInteger<uint64_t>(sceneHeader, 0);
        for (uint32_t index = 0; index < runtimeRegistry.descriptors().size();
            ++index) {
            const RuntimeComponentDescriptor& descriptor =
                runtimeRegistry.descriptors()[index];
            appendInteger(sceneHeader, intern(descriptor.id.value()));
            appendInteger(sceneHeader, descriptor.cookedSectionId.value());
            appendInteger(sceneHeader, descriptor.currentCookedVersion);
            appendInteger(sceneHeader, componentRecordCounts[index]);
        }

        std::vector<std::byte> canonicalSettings;
        appendInteger(canonicalSettings, input.sourceSceneSchemaVersion);
        appendInteger(canonicalSettings, input.compilerImplementationVersion);
        appendString(canonicalSettings, input.canonicalContentHash);
        appendString(canonicalSettings, manifestHash);
        appendString(canonicalSettings, input.cookPolicy);
        for (const RuntimeComponentDescriptor& descriptor :
            runtimeRegistry.descriptors()) {
            const SourceComponentCodec* codec = sourceRegistry.find(descriptor.id);
            appendString(canonicalSettings, descriptor.id.value());
            appendInteger(canonicalSettings, codec->currentSourceVersion);
            appendInteger(canonicalSettings, descriptor.currentCookedVersion);
        }
        const std::string cookKey = calculateCookKey({
            .assetGuid = input.sceneAssetGuid,
            .importerId = "iridium.scene",
            .importerImplementationVersion = input.compilerImplementationVersion,
            .settingsSchemaVersion = input.sourceSceneSchemaVersion,
            .canonicalSettings = canonicalSettings,
            .sourceContentHash = input.sourceContentHash,
            .dependencies = input.dependencies,
            .target = input.target,
            .cookerFeatureVersion = input.cookerFeatureVersion,
        });

        CookedArtifact artifact{
            .assetGuid = input.sceneAssetGuid,
            .artifactType = std::string(kRuntimeSceneArtifactType),
            .artifactSchemaVersion = kRuntimeSceneSchemaVersion,
            .target = std::move(input.target),
            .cookKey = cookKey,
            .dependencies = std::move(input.dependencies),
        };
        artifact.sections.push_back({ kCookedSceneHeaderSection, 1, 8,
            std::move(sceneHeader) });
        artifact.sections.push_back({ kCookedSceneStringSection, 1, 8,
            std::move(stringBytes) });
        artifact.sections.push_back({ kCookedSceneEntitySection, 1, 8,
            std::move(entityBytes) });
        artifact.sections.insert(artifact.sections.end(),
            std::make_move_iterator(componentSections.begin()),
            std::make_move_iterator(componentSections.end()));
        result.artifact = std::move(artifact);
        return result;
    }

} // namespace Iridium
