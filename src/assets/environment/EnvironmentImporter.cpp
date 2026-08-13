#include "assets/environment/EnvironmentImporter.h"

#include "assets/environment/EnvironmentConvolution.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace Iridium {
namespace {

    void addError(std::vector<CookDiagnostic>& diagnostics,
        std::string code, std::string field, std::string message) {
        diagnostics.push_back({ CookDiagnosticSeverity::Error,
            std::move(code), std::move(field), std::move(message) });
    }

    bool validPowerOfTwo(uint32_t value, uint32_t maximum) {
        return value != 0 && value <= maximum && (value & (value - 1u)) == 0;
    }

    EnvironmentConvolutionSettings readSettings(const nlohmann::json& value) {
        return {
            .radianceSize = value.at("radiance_size").get<uint32_t>(),
            .irradianceSize = value.at("irradiance_size").get<uint32_t>(),
            .prefilteredSize = value.at("prefiltered_size").get<uint32_t>(),
            .brdfLutSize = value.at("brdf_lut_size").get<uint32_t>(),
            .prefilteredSamples = value.at("prefiltered_samples").get<uint32_t>(),
            .brdfSamples = value.at("brdf_samples").get<uint32_t>(),
            .sourcePrimaries = value.at("source_primaries").get<std::string>(),
            .sourceRadianceScale = value.at("radiance_scale").get<float>(),
        };
    }

    std::vector<std::byte> serializeImage(const EnvironmentFloatImage& image) {
        std::vector<std::byte> result(sizeof(uint32_t) * 2 +
            image.pixels.size() * sizeof(glm::vec4));
        std::memcpy(result.data(), &image.width, sizeof(uint32_t));
        std::memcpy(result.data() + sizeof(uint32_t), &image.height,
            sizeof(uint32_t));
        std::memcpy(result.data() + sizeof(uint32_t) * 2,
            image.pixels.data(), image.pixels.size() * sizeof(glm::vec4));
        return result;
    }

    EnvironmentFloatImage deserializeImage(std::span<const std::byte> bytes) {
        if (bytes.size() < sizeof(uint32_t) * 2) {
            throw std::runtime_error("Environment parsed image header is truncated.");
        }
        EnvironmentFloatImage result;
        std::memcpy(&result.width, bytes.data(), sizeof(uint32_t));
        std::memcpy(&result.height, bytes.data() + sizeof(uint32_t),
            sizeof(uint32_t));
        const uint64_t count = static_cast<uint64_t>(result.width) * result.height;
        if (result.width == 0 || result.height == 0 || count > (1ull << 30) ||
            bytes.size() != sizeof(uint32_t) * 2 + count * sizeof(glm::vec4)) {
            throw std::runtime_error("Environment parsed image layout is invalid.");
        }
        result.pixels.resize(static_cast<size_t>(count));
        std::memcpy(result.pixels.data(), bytes.data() + sizeof(uint32_t) * 2,
            result.pixels.size() * sizeof(glm::vec4));
        return result;
    }

} // namespace

const ImporterDescriptor& EnvironmentImporter::descriptor() const noexcept {
    static const ImporterDescriptor value{
        .id = "iridium.environment.hdri",
        .implementationVersion = 3,
        .currentSettingsSchemaVersion = 1,
        .assetTypes = { "iridium.environment" },
        .extensions = { ".hdr" },
    };
    return value;
}

ImportProbeResult EnvironmentImporter::probe(
    const std::filesystem::path& relativePath,
    std::span<const std::byte> sourceBytes) const {
    std::string extension = relativePath.extension().generic_string();
    std::ranges::transform(extension, extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".hdr" && !sourceBytes.empty()
        ? ImportProbeResult::Supported : ImportProbeResult::Unsupported;
}

NormalizedImportSettings EnvironmentImporter::normalizeSettings(
    uint32_t sourceSchemaVersion, const nlohmann::json& settings,
    bool strict) const {
    NormalizedImportSettings result;
    result.schemaVersion = descriptor().currentSettingsSchemaVersion;
    if (sourceSchemaVersion != result.schemaVersion) {
        addError(result.diagnostics, "ENVIRONMENT_SETTINGS_SCHEMA", "/schema",
            "Environment importer cannot migrate this settings schema.");
        return result;
    }
    if (!settings.is_object()) {
        addError(result.diagnostics, "ENVIRONMENT_SETTINGS_TYPE", "/",
            "Environment settings must be an object.");
        return result;
    }
    static const std::set<std::string> known{
        "radiance_size", "irradiance_size", "prefiltered_size",
        "brdf_lut_size", "prefiltered_samples", "brdf_samples",
        "source_primaries", "radiance_scale",
    };
    if (strict) {
        for (const auto& [key, ignored] : settings.items()) {
            (void)ignored;
            if (!known.contains(key)) {
                addError(result.diagnostics, "ENVIRONMENT_SETTINGS_UNKNOWN",
                    "/" + key, "Strict environment cooking rejects this setting.");
            }
        }
    }
    nlohmann::ordered_json canonical{
        { "brdf_lut_size", settings.value("brdf_lut_size", 256u) },
        { "brdf_samples", settings.value("brdf_samples", 1024u) },
        { "irradiance_size", settings.value("irradiance_size", 32u) },
        { "prefiltered_samples", settings.value("prefiltered_samples", 1024u) },
        { "prefiltered_size", settings.value("prefiltered_size", 1024u) },
        { "radiance_scale", settings.value("radiance_scale", 1.0f) },
        { "radiance_size", settings.value("radiance_size", 1024u) },
        { "source_primaries", settings.value("source_primaries",
            std::string("linear_rec709_d65")) },
    };
    const EnvironmentConvolutionSettings values = readSettings(canonical);
    if (!validPowerOfTwo(values.radianceSize, 4096) ||
        !validPowerOfTwo(values.irradianceSize, 256) ||
        !validPowerOfTwo(values.prefilteredSize, 2048) ||
        !validPowerOfTwo(values.brdfLutSize, 1024) ||
        values.prefilteredSamples == 0 || values.prefilteredSamples > 16384 ||
        values.brdfSamples == 0 || values.brdfSamples > 16384 ||
        !std::isfinite(values.sourceRadianceScale) ||
        values.sourceRadianceScale < 0.0f ||
        (values.sourcePrimaries != "linear_rec709_d65" &&
         values.sourcePrimaries != "acescg_ap1_d60")) {
        addError(result.diagnostics, "ENVIRONMENT_SETTINGS_DOMAIN", "/",
            "Environment sizes, samples, primaries, or radiance scale are invalid.");
        return result;
    }
    const std::string text = canonical.dump();
    result.canonicalBytes.assign(
        reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size()));
    result.values = nlohmann::json::parse(text);
    return result;
}

ParsedSourceAsset EnvironmentImporter::parse(
    const ImportSource& source,
    const NormalizedImportSettings& settings) const {
    ParsedSourceAsset result;
    if (!settings.valid() || source.stopToken.stop_requested()) {
        addError(result.diagnostics, "ENVIRONMENT_IMPORT_CANCELLED", "/",
            "Environment import was cancelled or settings are invalid.");
        return result;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    float* decoded = stbi_loadf_from_memory(
        reinterpret_cast<const stbi_uc*>(source.bytes.data()),
        static_cast<int>(source.bytes.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > 65536 || height > 32768) {
        if (decoded) stbi_image_free(decoded);
        addError(result.diagnostics, "ENVIRONMENT_SOURCE_DECODE",
            source.relativePath.generic_string(),
            "The HDRI could not be decoded as a finite equirectangular image.");
        return result;
    }
    EnvironmentFloatImage image{
        .width = static_cast<uint32_t>(width),
        .height = static_cast<uint32_t>(height),
        .pixels = std::vector<glm::vec4>(
            static_cast<size_t>(width) * height),
    };
    for (int y = 0; y < height; ++y) {
        const int sourceY = height - 1 - y;
        std::memcpy(image.pixels.data() + static_cast<size_t>(y) * width,
            decoded + static_cast<size_t>(sourceY) * width * 4,
            static_cast<size_t>(width) * sizeof(glm::vec4));
    }
    stbi_image_free(decoded);
    for (const glm::vec4 pixel : image.pixels) {
        if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) ||
            !std::isfinite(pixel.z) || pixel.x < 0.0f ||
            pixel.y < 0.0f || pixel.z < 0.0f) {
            addError(result.diagnostics, "ENVIRONMENT_SOURCE_DOMAIN", "/",
                "HDRI radiance must be finite and nonnegative.");
            return result;
        }
    }
    result.documentBytes = serializeImage(image);
    return result;
}

CookProduct EnvironmentImporter::cook(
    const ParsedSourceAsset& source,
    const NormalizedImportSettings& settings,
    const CookTarget& target,
    const AssetCookContext& context,
    std::stop_token stopToken) const {
    CookProduct failed{
        .artifactType = "iridium.environment",
        .artifactSchemaVersion = kCookedEnvironmentSchemaVersion,
    };
    if (!settings.valid() || hasCookErrors(source.diagnostics) ||
        stopToken.stop_requested()) {
        addError(failed.diagnostics, "ENVIRONMENT_COOK_INPUT", "/",
            "Environment cook input is invalid or cancelled.");
        return failed;
    }
    try {
        EnvironmentConvolutionSettings values = readSettings(settings.values);
        const EnvironmentFloatImage image = deserializeImage(source.documentBytes);
        float sourcePeak = 0.0f;
        for (const glm::vec4 pixel : image.pixels) {
            sourcePeak = (std::max)({ sourcePeak, pixel.x, pixel.y, pixel.z });
        }
        const ConvolvedEnvironment convolved =
            convolveEnvironmentReference(image, values, stopToken);
        if (stopToken.stop_requested()) {
            throw std::runtime_error("Environment cook was cancelled.");
        }
        CookProduct product = makeConvolvedEnvironmentProduct(
            context.assetGuid, convolved,
            values, "iridium-environment-importer-v3");
        if (static_cast<double>(sourcePeak) *
            values.sourceRadianceScale > 65504.0) {
            product.diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Warning,
                .code = "ENVIRONMENT_SOURCE_FP16_CLAMP",
                .field = "/radiance_scale",
                .message = "HDRI channels above finite FP16 range were saturated "
                    "at 65504; the remaining environment exposure was preserved.",
            });
        }
        return product;
    }
    catch (const std::exception& exception) {
        addError(failed.diagnostics, "ENVIRONMENT_COOK_FAILED", "/",
            exception.what());
        return failed;
    }
}

} // namespace Iridium
