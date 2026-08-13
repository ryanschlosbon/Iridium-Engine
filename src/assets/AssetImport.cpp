#include "assets/AssetImport.h"

#include "assets/cooker/TextFixtureImporter.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"
#include "assets/environment/EnvironmentImporter.h"
#include "assets/environment/BakedProbeEnvironmentImporter.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace Iridium {

    namespace {

        std::vector<std::byte> readFile(
            const std::filesystem::path& path,
            std::stop_token stopToken) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error(
                    "Could not open source: " + path.generic_string());
            }
            const std::streamsize size = input.tellg();
            if (size < 0) {
                throw std::runtime_error(
                    "Could not size source: " + path.generic_string());
            }
            input.seekg(0, std::ios::beg);
            std::vector<std::byte> result(static_cast<size_t>(size));
            constexpr size_t chunkSize =
                4ull * 1024ull * 1024ull;
            size_t offset = 0;
            while (offset < result.size()) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset import cancelled.");
                }
                const size_t count = std::min(
                    chunkSize,
                    result.size() - offset);
                if (!input.read(
                        reinterpret_cast<char*>(
                            result.data() + offset),
                        static_cast<std::streamsize>(
                            count))) {
                    throw std::runtime_error(
                        "Could not read source: " +
                        path.generic_string());
                }
                offset += count;
            }
            return result;
        }

        std::string firstDiagnostic(
            const std::vector<CookDiagnostic>& diagnostics,
            std::string fallback) {
            for (const CookDiagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    return diagnostic.code + ": " + diagnostic.message;
                }
            }
            return fallback;
        }

    } // namespace

    ImporterRegistry createStandardAssetImporterRegistry() {
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextFixtureImporter>());
        registry.registerImporter(std::make_shared<TextureImporter>());
        registry.registerImporter(std::make_shared<EnvironmentImporter>());
        registry.registerImporter(
            std::make_shared<BakedProbeEnvironmentImporter>());
        registry.registerImporter(std::make_shared<GltfModelImporter>());
        return registry;
    }

    namespace {

    AssetImportResult importAssetSourceImpl(
        const std::filesystem::path& requestedSourcePath,
        const ImporterRegistry& importers,
        const nlohmann::json* settingsOverride,
        std::stop_token stopToken) {
        const std::filesystem::path source =
            std::filesystem::absolute(requestedSourcePath).lexically_normal();
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "Asset import source is not a regular file.");
        }
        const std::filesystem::path sidecar =
            assetMetadataSidecarPath(source);
        const std::vector<std::byte> bytes =
            readFile(source, stopToken);
        if (stopToken.stop_requested()) {
            throw std::runtime_error(
                "Asset import cancelled.");
        }
        const ImporterSelection selection =
            importers.selectAutomatic(source.filename(), bytes);
        if (!selection.valid()) {
            throw std::runtime_error(firstDiagnostic(
                selection.diagnostics,
                "No registered importer accepted the source."));
        }

        const ImporterDescriptor& descriptor =
            selection.importer->descriptor();
        std::optional<AssetMetadata> existingMetadata;
        if (std::filesystem::is_regular_file(sidecar)) {
            const AssetMetadataReadResult existing =
                readAssetMetadata(sidecar);
            if (!existing.metadata || existing.hasErrors()) {
                throw std::runtime_error(
                    "Existing metadata sidecar is invalid.");
            }
            if (existing.metadata->importerId != descriptor.id ||
                existing.metadata->importerVersion >
                    descriptor.implementationVersion) {
                throw std::runtime_error(
                    "Existing metadata selects a different or newer importer.");
            }
            existingMetadata = *existing.metadata;
        }

        const NormalizedImportSettings settings =
            selection.importer->normalizeSettings(
                existingMetadata
                    ? existingMetadata->settingsSchemaVersion
                    : descriptor.currentSettingsSchemaVersion,
                settingsOverride
                    ? *settingsOverride
                    : existingMetadata
                        ? existingMetadata->settings
                        : nlohmann::json::object(),
                true);
        if (!settings.valid()) {
            throw std::runtime_error(firstDiagnostic(
                settings.diagnostics,
                "Importer settings failed normalization."));
        }
        const ParsedSourceAsset parsed = selection.importer->parse({
            .relativePath = source.filename(),
            .resolvedPath = source,
            .bytes = bytes,
            .stopToken = stopToken,
            .metadataOnly = true,
        }, settings);
        if (hasCookErrors(parsed.diagnostics)) {
            throw std::runtime_error(firstDiagnostic(
                parsed.diagnostics, "Asset source parsing failed."));
        }

        AssetImportResult result{
            .sourcePath = source,
            .metadataPath = sidecar,
            .diagnostics = parsed.diagnostics,
        };
        if (existingMetadata) {
            result.metadata = *existingMetadata;
        }
        else {
            result.metadata.assetGuid = createAssetGuidV7();
            result.metadata.assetType = descriptor.assetTypes.front();
            result.metadata.importerId = descriptor.id;
        }
        result.metadata.importerVersion =
            descriptor.implementationVersion;
        result.metadata.settingsSchemaVersion = settings.schemaVersion;
        result.metadata.settings = settings.values;

        const std::vector<SubassetMatch> matches =
            matchSubassets(result.metadata.subassets,
                parsed.discoveredSubassets);
        std::vector<SubassetMetadata> updated;
        updated.reserve(matches.size());
        for (const SubassetMatch& match : matches) {
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Asset import cancelled.");
            }
            if (match.method == SubassetMatchMethod::Ambiguous) {
                throw std::runtime_error(
                    "Ambiguous subasset fingerprint match for " +
                    match.discovered.sourceKey +
                    "; metadata was not changed.");
            }
            const AssetGuid guid = match.existingGuid
                ? *match.existingGuid : createAssetGuidV7();
            if (match.existingGuid) ++result.preservedSubassets;
            else ++result.createdSubassets;
            updated.push_back({
                .guid = guid,
                .assetType = match.discovered.assetType,
                .sourceKey = match.discovered.sourceKey,
                .structuralFingerprint =
                    match.discovered.structuralFingerprint,
            });
        }
        result.metadata.subassets = std::move(updated);
        result.dependencyCount = parsed.dependencies.size();

        if (stopToken.stop_requested()) {
            throw std::runtime_error(
                "Asset import cancelled.");
        }
        std::string writeError;
        if (!writeAssetMetadataAtomic(
            sidecar, result.metadata, writeError)) {
            throw std::runtime_error(writeError);
        }
        return result;
    }

    } // namespace

    AssetImportResult importAssetSource(
        const std::filesystem::path& sourcePath,
        const ImporterRegistry& importers,
        std::stop_token stopToken) {
        return importAssetSourceImpl(
            sourcePath, importers, nullptr,
            stopToken);
    }

    AssetImportResult updateAssetImportSettings(
        const std::filesystem::path& sourcePath,
        const ImporterRegistry& importers,
        nlohmann::json settings,
        std::stop_token stopToken) {
        return importAssetSourceImpl(
            sourcePath, importers, &settings,
            stopToken);
    }

} // namespace Iridium
