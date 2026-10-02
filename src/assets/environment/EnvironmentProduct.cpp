#include "assets/environment/EnvironmentProduct.h"

#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Iridium {
namespace {

    using Json = nlohmann::json;

    void error(std::vector<CookDiagnostic>& diagnostics, std::string code,
        std::string field, std::string message) {
        diagnostics.push_back({ CookDiagnosticSeverity::Error,
            std::move(code), std::move(field), std::move(message) });
    }

    Json imageJson(const EnvironmentImageProductDesc& desc) {
        return {
            { "array_layers", desc.arrayLayers },
            { "format", static_cast<uint32_t>(desc.format) },
            { "height", desc.height },
            { "mip_levels", desc.mipLevels },
            { "width", desc.width },
        };
    }

    EnvironmentImageProductDesc readImage(const Json& source) {
        return {
            .width = source.at("width").get<uint32_t>(),
            .height = source.at("height").get<uint32_t>(),
            .mipLevels = source.at("mip_levels").get<uint32_t>(),
            .arrayLayers = source.at("array_layers").get<uint32_t>(),
            .format = static_cast<TextureFormat>(
                source.at("format").get<uint32_t>()),
        };
    }

    void validateImage(std::vector<CookDiagnostic>& diagnostics,
        std::string_view field, const EnvironmentImageProductDesc& desc,
        bool cube, size_t payloadSize) {
        TextureDesc texture{
            .width = desc.width,
            .height = desc.height,
            .format = desc.format,
            .usageClass = cube ? TextureUsageClass::Environment :
                TextureUsageClass::Sampled2D,
            .mipLevels = desc.mipLevels,
            .arrayLayers = desc.arrayLayers,
            .topology = cube ? TextureTopology::Cube : TextureTopology::Texture2D,
        };
        if (!validTextureTopology(texture)) {
            error(diagnostics, "ENVIRONMENT_IMAGE_LAYOUT", std::string(field),
                "Environment image dimensions, layers, mips, or topology are invalid.");
            return;
        }
        if (bytesPerBlock(desc.format) == 0 ||
            textureDataSize(texture) != payloadSize) {
            error(diagnostics, "ENVIRONMENT_PAYLOAD_SIZE", std::string(field),
                "Environment payload does not exactly cover every layer and mip.");
        }
    }

    void validateFiniteFp16(std::vector<CookDiagnostic>& diagnostics,
        std::string_view field, TextureFormat format,
        std::span<const std::byte> payload) {
        if (format != TextureFormat::RGBA16_SFloat &&
            format != TextureFormat::RG16_SFloat)
            return;
        if (payload.size() % sizeof(uint16_t) != 0) return;
        for (size_t offset = 0; offset < payload.size(); offset += sizeof(uint16_t)) {
            uint16_t value = 0;
            std::memcpy(&value, payload.data() + offset, sizeof(value));
            const bool negative = (value & 0x8000u) != 0 &&
                (value & 0x7fffu) != 0;
            const bool nonfinite = (value & 0x7c00u) == 0x7c00u;
            if (negative || nonfinite) {
                error(diagnostics, "ENVIRONMENT_FP16_DOMAIN", std::string(field),
                    "Environment FP16 payload must be finite and nonnegative.");
                return;
            }
        }
    }

} // namespace

uint64_t environmentProductByteSize(
    const EnvironmentImageProductDesc& desc) noexcept {
    return textureDataSize({
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .mipLevels = desc.mipLevels,
        .arrayLayers = desc.arrayLayers,
        .topology = desc.arrayLayers == 6 ? TextureTopology::Cube :
            TextureTopology::Texture2D,
    });
}

std::vector<std::byte> serializeEnvironmentManifest(
    const CookedEnvironmentManifest& manifest) {
    const Json root{
        { "brdf_lut", imageJson(manifest.brdfLut) },
        { "convolution_implementation", manifest.convolutionImplementation },
        { "irradiance", imageJson(manifest.irradiance) },
        { "orientation", manifest.orientation },
        { "prefiltered_specular", imageJson(manifest.prefilteredSpecular) },
        { "radiance", imageJson(manifest.radiance) },
        { "roughness_mip_convention", manifest.roughnessMipConvention },
        { "sample_sequence", manifest.sampleSequence },
        { "schema", manifest.schemaVersion },
        { "source_primaries", manifest.sourcePrimaries },
        { "source_radiance_scale", manifest.sourceRadianceScale },
        { "source_texture_guid", manifest.sourceTextureGuid.toString() },
        { "tool_version", manifest.toolVersion },
    };
    const std::string text = root.dump();
    return { reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size()) };
}

std::optional<CookedEnvironmentManifest> readEnvironmentManifest(
    std::span<const std::byte> bytes,
    std::vector<CookDiagnostic>& diagnostics) {
    try {
        const Json root = Json::parse(
            reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        const auto guid = AssetGuid::parse(
            root.at("source_texture_guid").get<std::string>());
        if (!guid) throw std::runtime_error("source texture GUID is invalid");
        CookedEnvironmentManifest result;
        result.schemaVersion = root.at("schema").get<uint32_t>();
        result.sourceTextureGuid = *guid;
        result.sourcePrimaries = root.at("source_primaries").get<std::string>();
        result.sourceRadianceScale =
            root.at("source_radiance_scale").get<float>();
        result.orientation = root.at("orientation").get<std::string>();
        result.convolutionImplementation =
            root.at("convolution_implementation").get<std::string>();
        result.sampleSequence = root.at("sample_sequence").get<std::string>();
        result.roughnessMipConvention =
            root.at("roughness_mip_convention").get<std::string>();
        result.toolVersion = root.at("tool_version").get<std::string>();
        result.radiance = readImage(root.at("radiance"));
        result.irradiance = readImage(root.at("irradiance"));
        result.prefilteredSpecular = readImage(root.at("prefiltered_specular"));
        result.brdfLut = readImage(root.at("brdf_lut"));
        return result;
    } catch (const std::exception& exception) {
        error(diagnostics, "ENVIRONMENT_MANIFEST_PARSE", "/",
            std::string("Invalid cooked environment manifest: ") +
                exception.what());
        return std::nullopt;
    }
}

std::vector<CookDiagnostic> validateEnvironmentProduct(
    const CookedEnvironmentManifest& manifest,
    const EnvironmentPayloads& payloads) {
    std::vector<CookDiagnostic> diagnostics;
    if (manifest.schemaVersion != kCookedEnvironmentSchemaVersion)
        error(diagnostics, "ENVIRONMENT_SCHEMA", "/schema",
            "Unsupported cooked environment schema.");
    if (manifest.sourceTextureGuid.isNil())
        error(diagnostics, "ENVIRONMENT_SOURCE", "/source_texture_guid",
            "A stable source texture GUID is required.");
    if (manifest.sourcePrimaries != "linear_rec709_d65" &&
        manifest.sourcePrimaries != "acescg_ap1_d60")
        error(diagnostics, "ENVIRONMENT_SOURCE_PRIMARIES", "/source_primaries",
            "Source primaries must be linear Rec.709/D65 or ACEScg AP1/D60.");
    if (!std::isfinite(manifest.sourceRadianceScale) ||
        manifest.sourceRadianceScale < 0.0f)
        error(diagnostics, "ENVIRONMENT_RADIANCE_SCALE", "/source_radiance_scale",
            "Source radiance scale must be finite and nonnegative.");
    if (manifest.orientation != kEnvironmentCubeOrientation)
        error(diagnostics, "ENVIRONMENT_ORIENTATION", "/orientation",
            "Cube orientation does not match the frozen Vulkan convention.");
    if (manifest.roughnessMipConvention !=
        kEnvironmentRoughnessMipConvention)
        error(diagnostics, "ENVIRONMENT_ROUGHNESS_MIPS",
            "/roughness_mip_convention",
            "Prefilter roughness-to-mip mapping is unsupported.");
    if (manifest.convolutionImplementation.empty() ||
        manifest.sampleSequence.empty() || manifest.toolVersion.empty())
        error(diagnostics, "ENVIRONMENT_PROVENANCE", "/",
            "Convolution implementation, sample sequence, and tool version are required.");
    validateImage(diagnostics, "/radiance", manifest.radiance, true,
        payloads.radiance.size());
    validateFiniteFp16(diagnostics, "/radiance", manifest.radiance.format,
        payloads.radiance);
    validateImage(diagnostics, "/irradiance", manifest.irradiance, true,
        payloads.irradiance.size());
    validateFiniteFp16(diagnostics, "/irradiance", manifest.irradiance.format,
        payloads.irradiance);
    validateImage(diagnostics, "/prefiltered_specular",
        manifest.prefilteredSpecular, true,
        payloads.prefilteredSpecular.size());
    validateFiniteFp16(diagnostics, "/prefiltered_specular",
        manifest.prefilteredSpecular.format, payloads.prefilteredSpecular);
    validateImage(diagnostics, "/brdf_lut", manifest.brdfLut, false,
        payloads.brdfLut.size());
    validateFiniteFp16(diagnostics, "/brdf_lut", manifest.brdfLut.format,
        payloads.brdfLut);
    if (manifest.brdfLut.format != TextureFormat::RG16_SFloat)
        error(diagnostics, "ENVIRONMENT_BRDF_FORMAT", "/brdf_lut/format",
            "The split-sum BRDF LUT must use two-channel FP16 storage.");
    return diagnostics;
}

CookProduct makeCookedEnvironmentProduct(
    const CookedEnvironmentManifest& manifest,
    const EnvironmentPayloads& payloads) {
    CookProduct result{
        .artifactType = "iridium.environment",
        .artifactSchemaVersion = kCookedEnvironmentSchemaVersion,
    };
    result.diagnostics = validateEnvironmentProduct(manifest, payloads);
    if (hasCookErrors(result.diagnostics)) return result;
    result.sections = {
        { kCookedEnvironmentManifestSection, kCookedEnvironmentSchemaVersion,
            16, serializeEnvironmentManifest(manifest) },
        { kCookedEnvironmentRadianceSection, kCookedEnvironmentSchemaVersion,
            256, { payloads.radiance.begin(), payloads.radiance.end() } },
        { kCookedEnvironmentIrradianceSection, kCookedEnvironmentSchemaVersion,
            256, { payloads.irradiance.begin(), payloads.irradiance.end() } },
        { kCookedEnvironmentPrefilterSection, kCookedEnvironmentSchemaVersion,
            256, { payloads.prefilteredSpecular.begin(),
                payloads.prefilteredSpecular.end() } },
        { kCookedEnvironmentBrdfSection, kCookedEnvironmentSchemaVersion,
            256, { payloads.brdfLut.begin(), payloads.brdfLut.end() } },
    };
    return result;
}

CookedEnvironmentReadResult readCookedEnvironmentProduct(
    const CookedArtifact& artifact) {
    CookedEnvironmentReadResult result;
    if (artifact.artifactType != "iridium.environment" ||
        artifact.artifactSchemaVersion != kCookedEnvironmentSchemaVersion) {
        error(result.diagnostics, "ENVIRONMENT_ARTIFACT_TYPE", "/",
            "Cooked artifact is not a supported environment product.");
        return result;
    }
    std::map<uint32_t, const CookSection*> sections;
    for (const CookSection& section : artifact.sections) {
        if (!sections.emplace(section.id, &section).second)
            error(result.diagnostics, "ENVIRONMENT_SECTION_DUPLICATE", "/sections",
                "Cooked environment section IDs must be unique.");
    }
    const uint32_t required[]{ kCookedEnvironmentManifestSection,
        kCookedEnvironmentRadianceSection, kCookedEnvironmentIrradianceSection,
        kCookedEnvironmentPrefilterSection, kCookedEnvironmentBrdfSection };
    for (uint32_t id : required)
        if (!sections.contains(id))
            error(result.diagnostics, "ENVIRONMENT_SECTION_MISSING", "/sections",
                "Cooked environment is missing a required section.");
    if (sections.size() != std::size(required))
        error(result.diagnostics, "ENVIRONMENT_SECTION_UNKNOWN", "/sections",
            "Cooked environment contains an unknown section.");
    for (uint32_t id : required)
        if (sections.contains(id) &&
            sections.at(id)->schemaVersion != kCookedEnvironmentSchemaVersion)
            error(result.diagnostics, "ENVIRONMENT_SECTION_SCHEMA", "/sections",
                "Cooked environment section schema is unsupported.");
    if (hasCookErrors(result.diagnostics)) return result;

    const auto manifest = readEnvironmentManifest(
        sections.at(kCookedEnvironmentManifestSection)->bytes,
        result.diagnostics);
    if (!manifest) return result;
    CookedEnvironmentProductData product{
        .manifest = *manifest,
        .radiance = sections.at(kCookedEnvironmentRadianceSection)->bytes,
        .irradiance = sections.at(kCookedEnvironmentIrradianceSection)->bytes,
        .prefilteredSpecular =
            sections.at(kCookedEnvironmentPrefilterSection)->bytes,
        .brdfLut = sections.at(kCookedEnvironmentBrdfSection)->bytes,
    };
    const EnvironmentPayloads payloads{ product.radiance, product.irradiance,
        product.prefilteredSpecular, product.brdfLut };
    std::vector<CookDiagnostic> validation =
        validateEnvironmentProduct(product.manifest, payloads);
    result.diagnostics.insert(result.diagnostics.end(), validation.begin(),
        validation.end());
    if (!hasCookErrors(result.diagnostics)) result.data = std::move(product);
    return result;
}

} // namespace Iridium
