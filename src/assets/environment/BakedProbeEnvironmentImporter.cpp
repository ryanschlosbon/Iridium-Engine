#include "assets/environment/BakedProbeEnvironmentImporter.h"

#include "assets/environment/EnvironmentProduct.h"

#include <algorithm>
#include <cctype>

namespace Iridium {
namespace {

    void error(std::vector<CookDiagnostic>& diagnostics, std::string code,
        std::string message) {
        diagnostics.push_back({ CookDiagnosticSeverity::Error,
            std::move(code), "/", std::move(message) });
    }

} // namespace

const ImporterDescriptor&
BakedProbeEnvironmentImporter::descriptor() const noexcept {
    static const ImporterDescriptor value{
        .id = "iridium.environment.probe_capture",
        .implementationVersion = 1,
        .currentSettingsSchemaVersion = 1,
        .assetTypes = { "iridium.environment" },
        .extensions = { ".irprobe" },
    };
    return value;
}

ImportProbeResult BakedProbeEnvironmentImporter::probe(
    const std::filesystem::path& relativePath,
    std::span<const std::byte> sourceBytes) const {
    std::string extension = relativePath.extension().generic_string();
    std::ranges::transform(extension, extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".irprobe" && !sourceBytes.empty()
        ? ImportProbeResult::Supported : ImportProbeResult::Unsupported;
}

NormalizedImportSettings BakedProbeEnvironmentImporter::normalizeSettings(
    uint32_t sourceSchemaVersion, const nlohmann::json& settings,
    bool strict) const {
    NormalizedImportSettings result;
    result.schemaVersion = descriptor().currentSettingsSchemaVersion;
    if (sourceSchemaVersion != result.schemaVersion || !settings.is_object() ||
        (strict && !settings.empty())) {
        error(result.diagnostics, "PROBE_CAPTURE_SETTINGS",
            "Baked probe capture settings must be an empty schema-1 object.");
        return result;
    }
    static constexpr char EmptyObject[] = "{}";
    result.canonicalBytes.assign(
        reinterpret_cast<const std::byte*>(EmptyObject),
        reinterpret_cast<const std::byte*>(EmptyObject + 2));
    result.values = nlohmann::json::object();
    return result;
}

ParsedSourceAsset BakedProbeEnvironmentImporter::parse(
    const ImportSource& source,
    const NormalizedImportSettings& settings) const {
    ParsedSourceAsset result;
    if (!settings.valid() || source.stopToken.stop_requested()) {
        error(result.diagnostics, "PROBE_CAPTURE_IMPORT_CANCELLED",
            "Baked probe capture import was cancelled or invalid.");
        return result;
    }
    CookedArtifactReadResult decoded = readCookedArtifact(source.bytes);
    if (!decoded.valid()) {
        result.diagnostics = std::move(decoded.diagnostics);
        error(result.diagnostics, "PROBE_CAPTURE_SOURCE",
            "The baked probe source is not a valid cooked environment container.");
        return result;
    }
    CookedEnvironmentReadResult environment =
        readCookedEnvironmentProduct(*decoded.artifact);
    if (!environment.valid()) {
        result.diagnostics = std::move(environment.diagnostics);
        error(result.diagnostics, "PROBE_CAPTURE_PRODUCT",
            "The baked probe source does not contain a complete environment product.");
        return result;
    }
    result.documentBytes.assign(source.bytes.begin(), source.bytes.end());
    result.dependencies = decoded.artifact->dependencies;
    return result;
}

CookProduct BakedProbeEnvironmentImporter::cook(
    const ParsedSourceAsset& source,
    const NormalizedImportSettings& settings,
    const CookTarget&, const AssetCookContext& context,
    std::stop_token stopToken) const {
    CookProduct failed{
        .artifactType = "iridium.environment",
        .artifactSchemaVersion = kCookedEnvironmentSchemaVersion,
    };
    if (!settings.valid() || hasCookErrors(source.diagnostics) ||
        stopToken.stop_requested()) {
        error(failed.diagnostics, "PROBE_CAPTURE_COOK_INPUT",
            "Baked probe capture cook input is invalid or cancelled.");
        return failed;
    }
    CookedArtifactReadResult decoded = readCookedArtifact(
        source.documentBytes);
    if (!decoded.valid() || decoded.artifact->assetGuid != context.assetGuid) {
        error(failed.diagnostics, "PROBE_CAPTURE_IDENTITY",
            "Baked probe source and asset metadata identities disagree.");
        return failed;
    }
    CookedEnvironmentReadResult environment =
        readCookedEnvironmentProduct(*decoded.artifact);
    if (!environment.valid()) {
        failed.diagnostics = std::move(environment.diagnostics);
        return failed;
    }
    const CookedEnvironmentProductData& product = *environment.data;
    return makeCookedEnvironmentProduct(product.manifest,
        { product.radiance, product.irradiance,
            product.prefilteredSpecular, product.brdfLut });
}

} // namespace Iridium
