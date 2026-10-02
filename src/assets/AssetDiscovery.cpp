#include "assets/AssetDiscovery.h"

#include <algorithm>
#include <map>
#include <system_error>

namespace Iridium {

    namespace {

        constexpr std::string_view kSidecarSuffix = ".iridium.meta";

        bool isSidecar(const std::filesystem::path& path) {
            const std::string filename = path.filename().generic_string();
            return filename.size() > kSidecarSuffix.size() &&
                filename.ends_with(kSidecarSuffix);
        }

        std::filesystem::path sourceForSidecar(const std::filesystem::path& sidecar) {
            auto native = sidecar.native();
            native.resize(native.size() - kSidecarSuffix.size());
            return std::filesystem::path(std::move(native));
        }

        std::string relativePath(const std::filesystem::path& path,
            const std::filesystem::path& root) {
            std::error_code error;
            const auto relative = std::filesystem::relative(path, root, error);
            return error ? path.filename().generic_string() : relative.generic_string();
        }

        void appendMetadataDiagnostics(AssetDiscoveryResult& result,
            const std::filesystem::path& sidecar,
            const std::vector<AssetMetadataDiagnostic>& diagnostics) {
            for (const AssetMetadataDiagnostic& diagnostic : diagnostics) {
                result.diagnostics.push_back({
                    .severity = diagnostic.severity,
                    .code = diagnostic.code,
                    .path = sidecar.generic_string() +
                        (diagnostic.field.empty() ? "" : ":" + diagnostic.field),
                    .message = diagnostic.message,
                });
            }
        }

    } // namespace

    bool AssetDiscoveryResult::hasErrors() const noexcept {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const AssetDiscoveryDiagnostic& diagnostic) {
                return diagnostic.severity == AssetMetadataSeverity::Error;
            });
    }

    AssetDiscoveryResult discoverAssetRoots(std::span<const AssetRoot> roots) {
        AssetDiscoveryResult result;

        for (const AssetRoot& root : roots) {
            std::error_code error;
            if (!std::filesystem::is_directory(root.path, error)) {
                result.diagnostics.push_back({
                    .severity = AssetMetadataSeverity::Error,
                    .code = "ASSET_ROOT_UNAVAILABLE",
                    .path = root.path.generic_string(),
                    .message = "Asset root does not exist or is not a directory.",
                });
                continue;
            }

            std::filesystem::recursive_directory_iterator iterator(
                root.path, std::filesystem::directory_options::skip_permission_denied, error);
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end) {
                const std::filesystem::directory_entry entry = *iterator;
                iterator.increment(error);
                if (entry.is_directory()) {
                    result.sourceDirectories.push_back(
                        relativePath(entry.path(), root.path));
                    continue;
                }
                if (!entry.is_regular_file() || !isSidecar(entry.path())) continue;

                const std::filesystem::path sidecar = entry.path();
                const std::filesystem::path source = sourceForSidecar(sidecar);
                const AssetMetadataReadResult read = readAssetMetadata(sidecar);
                appendMetadataDiagnostics(result, sidecar, read.diagnostics);
                if (!read.metadata) continue;

                const AssetMetadata& metadata = *read.metadata;
                const bool sourceExists = std::filesystem::is_regular_file(source);
                const AssetCatalogStatus status = sourceExists
                    ? AssetCatalogStatus::Ready : AssetCatalogStatus::MissingSource;
                const std::string sourceRelative = relativePath(source, root.path);
                const std::string metadataRelative = relativePath(sidecar, root.path);
                const std::string missingSummary = sourceExists
                    ? "" : "Source file is missing; move the sidecar with its source.";

                result.records.push_back({
                    .guid = metadata.assetGuid,
                    .assetType = metadata.assetType,
                    .assetRoot = root.id,
                    .sourcePath = sourceRelative,
                    .metadataPath = metadataRelative,
                    .displayName = source.filename().generic_string(),
                    .importerId = metadata.importerId,
                    .importerVersion = metadata.importerVersion,
                    .status = status,
                    .tags = metadata.tags,
                    .diagnosticSummary = missingSummary,
                });
                for (const SubassetMetadata& subasset : metadata.subassets) {
                    result.records.push_back({
                        .guid = subasset.guid,
                        .parentGuid = metadata.assetGuid,
                        .assetType = subasset.assetType,
                        .assetRoot = root.id,
                        .sourcePath = sourceRelative,
                        .metadataPath = metadataRelative,
                        .sourceKey = subasset.sourceKey,
                        .displayName = source.filename().generic_string() +
                            " : " + subasset.sourceKey,
                        .importerId = metadata.importerId,
                        .importerVersion = metadata.importerVersion,
                        .status = status,
                        .tags = metadata.tags,
                        .diagnosticSummary = missingSummary,
                    });
                }
            }
            if (error) {
                result.diagnostics.push_back({
                    .severity = AssetMetadataSeverity::Error,
                    .code = "ASSET_ROOT_SCAN_FAILED",
                    .path = root.path.generic_string(),
                    .message = error.message(),
                });
            }
        }

        std::map<AssetGuid, std::vector<size_t>> byGuid;
        for (size_t index = 0; index < result.records.size(); ++index) {
            byGuid[result.records[index].guid].push_back(index);
        }
        for (const auto& [guid, indices] : byGuid) {
            if (indices.size() < 2) continue;
            for (const size_t index : indices) {
                result.records[index].status = AssetCatalogStatus::DuplicateGuid;
                result.records[index].diagnosticSummary =
                    "GUID is duplicated by another metadata record.";
            }
            result.diagnostics.push_back({
                .severity = AssetMetadataSeverity::Error,
                .code = "ASSET_GUID_DUPLICATE",
                .path = guid.toString(),
                .message = "Multiple catalog records claim the same persistent GUID.",
            });
        }

        std::sort(result.records.begin(), result.records.end(),
            [](const AssetCatalogRecord& lhs, const AssetCatalogRecord& rhs) {
                if (lhs.guid != rhs.guid) return lhs.guid < rhs.guid;
                if (lhs.assetRoot != rhs.assetRoot) return lhs.assetRoot < rhs.assetRoot;
                if (lhs.sourcePath != rhs.sourcePath) return lhs.sourcePath < rhs.sourcePath;
                return lhs.sourceKey < rhs.sourceKey;
            });
        std::ranges::sort(result.sourceDirectories);
        result.sourceDirectories.erase(
            std::unique(result.sourceDirectories.begin(),
                result.sourceDirectories.end()),
            result.sourceDirectories.end());
        return result;
    }

} // namespace Iridium
