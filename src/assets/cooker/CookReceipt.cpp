#include "assets/cooker/CookReceipt.h"

#include "assets/cooker/LocalDerivedDataCache.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace Iridium {

    namespace {

        using Json = nlohmann::ordered_json;

        void warning(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string message) {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Warning,
                .code = std::move(code),
                .message = std::move(message),
            });
        }

        Json requestIdentity(const AssetMetadata& metadata,
            const ImporterDescriptor& importer,
            std::string_view settingsHash,
            const CookTarget& target,
            std::string_view cookerFeatureVersion,
            const std::filesystem::path& sourceRelativePath) {
            return {
                { "asset_guid", metadata.assetGuid.toString() },
                { "container_version", target.artifactContainerVersion },
                { "cooker_feature_version", cookerFeatureVersion },
                { "importer_id", importer.id },
                { "importer_version", importer.implementationVersion },
                { "material_schema_version", target.materialSchemaVersion },
                { "platform", target.platform },
                { "profile", target.profile },
                { "quality_policy", target.qualityPolicy },
                { "settings_hash", settingsHash },
                { "settings_schema",
                    importer.currentSettingsSchemaVersion },
                { "source", sourceRelativePath.generic_string() },
            };
        }

        std::string identityHash(const Json& identity) {
            const std::string text = identity.dump();
            return sha256(std::as_bytes(std::span<const char>(
                text.data(), text.size())));
        }

        std::filesystem::path receiptPath(
            const LocalDerivedDataCache& cache,
            const AssetMetadata& metadata,
            const Json& identity) {
            return cache.root() / "receipts" /
                metadata.assetGuid.toString() /
                (identityHash(identity) + ".json");
        }

        bool atomicReplace(const std::filesystem::path& source,
            const std::filesystem::path& destination) {
#if defined(_WIN32)
            return MoveFileExW(source.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
        }

        std::optional<Json> readJson(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return std::nullopt;
            try {
                return Json::parse(input);
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<AssetDependency> parseDependency(const Json& value) {
            try {
                AssetDependency dependency{
                    .type = static_cast<AssetDependencyType>(
                        value.at("type").get<uint32_t>()),
                    .location = value.at("location").get<std::string>(),
                    .contentHash =
                        value.at("content_hash").get<std::string>(),
                    .artifactHash =
                        value.at("artifact_hash").get<std::string>(),
                };
                if (static_cast<uint8_t>(dependency.type) >
                    static_cast<uint8_t>(
                        AssetDependencyType::OptionalAsset)) {
                    return std::nullopt;
                }
                if (!value.at("asset_guid").is_null()) {
                    dependency.assetGuid = AssetGuid::parse(
                        value.at("asset_guid").get<std::string>());
                    if (!dependency.assetGuid) return std::nullopt;
                }
                return dependency;
            } catch (...) {
                return std::nullopt;
            }
        }

    } // namespace

    std::optional<PreparedAssetCook> tryPrepareAssetCookFromReceipt(
        const ImporterRegistry& registry,
        LocalDerivedDataCache& cache,
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& sourceRelativePath,
        const AssetMetadata& metadata,
        const CookTarget& target,
        std::string cookerFeatureVersion,
        std::vector<CookDiagnostic>& diagnostics) {
        const ImporterSelection selection = registry.selectExplicit(
            metadata.importerId, metadata.importerVersion);
        if (!selection.valid()) return std::nullopt;
        NormalizedImportSettings settings =
            selection.importer->normalizeSettings(
                metadata.settingsSchemaVersion, metadata.settings, true);
        if (!settings.valid()) return std::nullopt;
        const std::string settingsHash = sha256(settings.canonicalBytes);
        const Json identity = requestIdentity(metadata,
            selection.importer->descriptor(), settingsHash, target,
            cookerFeatureVersion, sourceRelativePath);
        const std::filesystem::path path =
            receiptPath(cache, metadata, identity);
        if (!std::filesystem::is_regular_file(path)) return std::nullopt;
        const std::optional<Json> receipt = readJson(path);
        if (!receipt || receipt->value("schema", 0u) != 1u ||
            receipt->value("identity_hash", std::string{}) !=
                identityHash(identity) ||
            receipt->value("identity", Json::object()) != identity) {
            warning(diagnostics, "COOK_RECEIPT_INVALID",
                "Cook receipt is invalid; falling back to source parsing.");
            return std::nullopt;
        }

        const std::filesystem::path sourcePath =
            assetRoot / sourceRelativePath;
        if (!std::filesystem::is_regular_file(sourcePath)) {
            return std::nullopt;
        }
        const std::string sourceHash = sha256File(sourcePath);
        if (sourceHash !=
            receipt->value("source_hash", std::string{})) {
            return std::nullopt;
        }

        std::vector<AssetDependency> dependencies;
        const auto dependencyValues = receipt->find("dependencies");
        if (dependencyValues == receipt->end() ||
            !dependencyValues->is_array()) {
            warning(diagnostics, "COOK_RECEIPT_DEPENDENCIES",
                "Cook receipt dependency table is invalid; falling back.");
            return std::nullopt;
        }
        for (const Json& value : *dependencyValues) {
            std::optional<AssetDependency> dependency =
                parseDependency(value);
            if (!dependency) return std::nullopt;
            if (dependency->type == AssetDependencyType::SourceFile) {
                const std::filesystem::path dependencyPath =
                    assetRoot / dependency->location;
                if (!std::filesystem::is_regular_file(dependencyPath) ||
                    sha256File(dependencyPath) !=
                        dependency->contentHash) {
                    return std::nullopt;
                }
            } else if (dependency->type == AssetDependencyType::Asset) {
                // Asset revision validation becomes catalog-driven in M3.5.
                return std::nullopt;
            }
            dependencies.push_back(std::move(*dependency));
        }
        std::sort(dependencies.begin(), dependencies.end());
        const std::string cookKey =
            receipt->value("cook_key", std::string{});
        if (cache.probe(cookKey).status != DdcLookupStatus::Hit) {
            return std::nullopt;
        }

        return PreparedAssetCook{
            .assetGuid = metadata.assetGuid,
            .context = {
                .assetGuid = metadata.assetGuid,
                .subassets = metadata.subassets,
            },
            .importer = selection.importer,
            .settings = std::move(settings),
            .resolvedDependencies = std::move(dependencies),
            .target = target,
            .sourceContentHash = sourceHash,
            .settingsHash = settingsHash,
            .cookKey = cookKey,
            .cookerFeatureVersion =
                std::move(cookerFeatureVersion),
        };
    }

    std::vector<CookDiagnostic> storePreparedCookReceipt(
        const LocalDerivedDataCache& cache,
        const std::filesystem::path& sourceRelativePath,
        const PreparedAssetCook& prepared) {
        std::vector<CookDiagnostic> diagnostics;
        if (!prepared.valid() || prepared.cookKey.empty()) {
            warning(diagnostics, "COOK_RECEIPT_PREPARED_INVALID",
                "Invalid prepared cook cannot publish a receipt.");
            return diagnostics;
        }
        AssetMetadata identityMetadata{
            .assetGuid = prepared.assetGuid,
            .importerId = prepared.importer->descriptor().id,
            .importerVersion =
                prepared.importer->descriptor().implementationVersion,
            .settingsSchemaVersion = prepared.settings.schemaVersion,
        };
        const Json identity = requestIdentity(identityMetadata,
            prepared.importer->descriptor(), prepared.settingsHash,
            prepared.target, prepared.cookerFeatureVersion,
            sourceRelativePath);
        Json receipt{
            { "schema", 1 },
            { "identity_hash", identityHash(identity) },
            { "identity", identity },
            { "source_hash", prepared.sourceContentHash },
            { "cook_key", prepared.cookKey },
            { "dependencies", Json::array() },
        };
        for (const AssetDependency& dependency :
            prepared.resolvedDependencies) {
            receipt["dependencies"].push_back({
                { "type", static_cast<uint32_t>(dependency.type) },
                { "asset_guid", dependency.assetGuid
                    ? Json(dependency.assetGuid->toString())
                    : Json(nullptr) },
                { "location", dependency.location },
                { "content_hash", dependency.contentHash },
                { "artifact_hash", dependency.artifactHash },
            });
        }

        const std::filesystem::path destination =
            receiptPath(cache, identityMetadata, identity);
        std::error_code filesystemError;
        std::filesystem::create_directories(
            destination.parent_path(), filesystemError);
        if (filesystemError) {
            warning(diagnostics, "COOK_RECEIPT_DIRECTORY",
                "Could not create cook receipt directory: " +
                    filesystemError.message());
            return diagnostics;
        }
        const std::filesystem::path temporary =
            destination.string() + "." +
            createAssetGuidV7().toString() + ".tmp";
        {
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc);
            output << receipt.dump(2) << '\n';
            output.flush();
            if (!output) {
                warning(diagnostics, "COOK_RECEIPT_WRITE",
                    "Could not write temporary cook receipt.");
            }
        }
        if (diagnostics.empty() &&
            !atomicReplace(temporary, destination)) {
            warning(diagnostics, "COOK_RECEIPT_PUBLISH",
                "Could not atomically publish cook receipt.");
        }
        if (!diagnostics.empty()) {
            std::filesystem::remove(temporary, filesystemError);
        }
        return diagnostics;
    }

} // namespace Iridium
