#include "assets/AssetMetadata.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace Iridium {

    namespace {

        using OrderedJson = nlohmann::ordered_json;

        OrderedJson canonicalJson(const nlohmann::json& value) {
            if (value.is_object()) {
                OrderedJson result = OrderedJson::object();
                std::vector<std::string> keys;
                keys.reserve(value.size());
                for (const auto& [key, ignored] : value.items()) {
                    (void)ignored;
                    keys.push_back(key);
                }
                std::sort(keys.begin(), keys.end());
                for (const std::string& key : keys) {
                    result[key] = canonicalJson(value.at(key));
                }
                return result;
            }
            if (value.is_array()) {
                OrderedJson result = OrderedJson::array();
                for (const auto& item : value) result.push_back(canonicalJson(item));
                return result;
            }
            return value;
        }

        void addDiagnostic(AssetMetadataReadResult& result,
            AssetMetadataSeverity severity, std::string code,
            std::string field, std::string message) {
            result.diagnostics.push_back({
                .severity = severity,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        std::optional<AssetGuid> readGuid(AssetMetadataReadResult& result,
            const nlohmann::json& value, std::string_view field) {
            if (!value.is_string()) {
                addDiagnostic(result, AssetMetadataSeverity::Error, "META_GUID_TYPE",
                    std::string(field), "GUID must be a canonical UUID string.");
                return std::nullopt;
            }
            const std::string text = value.get<std::string>();
            const auto guid = AssetGuid::parse(text);
            if (!guid || guid->isNil() || guid->version() != 7 || !guid->hasRfc4122Variant()) {
                addDiagnostic(result, AssetMetadataSeverity::Error, "META_GUID_INVALID",
                    std::string(field), "GUID must be a non-nil RFC 9562 UUIDv7 value.");
                return std::nullopt;
            }
            if (guid->toString() != text) {
                addDiagnostic(result, AssetMetadataSeverity::Warning, "META_GUID_NONCANONICAL",
                    std::string(field), "GUID text will serialize in lower-case canonical form.");
            }
            return guid;
        }

        bool readRequiredString(AssetMetadataReadResult& result,
            const nlohmann::json& object, std::string_view key,
            std::string_view field, std::string& destination) {
            const auto found = object.find(key);
            if (found == object.end() || !found->is_string() ||
                found->get_ref<const std::string&>().empty()) {
                addDiagnostic(result, AssetMetadataSeverity::Error, "META_REQUIRED_STRING",
                    std::string(field), "A non-empty string is required.");
                return false;
            }
            destination = found->get<std::string>();
            return true;
        }

        bool readUint32(AssetMetadataReadResult& result, const nlohmann::json& object,
            std::string_view key, std::string_view field, uint32_t& destination) {
            const auto found = object.find(key);
            if (found == object.end() || !found->is_number_unsigned() ||
                found->get<uint64_t>() > UINT32_MAX) {
                addDiagnostic(result, AssetMetadataSeverity::Error, "META_UNSIGNED_INTEGER",
                    std::string(field), "An unsigned 32-bit integer is required.");
                return false;
            }
            destination = found->get<uint32_t>();
            return true;
        }

        bool atomicReplace(const std::filesystem::path& temporary,
            const std::filesystem::path& destination, std::string& error) {
#if defined(_WIN32)
            if (MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                return true;
            }
            error = "MoveFileExW failed with error " + std::to_string(GetLastError());
            return false;
#else
            if (std::rename(temporary.c_str(), destination.c_str()) == 0) return true;
            error = "rename failed";
            return false;
#endif
        }

    } // namespace

    bool AssetMetadataReadResult::hasErrors() const noexcept {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const AssetMetadataDiagnostic& diagnostic) {
                return diagnostic.severity == AssetMetadataSeverity::Error;
            });
    }

    std::filesystem::path assetMetadataSidecarPath(
        const std::filesystem::path& sourcePath) {
        return std::filesystem::path(sourcePath.native() +
            std::filesystem::path(".iridium.meta").native());
    }

    std::string serializeAssetMetadata(const AssetMetadata& metadata) {
        OrderedJson root = OrderedJson::object();
        root["schemaVersion"] = metadata.schemaVersion;
        root["assetGuid"] = metadata.assetGuid.toString();
        root["assetType"] = metadata.assetType;
        root["importer"] = OrderedJson{
            { "id", metadata.importerId },
            { "version", metadata.importerVersion },
        };
        root["settings"] = OrderedJson{
            { "schemaVersion", metadata.settingsSchemaVersion },
            { "values", canonicalJson(metadata.settings) },
        };

        std::vector<SubassetMetadata> subassets = metadata.subassets;
        std::sort(subassets.begin(), subassets.end(),
            [](const SubassetMetadata& lhs, const SubassetMetadata& rhs) {
                if (lhs.sourceKey != rhs.sourceKey) return lhs.sourceKey < rhs.sourceKey;
                return lhs.guid < rhs.guid;
            });
        root["subassets"] = OrderedJson::array();
        for (const SubassetMetadata& subasset : subassets) {
            root["subassets"].push_back(OrderedJson{
                { "guid", subasset.guid.toString() },
                { "assetType", subasset.assetType },
                { "sourceKey", subasset.sourceKey },
                { "structuralFingerprint", subasset.structuralFingerprint },
            });
        }

        std::vector<std::string> tags = metadata.tags;
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
        root["tags"] = tags;
        return root.dump(2) + "\n";
    }

    AssetMetadataReadResult parseAssetMetadata(std::string_view text) {
        AssetMetadataReadResult result;
        const nlohmann::json root = nlohmann::json::parse(
            text.begin(), text.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_JSON_INVALID",
                "", "Metadata sidecar is not a valid JSON object.");
            return result;
        }

        AssetMetadata metadata;
        readUint32(result, root, "schemaVersion", "schemaVersion", metadata.schemaVersion);
        if (metadata.schemaVersion != kAssetMetadataSchemaVersion) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_SCHEMA_UNSUPPORTED",
                "schemaVersion", "Only asset metadata schema version 1 is supported.");
        }

        const auto guidFound = root.find("assetGuid");
        if (guidFound == root.end()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_GUID_MISSING",
                "assetGuid", "The root asset GUID is required.");
        } else if (const auto guid = readGuid(result, *guidFound, "assetGuid")) {
            metadata.assetGuid = *guid;
        }
        readRequiredString(result, root, "assetType", "assetType", metadata.assetType);

        const auto importer = root.find("importer");
        if (importer == root.end() || !importer->is_object()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_IMPORTER_INVALID",
                "importer", "Importer must be an object.");
        } else {
            readRequiredString(result, *importer, "id", "importer.id", metadata.importerId);
            readUint32(result, *importer, "version", "importer.version",
                metadata.importerVersion);
        }

        const auto settings = root.find("settings");
        if (settings == root.end() || !settings->is_object()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_SETTINGS_INVALID",
                "settings", "Settings must be an object.");
        } else {
            readUint32(result, *settings, "schemaVersion", "settings.schemaVersion",
                metadata.settingsSchemaVersion);
            const auto values = settings->find("values");
            if (values == settings->end() || !values->is_object()) {
                addDiagnostic(result, AssetMetadataSeverity::Error, "META_SETTINGS_VALUES",
                    "settings.values", "Settings values must be an object.");
            } else {
                metadata.settings = *values;
            }
        }

        const auto subassets = root.find("subassets");
        if (subassets == root.end() || !subassets->is_array()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_SUBASSETS_INVALID",
                "subassets", "Subassets must be an array.");
        } else {
            std::set<AssetGuid> guids;
            std::set<std::string> sourceKeys;
            size_t index = 0;
            for (const auto& entry : *subassets) {
                const std::string prefix = "subassets[" + std::to_string(index) + "]";
                ++index;
                if (!entry.is_object()) {
                    addDiagnostic(result, AssetMetadataSeverity::Error, "META_SUBASSET_INVALID",
                        prefix, "Subasset entry must be an object.");
                    continue;
                }
                SubassetMetadata subasset;
                const auto subGuid = entry.find("guid");
                if (subGuid == entry.end()) {
                    addDiagnostic(result, AssetMetadataSeverity::Error, "META_GUID_MISSING",
                        prefix + ".guid", "Subasset GUID is required.");
                } else if (const auto guid = readGuid(result, *subGuid, prefix + ".guid")) {
                    subasset.guid = *guid;
                    if (!guids.insert(*guid).second || *guid == metadata.assetGuid) {
                        addDiagnostic(result, AssetMetadataSeverity::Error,
                            "META_SUBASSET_GUID_DUPLICATE", prefix + ".guid",
                            "Subasset GUID must be unique within the sidecar.");
                    }
                }
                readRequiredString(result, entry, "assetType", prefix + ".assetType",
                    subasset.assetType);
                readRequiredString(result, entry, "sourceKey", prefix + ".sourceKey",
                    subasset.sourceKey);
                if (!subasset.sourceKey.empty() &&
                    !sourceKeys.insert(subasset.sourceKey).second) {
                    addDiagnostic(result, AssetMetadataSeverity::Error,
                        "META_SUBASSET_KEY_DUPLICATE", prefix + ".sourceKey",
                        "Subasset source key must be unique within the sidecar.");
                }
                const auto fingerprint = entry.find("structuralFingerprint");
                if (fingerprint != entry.end()) {
                    if (!fingerprint->is_string()) {
                        addDiagnostic(result, AssetMetadataSeverity::Error,
                            "META_FINGERPRINT_TYPE", prefix + ".structuralFingerprint",
                            "Structural fingerprint must be a string.");
                    } else {
                        subasset.structuralFingerprint = fingerprint->get<std::string>();
                    }
                }
                metadata.subassets.push_back(std::move(subasset));
            }
        }

        const auto tags = root.find("tags");
        if (tags == root.end() || !tags->is_array()) {
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_TAGS_INVALID",
                "tags", "Tags must be an array.");
        } else {
            for (const auto& tag : *tags) {
                if (!tag.is_string() || tag.get_ref<const std::string&>().empty()) {
                    addDiagnostic(result, AssetMetadataSeverity::Error, "META_TAG_INVALID",
                        "tags", "Each tag must be a non-empty string.");
                } else {
                    metadata.tags.push_back(tag.get<std::string>());
                }
            }
        }

        if (!result.hasErrors()) result.metadata = std::move(metadata);
        return result;
    }

    AssetMetadataReadResult readAssetMetadata(
        const std::filesystem::path& sidecarPath) {
        std::ifstream input(sidecarPath, std::ios::binary);
        if (!input) {
            AssetMetadataReadResult result;
            addDiagnostic(result, AssetMetadataSeverity::Error, "META_READ_FAILED",
                sidecarPath.generic_string(), "Metadata sidecar could not be opened.");
            return result;
        }
        const std::string text((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        return parseAssetMetadata(text);
    }

    bool writeAssetMetadataAtomic(const std::filesystem::path& sidecarPath,
        const AssetMetadata& metadata, std::string& error) {
        error.clear();
        std::error_code filesystemError;
        if (!sidecarPath.parent_path().empty()) {
            std::filesystem::create_directories(sidecarPath.parent_path(), filesystemError);
            if (filesystemError) {
                error = "Could not create metadata directory: " + filesystemError.message();
                return false;
            }
        }

        std::filesystem::path temporary = sidecarPath;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "Could not open temporary metadata sidecar.";
                return false;
            }
            const std::string bytes = serializeAssetMetadata(metadata);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                error = "Could not write temporary metadata sidecar.";
                output.close();
                std::filesystem::remove(temporary, filesystemError);
                return false;
            }
        }

        if (atomicReplace(temporary, sidecarPath, error)) return true;
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }

    std::vector<SubassetMatch> matchSubassets(
        std::span<const SubassetMetadata> previous,
        std::span<const DiscoveredSubasset> discovered) {
        std::vector<SubassetMatch> result;
        result.reserve(discovered.size());
        std::vector<bool> used(previous.size(), false);

        for (const DiscoveredSubasset& item : discovered) {
            SubassetMatch match{ .discovered = item };
            for (size_t previousIndex = 0; previousIndex < previous.size(); ++previousIndex) {
                if (!used[previousIndex] &&
                    previous[previousIndex].sourceKey == item.sourceKey) {
                    match.existingGuid = previous[previousIndex].guid;
                    match.method = SubassetMatchMethod::ExactSourceKey;
                    used[previousIndex] = true;
                    break;
                }
            }
            result.push_back(std::move(match));
        }

        for (SubassetMatch& match : result) {
            if (match.existingGuid || match.discovered.structuralFingerprint.empty()) continue;
            std::vector<size_t> candidates;
            for (size_t previousIndex = 0; previousIndex < previous.size(); ++previousIndex) {
                if (!used[previousIndex] &&
                    previous[previousIndex].assetType == match.discovered.assetType &&
                    previous[previousIndex].structuralFingerprint ==
                        match.discovered.structuralFingerprint) {
                    candidates.push_back(previousIndex);
                }
            }
            if (candidates.size() == 1) {
                match.existingGuid = previous[candidates.front()].guid;
                match.method = SubassetMatchMethod::UniqueStructuralFingerprint;
                used[candidates.front()] = true;
            } else if (candidates.size() > 1) {
                match.method = SubassetMatchMethod::Ambiguous;
                for (const size_t index : candidates) {
                    match.ambiguousCandidates.push_back(previous[index].guid);
                }
            }
        }
        return result;
    }

} // namespace Iridium
