#include "scene/runtime/CookedScene.h"

#include "assets/cooker/CookedArtifact.h"
#include "scene/runtime/CookedComponentIO.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace Iridium {
    namespace {

        constexpr uint32_t kEndianMarker = 0x01020304u;
        constexpr uint32_t kSceneHeaderSize = 112;
        constexpr uint32_t kTypeDirectoryRecordSize = 16;

        [[nodiscard]] SceneDiagnostic loadError(std::string code,
            std::string message,
            std::optional<SceneEntityUuid> entity = std::nullopt,
            std::optional<ComponentTypeId> component = std::nullopt) {
            return {
                .severity = SceneDiagnosticSeverity::Error,
                .code = std::move(code),
                .phase = ScenePhase::RuntimeLoad,
                .entity = entity,
                .component = std::move(component),
                .message = std::move(message),
            };
        }

        class ByteReader {
        public:
            explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

            template <typename Integer>
            [[nodiscard]] bool integer(Integer& value) {
                static_assert(std::is_integral_v<Integer>);
                if (offset_ + sizeof(Integer) > bytes_.size()) return false;
                using Unsigned = std::make_unsigned_t<Integer>;
                Unsigned bits = 0;
                for (size_t index = 0; index < sizeof(Integer); ++index) {
                    bits |= static_cast<Unsigned>(std::to_integer<uint8_t>(
                        bytes_[offset_ + index])) << (index * 8);
                }
                offset_ += sizeof(Integer);
                value = static_cast<Integer>(bits);
                return true;
            }

            [[nodiscard]] bool bytes(size_t count,
                std::span<const std::byte>& value) {
                if (count > remaining()) return false;
                value = bytes_.subspan(offset_, count);
                offset_ += count;
                return true;
            }

            [[nodiscard]] bool seek(size_t offset) {
                if (offset > bytes_.size()) return false;
                offset_ = offset;
                return true;
            }

            [[nodiscard]] size_t offset() const noexcept { return offset_; }
            [[nodiscard]] size_t remaining() const noexcept {
                return offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0;
            }
            [[nodiscard]] bool finished() const noexcept {
                return offset_ == bytes_.size();
            }

        private:
            std::span<const std::byte> bytes_;
            size_t offset_ = 0;
        };

        [[nodiscard]] std::string hashText(std::span<const std::byte> bytes) {
            constexpr char hex[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (std::byte value : bytes) {
                const uint8_t byte = std::to_integer<uint8_t>(value);
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
            }
            return result;
        }

        [[nodiscard]] bool validUtf8(std::string_view value) {
            size_t index = 0;
            while (index < value.size()) {
                const uint8_t lead = static_cast<uint8_t>(value[index]);
                if (lead <= 0x7f) {
                    if (lead == 0) return false;
                    ++index;
                    continue;
                }
                size_t count = 0;
                uint32_t codepoint = 0;
                if ((lead & 0xe0) == 0xc0) { count = 2; codepoint = lead & 0x1f; }
                else if ((lead & 0xf0) == 0xe0) {
                    count = 3; codepoint = lead & 0x0f;
                }
                else if ((lead & 0xf8) == 0xf0) {
                    count = 4; codepoint = lead & 0x07;
                }
                else return false;
                if (index + count > value.size()) return false;
                for (size_t part = 1; part < count; ++part) {
                    const uint8_t continuation =
                        static_cast<uint8_t>(value[index + part]);
                    if ((continuation & 0xc0) != 0x80) return false;
                    codepoint = (codepoint << 6) | (continuation & 0x3f);
                }
                if ((count == 2 && codepoint < 0x80) ||
                    (count == 3 && codepoint < 0x800) ||
                    (count == 4 && codepoint < 0x10000) ||
                    codepoint > 0x10ffff ||
                    (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
                index += count;
            }
            return true;
        }

        [[nodiscard]] bool canonicalHash(std::string_view hash) {
            return hash.size() == 64 && std::ranges::all_of(hash, [](char value) {
                return (value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f');
            });
        }

        struct TypeRecord {
            uint32_t stringIndex = 0;
            uint32_t sectionId = 0;
            uint32_t version = 0;
            uint32_t recordCount = 0;
        };

        struct EntityRecord {
            SceneEntityUuid uuid;
            uint32_t parent = kNullCookedSceneIndex;
            int32_t siblingOrder = 0;
            uint32_t bindingStart = 0;
            uint32_t bindingCount = 0;
        };

        struct ComponentBinding {
            uint32_t type = 0;
            uint32_t record = 0;
        };

        struct ComponentRecord {
            uint32_t owner = 0;
            std::span<const std::byte> payload;
        };

        [[nodiscard]] const CookSection* section(
            const std::map<uint32_t, const CookSection*>& sections,
            uint32_t id) {
            const auto found = sections.find(id);
            return found == sections.end() ? nullptr : found->second;
        }

    } // namespace

    CookedSceneStageResult stageCookedScene(
        std::span<const std::byte> bytes,
        const RuntimeComponentRegistry& registry,
        CookedSceneLoadOptions options) {
        CookedSceneStageResult result;
        if (!registry.isFrozen()) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.registry_not_frozen",
                "Runtime component registry must be frozen before loading"));
            return result;
        }
        const CookedArtifactReadResult read = readCookedArtifact(bytes,
            options.expectedArtifactHash);
        if (!read.valid()) {
            for (const CookDiagnostic& diagnostic : read.diagnostics) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.artifact_" + diagnostic.code,
                    diagnostic.message));
            }
            if (result.diagnostics.empty()) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.artifact_invalid",
                    "Cooked artifact validation failed"));
            }
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }
        const CookedArtifact& artifact = *read.artifact;
        const bool targetMatches = !options.expectedTarget ||
            (artifact.target.platform == options.expectedTarget->platform &&
                artifact.target.profile == options.expectedTarget->profile &&
                artifact.target.artifactContainerVersion ==
                    options.expectedTarget->artifactContainerVersion &&
                artifact.target.materialSchemaVersion ==
                    options.expectedTarget->materialSchemaVersion);
        if (artifact.artifactType != kRuntimeSceneArtifactType ||
            artifact.artifactSchemaVersion != kRuntimeSceneSchemaVersion ||
            (options.expectedSceneAssetGuid &&
                artifact.assetGuid != *options.expectedSceneAssetGuid) ||
            (options.expectedCookKey &&
                artifact.cookKey != *options.expectedCookKey) ||
            !targetMatches) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.outer_identity",
                "Cooked scene artifact identity, schema, or target does not match"));
            return result;
        }

        std::unordered_map<AssetGuid, const AssetDependency*, AssetGuidHash>
            dependencies;
        if (!std::ranges::is_sorted(artifact.dependencies)) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.dependency_order",
                "Cooked scene dependency table is not in canonical order"));
        }
        for (const AssetDependency& dependency : artifact.dependencies) {
            if ((dependency.type != AssetDependencyType::Asset &&
                    dependency.type != AssetDependencyType::OptionalAsset) ||
                !dependency.assetGuid || dependency.assetGuid->isNil() ||
                !canonicalHash(dependency.artifactHash) ||
                !dependencies.emplace(*dependency.assetGuid, &dependency).second) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.dependency_invalid",
                    "Cooked scene dependencies must be unique asset GUIDs with artifact hashes"));
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::map<uint32_t, const CookSection*> sections;
        uint64_t totalDecodedSize = 0;
        for (const CookSection& value : artifact.sections) {
            totalDecodedSize += value.bytes.size();
            if (value.alignment != 8 || !sections.emplace(value.id, &value).second) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.section_contract",
                    "Cooked scene sections must be unique and 8-byte aligned"));
            }
        }
        const CookSection* headerSection = section(sections,
            kCookedSceneHeaderSection);
        const CookSection* stringSection = section(sections,
            kCookedSceneStringSection);
        const CookSection* entitySection = section(sections,
            kCookedSceneEntitySection);
        if (!headerSection || !stringSection || !entitySection ||
            headerSection->schemaVersion != 1 ||
            stringSection->schemaVersion != 1 ||
            entitySection->schemaVersion != 1) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.required_section",
                "Cooked scene is missing a supported SCN1, STR1, or ENT1 section"));
        }
        if (hasSceneErrors(result.diagnostics)) return result;

        ByteReader header(headerSection->bytes);
        uint32_t magic = 0;
        uint32_t schema = 0;
        uint32_t endian = 0;
        uint32_t headerSize = 0;
        std::span<const std::byte> sceneGuidBytes;
        uint64_t declaredDecodedSize = 0;
        uint32_t entityCount = 0;
        uint32_t typeCount = 0;
        uint32_t stringCount = 0;
        uint32_t dependencyCount = 0;
        uint64_t typeDirectoryOffset = 0;
        uint64_t typeDirectorySize = 0;
        std::span<const std::byte> manifestBytes;
        uint64_t reserved = 1;
        if (!header.integer(magic) || !header.integer(schema) ||
            !header.integer(endian) || !header.integer(headerSize) ||
            !header.bytes(16, sceneGuidBytes) ||
            !header.integer(declaredDecodedSize) ||
            !header.integer(entityCount) || !header.integer(typeCount) ||
            !header.integer(stringCount) || !header.integer(dependencyCount) ||
            !header.integer(typeDirectoryOffset) ||
            !header.integer(typeDirectorySize) ||
            !header.bytes(32, manifestBytes) || !header.integer(reserved) ||
            magic != kCookedSceneHeaderSection ||
            schema != kRuntimeSceneSchemaVersion || endian != kEndianMarker ||
            headerSize != kSceneHeaderSize || reserved != 0 ||
            declaredDecodedSize != totalDecodedSize ||
            typeCount != registry.descriptors().size() ||
            dependencyCount != artifact.dependencies.size() ||
            typeDirectoryOffset != kSceneHeaderSize ||
            typeDirectorySize != static_cast<uint64_t>(typeCount) *
                kTypeDirectoryRecordSize ||
            headerSection->bytes.size() != typeDirectoryOffset +
                typeDirectorySize ||
            hashText(manifestBytes) != runtimeComponentManifestHash(registry)) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.header_invalid",
                "SCN1 header, size, endian marker, or registry manifest is invalid"));
            return result;
        }

        std::set<uint32_t> expectedSectionIds{
            kCookedSceneHeaderSection,
            kCookedSceneStringSection,
            kCookedSceneEntitySection,
        };
        for (const RuntimeComponentDescriptor& descriptor :
            registry.descriptors()) {
            expectedSectionIds.insert(descriptor.cookedSectionId.value());
        }
        if (sections.size() != expectedSectionIds.size() ||
            !std::ranges::all_of(sections, [&](const auto& entry) {
                return expectedSectionIds.contains(entry.first);
            })) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.unknown_section",
                "Cooked scene contains an unregistered or missing section"));
            return result;
        }
        AssetGuid::Bytes guidBytes{};
        for (size_t index = 0; index < guidBytes.size(); ++index) {
            guidBytes[index] = std::to_integer<uint8_t>(sceneGuidBytes[index]);
        }
        const AssetGuid sceneAssetGuid(guidBytes);
        if (sceneAssetGuid.isNil() || sceneAssetGuid != artifact.assetGuid) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.scene_guid",
                "SCN1 scene asset GUID does not match the outer artifact"));
            return result;
        }

        std::vector<TypeRecord> types(typeCount);
        for (TypeRecord& type : types) {
            if (!header.integer(type.stringIndex) ||
                !header.integer(type.sectionId) ||
                !header.integer(type.version) ||
                !header.integer(type.recordCount)) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.type_directory",
                    "SCN1 type directory is truncated"));
                return result;
            }
        }
        if (!header.finished()) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.header_trailing",
                "SCN1 contains unexpected trailing bytes"));
            return result;
        }

        std::vector<std::string> strings;
        ByteReader stringReader(stringSection->bytes);
        uint32_t encodedStringCount = 0;
        if (!stringReader.integer(encodedStringCount) ||
            encodedStringCount != stringCount ||
            encodedStringCount > stringReader.remaining() / sizeof(uint32_t)) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.string_count", "STR1 count does not match SCN1"));
            return result;
        }
        strings.reserve(stringCount);
        for (uint32_t index = 0; index < stringCount; ++index) {
            uint32_t length = 0;
            std::span<const std::byte> encoded;
            if (!stringReader.integer(length) ||
                !stringReader.bytes(length, encoded)) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.string_range", "STR1 string is out of range"));
                return result;
            }
            std::string value(reinterpret_cast<const char*>(encoded.data()),
                encoded.size());
            if (!validUtf8(value)) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.string_utf8", "STR1 contains invalid UTF-8"));
                return result;
            }
            strings.push_back(std::move(value));
        }
        if (!stringReader.finished()) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.string_trailing",
                "STR1 contains unexpected trailing bytes"));
            return result;
        }

        for (uint32_t index = 0; index < typeCount; ++index) {
            const RuntimeComponentDescriptor& descriptor =
                registry.descriptors()[index];
            const TypeRecord& type = types[index];
            const CookSection* componentSection = section(sections, type.sectionId);
            if (type.stringIndex >= strings.size() ||
                strings[type.stringIndex] != descriptor.id.value() ||
                type.sectionId != descriptor.cookedSectionId.value() ||
                type.version != descriptor.currentCookedVersion ||
                !componentSection ||
                componentSection->schemaVersion != type.version) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.type_contract",
                    "Cooked type directory does not match the frozen registry",
                    std::nullopt, descriptor.id));
            }
        }
        if (hasSceneErrors(result.diagnostics)) return result;

        ByteReader entityReader(entitySection->bytes);
        uint32_t encodedEntityCount = 0;
        uint32_t bindingCount = 0;
        if (!entityReader.integer(encodedEntityCount) ||
            !entityReader.integer(bindingCount) ||
            encodedEntityCount != entityCount ||
            encodedEntityCount > entityReader.remaining() / 32 ||
            static_cast<uint64_t>(encodedEntityCount) * 32 +
                static_cast<uint64_t>(bindingCount) * 8 !=
                    entityReader.remaining()) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.entity_count", "ENT1 count does not match SCN1"));
            return result;
        }
        std::vector<EntityRecord> entities;
        entities.reserve(entityCount);
        std::unordered_set<SceneEntityUuid, SceneEntityUuidHash> uniqueUuids;
        uint32_t expectedBindingStart = 0;
        for (uint32_t index = 0; index < entityCount; ++index) {
            std::span<const std::byte> uuidBytes;
            EntityRecord entity;
            if (!entityReader.bytes(16, uuidBytes) ||
                !entityReader.integer(entity.parent) ||
                !entityReader.integer(entity.siblingOrder) ||
                !entityReader.integer(entity.bindingStart) ||
                !entityReader.integer(entity.bindingCount)) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.entity_record", "ENT1 entity record is truncated"));
                return result;
            }
            SceneEntityUuid::Bytes uuid{};
            for (size_t byte = 0; byte < uuid.size(); ++byte) {
                uuid[byte] = std::to_integer<uint8_t>(uuidBytes[byte]);
            }
            entity.uuid = SceneEntityUuid(uuid);
            const uint64_t bindingEnd = static_cast<uint64_t>(
                entity.bindingStart) + entity.bindingCount;
            if (!entity.uuid.isSupported() ||
                !uniqueUuids.insert(entity.uuid).second ||
                (entity.parent != kNullCookedSceneIndex && entity.parent >= index) ||
                entity.siblingOrder < 0 ||
                entity.bindingStart != expectedBindingStart ||
                bindingEnd > bindingCount) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.entity_invalid",
                    "ENT1 contains an invalid UUID, parent, order, or binding range",
                    entity.uuid));
            }
            expectedBindingStart += entity.bindingCount;
            entities.push_back(entity);
        }
        if (expectedBindingStart != bindingCount) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.binding_count",
                "ENT1 component binding ranges do not cover the binding table"));
        }
        std::vector<ComponentBinding> bindings(bindingCount);
        for (ComponentBinding& binding : bindings) {
            if (!entityReader.integer(binding.type) ||
                !entityReader.integer(binding.record)) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.binding_truncated",
                    "ENT1 component binding table is truncated"));
                return result;
            }
        }
        if (!entityReader.finished() || hasSceneErrors(result.diagnostics)) {
            if (!entityReader.finished()) result.diagnostics.push_back(loadError(
                "scene.runtime.entity_trailing",
                "ENT1 contains unexpected trailing bytes"));
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::vector<std::vector<ComponentRecord>> componentRecords(typeCount);
        for (uint32_t typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
            const CookSection* value = section(sections, types[typeIndex].sectionId);
            ByteReader records(value->bytes);
            uint32_t count = 0;
            if (!records.integer(count) || count != types[typeIndex].recordCount ||
                count > records.remaining() / 8) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.component_count",
                    "Cooked component record count does not match SCN1",
                    std::nullopt, registry.descriptors()[typeIndex].id));
                continue;
            }
            componentRecords[typeIndex].reserve(count);
            std::unordered_set<uint32_t> owners;
            for (uint32_t recordIndex = 0; recordIndex < count; ++recordIndex) {
                uint32_t owner = 0;
                uint32_t size = 0;
                std::span<const std::byte> payload;
                if (!records.integer(owner) || !records.integer(size) ||
                    !records.bytes(size, payload) || owner >= entityCount ||
                    !owners.insert(owner).second) {
                    result.diagnostics.push_back(loadError(
                        "scene.runtime.component_record",
                        "Cooked component record has an invalid owner or range",
                        std::nullopt, registry.descriptors()[typeIndex].id));
                    break;
                }
                componentRecords[typeIndex].push_back({ owner, payload });
            }
            if (!records.finished()) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.component_trailing",
                    "Cooked component section contains trailing bytes",
                    std::nullopt, registry.descriptors()[typeIndex].id));
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        std::vector<std::vector<bool>> bound(typeCount);
        for (uint32_t type = 0; type < typeCount; ++type) {
            bound[type].resize(componentRecords[type].size(), false);
        }
        for (uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
            std::set<uint32_t> entityTypes;
            const EntityRecord& entity = entities[entityIndex];
            for (uint32_t offset = 0; offset < entity.bindingCount; ++offset) {
                const ComponentBinding& binding =
                    bindings[entity.bindingStart + offset];
                if (binding.type >= typeCount ||
                    binding.record >= componentRecords[binding.type].size() ||
                    componentRecords[binding.type][binding.record].owner != entityIndex ||
                    bound[binding.type][binding.record] ||
                    !entityTypes.insert(binding.type).second) {
                    result.diagnostics.push_back(loadError(
                        "scene.runtime.binding_invalid",
                        "ENT1 component binding is duplicated or out of range",
                        entity.uuid));
                    continue;
                }
                bound[binding.type][binding.record] = true;
            }
        }
        for (const auto& values : bound) {
            if (std::ranges::find(values, false) != values.end()) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.binding_orphan",
                    "Cooked component record is not bound to its owner entity"));
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        auto staging = std::make_unique<StagedCookedScene>();
        staging->world = std::make_unique<SceneWorld>();
        staging->sceneAssetGuid = sceneAssetGuid;
        staging->artifactHash = read.artifactHash;
        std::vector<Entity> handles;
        handles.reserve(entityCount);
        try {
            for (const EntityRecord& entity : entities) {
                handles.push_back(staging->world->createEntity(entity.uuid));
            }
        }
        catch (const std::exception& exception) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.identity_creation", exception.what()));
            return result;
        }

        std::vector<SceneEntityUuid> entityUuids;
        entityUuids.reserve(entities.size());
        for (const EntityRecord& entity : entities) entityUuids.push_back(entity.uuid);
        for (uint32_t typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
            const RuntimeComponentDescriptor& descriptor =
                registry.descriptors()[typeIndex];
            for (const ComponentRecord& record : componentRecords[typeIndex]) {
                CookedComponentReader reader(record.payload, {
                    .strings = strings,
                    .entities = entityUuids,
                    .dependencies = artifact.dependencies,
                    .owner = entities[record.owner].uuid,
                    .component = descriptor.id,
                    .references = &staging->world->references(),
                });
                try {
                    if (!descriptor.decodeCooked(staging->world->registry(),
                            handles[record.owner], reader)) {
                        result.diagnostics.push_back(loadError(
                            "scene.runtime.component_decode",
                            reader.error().empty()
                                ? "Cooked component decoding failed"
                                : reader.error(), entities[record.owner].uuid,
                            descriptor.id));
                    }
                }
                catch (const std::exception& exception) {
                    result.diagnostics.push_back(loadError(
                        "scene.runtime.component_exception", exception.what(),
                        entities[record.owner].uuid, descriptor.id));
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (SceneReferenceRecord& reference :
            staging->world->references().records()) {
            if (reference.kind == StableReferenceKind::Entity) {
                reference.resolution = staging->world->identities().resolve(
                    SceneEntityUuid(reference.target))
                    ? StableReferenceResolution::Resolved
                    : StableReferenceResolution::Failed;
            }
            else {
                const AssetGuid guid(reference.target);
                const auto found = dependencies.find(guid);
                if (found == dependencies.end()) {
                    reference.resolution = StableReferenceResolution::Failed;
                }
                else {
                    const CookedAssetAvailability availability =
                        options.assetAvailability
                        ? options.assetAvailability(*found->second)
                        : CookedAssetAvailability::Pending;
                    reference.resolution = availability == CookedAssetAvailability::Available
                        ? StableReferenceResolution::Resolved
                        : availability == CookedAssetAvailability::Pending
                            ? StableReferenceResolution::Pending
                            : StableReferenceResolution::Failed;
                }
            }
            if (reference.required &&
                reference.resolution == StableReferenceResolution::Failed) {
                result.diagnostics.push_back(loadError(
                    "scene.runtime.required_reference_missing",
                    "Required cooked scene reference is unavailable",
                    reference.key.owner, reference.key.component));
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
            const EntityRecord& entity = entities[entityIndex];
            for (uint32_t offset = 0; offset < entity.bindingCount; ++offset) {
                const ComponentBinding& binding =
                    bindings[entity.bindingStart + offset];
                const RuntimeComponentDescriptor& descriptor =
                    registry.descriptors()[binding.type];
                if (!descriptor.resolveReferences(staging->world->registry(),
                        handles[entityIndex], staging->world->identities(),
                        staging->world->references())) {
                    result.diagnostics.push_back(loadError(
                        "scene.runtime.reference_callback",
                        "Cooked component reference resolution failed",
                        entity.uuid, descriptor.id));
                }
            }
        }
        if (hasSceneErrors(result.diagnostics)) {
            sortSceneDiagnostics(result.diagnostics);
            return result;
        }

        for (uint32_t entityIndex = 0; entityIndex < entityCount; ++entityIndex) {
            const EntityRecord& entity = entities[entityIndex];
            for (uint32_t offset = 0; offset < entity.bindingCount; ++offset) {
                const ComponentBinding& binding =
                    bindings[entity.bindingStart + offset];
                const RuntimeComponentDescriptor& descriptor =
                    registry.descriptors()[binding.type];
                if (!descriptor.postLoadValidate(staging->world->registry(),
                        handles[entityIndex])) {
                    result.diagnostics.push_back(loadError(
                        "scene.runtime.post_validation",
                        "Cooked component post-load validation failed",
                        entity.uuid, descriptor.id));
                }
            }
        }
        if (!staging->world->identities().validate(staging->world->registry())) {
            result.diagnostics.push_back(loadError(
                "scene.runtime.identity_validation",
                "Cooked scene identity map failed validation"));
        }
        sortSceneDiagnostics(result.diagnostics);
        if (!hasSceneErrors(result.diagnostics)) {
            result.staging = std::move(staging);
        }
        return result;
    }

    void commitStagedCookedScene(
        SceneWorld& active,
        StagedCookedScene& staging) {
        if (!staging.world) {
            throw std::invalid_argument("Cannot commit an empty cooked scene");
        }
        active.swapState(*staging.world);
    }

} // namespace Iridium
