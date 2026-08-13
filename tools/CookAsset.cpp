#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/environment/EnvironmentImporter.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace {

    using namespace Iridium;

    struct Options {
        std::filesystem::path source;
        std::filesystem::path metadata;
        std::filesystem::path assetRoot;
        std::filesystem::path ddc;
        CookTarget target{
            .platform = "windows-x64",
            .profile = "release",
            .qualityPolicy = "reference",
        };
    };

    std::optional<Options> parseOptions(int argc, char** argv) {
        Options result;
        for (int index = 1; index < argc; ++index) {
            if (index + 1 >= argc) return std::nullopt;
            const std::string argument = argv[index];
            const std::string value = argv[++index];
            if (argument == "--source") result.source = value;
            else if (argument == "--metadata") result.metadata = value;
            else if (argument == "--asset-root") result.assetRoot = value;
            else if (argument == "--ddc") result.ddc = value;
            else if (argument == "--platform") result.target.platform = value;
            else if (argument == "--profile") result.target.profile = value;
            else if (argument == "--quality") result.target.qualityPolicy = value;
            else return std::nullopt;
        }
        if (result.source.empty() || result.ddc.empty()) return std::nullopt;
        result.source = std::filesystem::absolute(result.source);
        if (result.metadata.empty()) {
            result.metadata = assetMetadataSidecarPath(result.source);
        } else {
            result.metadata = std::filesystem::absolute(result.metadata);
        }
        if (result.assetRoot.empty()) {
            result.assetRoot = result.source.parent_path();
        } else {
            result.assetRoot = std::filesystem::absolute(result.assetRoot);
        }
        result.ddc = std::filesystem::absolute(result.ddc);
        return result;
    }

    const char* statusName(DdcRequestStatus status) {
        switch (status) {
        case DdcRequestStatus::CacheHit: return "cache-hit";
        case DdcRequestStatus::Built: return "built";
        case DdcRequestStatus::Cancelled: return "cancelled";
        case DdcRequestStatus::Failed: return "failed";
        }
        return "unknown";
    }

    nlohmann::ordered_json diagnosticsJson(
        const std::vector<CookDiagnostic>& diagnostics) {
        nlohmann::ordered_json result = nlohmann::ordered_json::array();
        for (const CookDiagnostic& diagnostic : diagnostics) {
            const char* severity = diagnostic.severity ==
                CookDiagnosticSeverity::Error ? "error" :
                diagnostic.severity == CookDiagnosticSeverity::Warning
                    ? "warning" : "info";
            result.push_back({
                { "severity", severity },
                { "code", diagnostic.code },
                { "field", diagnostic.field },
                { "message", diagnostic.message },
            });
        }
        return result;
    }

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        std::cerr
            << "Usage: IridiumCookAsset --source path --ddc directory "
               "[--metadata path] [--asset-root path] [--platform name] "
               "[--profile name] [--quality name]\n";
        return 1;
    }

    try {
        const AssetMetadataReadResult metadata =
            readAssetMetadata(options->metadata);
        if (!metadata.metadata) {
            nlohmann::ordered_json output{
                { "status", "failed" },
                { "metadataDiagnostics", nlohmann::ordered_json::array() },
            };
            for (const AssetMetadataDiagnostic& diagnostic : metadata.diagnostics) {
                output["metadataDiagnostics"].push_back({
                    { "code", diagnostic.code },
                    { "field", diagnostic.field },
                    { "message", diagnostic.message },
                });
            }
            std::cout << output.dump(2) << '\n';
            return 2;
        }

        std::error_code pathError;
        const std::filesystem::path relativeSource =
            std::filesystem::relative(options->source, options->assetRoot, pathError);
        if (pathError || relativeSource.generic_string().starts_with("..")) {
            throw std::runtime_error("Source must be inside the configured asset root.");
        }
        ImporterRegistry registry;
        registry.registerImporter(std::make_shared<TextFixtureImporter>());
        registry.registerImporter(std::make_shared<TextureImporter>());
        registry.registerImporter(std::make_shared<EnvironmentImporter>());
        registry.registerImporter(std::make_shared<GltfModelImporter>());
        LocalDerivedDataCache cache(options->ddc);
        std::vector<CookDiagnostic> receiptDiagnostics;
        std::optional<PreparedAssetCook> receipt =
            tryPrepareAssetCookFromReceipt(
                registry, cache, options->assetRoot, relativeSource,
                *metadata.metadata, options->target,
                "m3.2-framework-v3", receiptDiagnostics);
        const bool usedReceipt = receipt.has_value();
        auto prepared = std::make_shared<PreparedAssetCook>(usedReceipt
            ? std::move(*receipt)
            : prepareAssetCook(
                registry, options->assetRoot, relativeSource,
                *metadata.metadata, options->target,
                "m3.2-framework-v3"));
        prepared->diagnostics.insert(prepared->diagnostics.end(),
            receiptDiagnostics.begin(), receiptDiagnostics.end());
        if (!prepared->valid()) {
            std::cout << nlohmann::ordered_json{
                { "status", "failed" },
                { "diagnostics", diagnosticsJson(prepared->diagnostics) },
            }.dump(2) << '\n';
            return 2;
        }

        DdcRequestResult result =
            requestPreparedCook(cache, prepared).get();
        if (result.status == DdcRequestStatus::Built ||
            result.status == DdcRequestStatus::CacheHit) {
            std::vector<CookDiagnostic> receiptStore =
                storePreparedCookReceipt(
                    cache, relativeSource, *prepared);
            result.diagnostics.insert(result.diagnostics.end(),
                receiptStore.begin(), receiptStore.end());
        }
        nlohmann::ordered_json output{
            { "status", statusName(result.status) },
            { "preparation", usedReceipt
                ? "receipt-hit" : "source-parse" },
            { "assetGuid", prepared->assetGuid.toString() },
            { "importer", prepared->importer->descriptor().id },
            { "importerVersion",
                prepared->importer->descriptor().implementationVersion },
            { "settingsSchemaVersion", prepared->settings.schemaVersion },
            { "settingsHash", prepared->settingsHash },
            { "sourceHash", prepared->sourceContentHash },
            { "cookKey", prepared->cookKey },
            { "artifactHash", result.blob
                ? nlohmann::json(result.blob->artifactHash) : nlohmann::json(nullptr) },
            { "dependencies", nlohmann::ordered_json::array() },
            { "diagnostics", diagnosticsJson(result.diagnostics) },
        };
        for (const AssetDependency& dependency : prepared->resolvedDependencies) {
            output["dependencies"].push_back({
                { "type", static_cast<uint32_t>(dependency.type) },
                { "assetGuid", dependency.assetGuid
                    ? nlohmann::json(dependency.assetGuid->toString())
                    : nlohmann::json(nullptr) },
                { "location", dependency.location },
                { "contentHash", dependency.contentHash },
                { "artifactHash", dependency.artifactHash },
            });
        }
        std::cout << output.dump(2) << '\n';
        return result.status == DdcRequestStatus::Built ||
            result.status == DdcRequestStatus::CacheHit ? 0 : 3;
    } catch (const std::exception& exception) {
        std::cerr << "Asset cook failed: " << exception.what() << '\n';
        return 4;
    }
}
