#include "assets/cooker/AssetCooker.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace Iridium {

    namespace {

        std::vector<std::byte> readFile(
            const std::filesystem::path& path,
            std::stop_token stopToken) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) throw std::runtime_error(
                "Could not open cook source: " + path.generic_string());
            const std::streamsize size = input.tellg();
            if (size < 0) throw std::runtime_error(
                "Could not size cook source: " + path.generic_string());
            input.seekg(0, std::ios::beg);
            std::vector<std::byte> bytes(static_cast<size_t>(size));
            constexpr size_t chunkSize =
                4ull * 1024ull * 1024ull;
            size_t offset = 0;
            while (offset < bytes.size()) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset cook preparation cancelled.");
                }
                const size_t count =
                    std::min(
                        chunkSize,
                        bytes.size() - offset);
                if (!input.read(
                        reinterpret_cast<char*>(
                            bytes.data() + offset),
                        static_cast<std::streamsize>(
                            count))) {
                    throw std::runtime_error(
                        "Could not read cook source: " +
                        path.generic_string());
                }
                offset += count;
            }
            return bytes;
        }

        void appendDiagnostics(std::vector<CookDiagnostic>& destination,
            const std::vector<CookDiagnostic>& source) {
            destination.insert(destination.end(), source.begin(), source.end());
        }

    } // namespace

    PreparedAssetCook prepareAssetCook(
        const ImporterRegistry& registry,
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& sourceRelativePath,
        const AssetMetadata& metadata,
        const CookTarget& target,
        std::string cookerFeatureVersion,
        std::stop_token stopToken) {
        PreparedAssetCook result{
            .assetGuid = metadata.assetGuid,
            .context = {
                .assetGuid = metadata.assetGuid,
                .subassets = metadata.subassets,
            },
            .target = target,
            .cookerFeatureVersion = std::move(cookerFeatureVersion),
        };
        try {
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Asset cook preparation cancelled.");
            }
            const ImporterSelection selection = registry.selectExplicit(
                metadata.importerId, metadata.importerVersion);
            appendDiagnostics(result.diagnostics, selection.diagnostics);
            if (!selection.valid()) return result;
            result.importer = selection.importer;
            result.settings = result.importer->normalizeSettings(
                metadata.settingsSchemaVersion, metadata.settings, true);
            appendDiagnostics(result.diagnostics, result.settings.diagnostics);
            if (!result.settings.valid()) return result;
            result.settingsHash = sha256(result.settings.canonicalBytes);

            const std::filesystem::path sourcePath =
                assetRoot / sourceRelativePath;
            const std::vector<std::byte> sourceBytes =
                readFile(
                    sourcePath,
                    stopToken);
            result.sourceContentHash = sha256(sourceBytes);
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Asset cook preparation cancelled.");
            }
            result.source = result.importer->parse(
                {
                    .relativePath = sourceRelativePath,
                    .resolvedPath = sourcePath,
                    .bytes = sourceBytes,
                    .stopToken = stopToken,
                },
                result.settings);
            appendDiagnostics(result.diagnostics, result.source.diagnostics);
            if (hasCookErrors(result.source.diagnostics)) return result;

            result.resolvedDependencies = result.source.dependencies;
            for (AssetDependency& dependency : result.resolvedDependencies) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset cook preparation cancelled.");
                }
                if (dependency.type == AssetDependencyType::Tool &&
                    !dependency.contentHash.empty()) {
                    // A pinned embedded/library tool identity is already resolved.
                    // The importer implementation version and this exact content
                    // identity both participate in the cook key.
                    continue;
                }
                if (dependency.type == AssetDependencyType::SourceFile ||
                    dependency.type == AssetDependencyType::Tool) {
                    const std::filesystem::path dependencyPath =
                        assetRoot / dependency.location;
                    std::error_code pathError;
                    const auto relative = std::filesystem::relative(
                        dependencyPath, assetRoot, pathError);
                    if (pathError || relative.empty() ||
                        relative.generic_string().starts_with("..") ||
                        !std::filesystem::is_regular_file(dependencyPath)) {
                        result.diagnostics.push_back({
                            .code = "COOK_DEPENDENCY_MISSING",
                            .field = dependency.location,
                            .message = "Required source/tool dependency is unavailable.",
                        });
                        continue;
                    }
                    dependency.location = relative.generic_string();
                    dependency.contentHash = sha256File(dependencyPath);
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Asset cook preparation cancelled.");
                    }
                } else if (dependency.type == AssetDependencyType::Asset &&
                    dependency.contentHash.empty() &&
                    dependency.artifactHash.empty()) {
                    result.diagnostics.push_back({
                        .code = "COOK_ASSET_DEPENDENCY_UNRESOLVED",
                        .field = dependency.assetGuid
                            ? dependency.assetGuid->toString() : "",
                        .message = "Required asset dependency has no resolved hash.",
                    });
                }
            }
            if (hasCookErrors(result.diagnostics)) return result;
            std::sort(result.resolvedDependencies.begin(),
                result.resolvedDependencies.end());
            result.cookKey = calculateCookKey({
                .assetGuid = metadata.assetGuid,
                .importerId = result.importer->descriptor().id,
                .importerImplementationVersion =
                    result.importer->descriptor().implementationVersion,
                .settingsSchemaVersion = result.settings.schemaVersion,
                .canonicalSettings = result.settings.canonicalBytes,
                .sourceContentHash = result.sourceContentHash,
                .dependencies = result.resolvedDependencies,
                .target = target,
                .cookerFeatureVersion = result.cookerFeatureVersion,
            });
        } catch (const std::exception& exception) {
            result.diagnostics.push_back({
                .code = "COOK_PREPARE_EXCEPTION",
                .message = exception.what(),
            });
        }
        return result;
    }

    CookedArtifactBlob buildPreparedArtifact(
        const PreparedAssetCook& prepared,
        std::stop_token stopToken) {
        if (!prepared.valid()) {
            throw std::invalid_argument("Cannot build an invalid prepared cook.");
        }
        CookProduct product = prepared.importer->cook(
            prepared.source, prepared.settings, prepared.target,
            prepared.context, stopToken);
        if (hasCookErrors(product.diagnostics)) {
            std::string message = "Importer cook failed";
            for (const CookDiagnostic& diagnostic : product.diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code + " " + diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }
        return serializeCookedArtifact({
            .assetGuid = prepared.assetGuid,
            .artifactType = std::move(product.artifactType),
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .target = prepared.target,
            .cookKey = prepared.cookKey,
            .dependencies = prepared.resolvedDependencies,
            .sections = std::move(product.sections),
        });
    }

    std::shared_future<DdcRequestResult> requestPreparedCook(
        DerivedDataCache& cache,
        const PreparedAssetCook& prepared,
        std::stop_token stopToken) {
        return requestPreparedCook(
            cache,
            std::make_shared<PreparedAssetCook>(prepared),
            stopToken);
    }

    std::shared_future<DdcRequestResult> requestPreparedCook(
        DerivedDataCache& cache,
        std::shared_ptr<const PreparedAssetCook> prepared,
        std::stop_token stopToken) {
        if (!prepared || !prepared->valid()) {
            std::promise<DdcRequestResult> promise;
            DdcRequestResult result{
                .status = DdcRequestStatus::Failed,
                .diagnostics = prepared
                    ? prepared->diagnostics
                    : std::vector<CookDiagnostic>{},
            };
            promise.set_value(std::move(result));
            return promise.get_future().share();
        }
        const std::string cookKey = prepared->cookKey;
        return cache.request(cookKey, stopToken,
            [prepared = std::move(prepared)](
                std::stop_token token) {
                if (token.stop_requested()) return CookedArtifactBlob{};
                return buildPreparedArtifact(
                    *prepared, token);
            });
    }

} // namespace Iridium
