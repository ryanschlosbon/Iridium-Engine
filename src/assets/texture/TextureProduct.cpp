#include "assets/texture/TextureProduct.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace Iridium {

    namespace {
        using Json = nlohmann::json;

        template <typename Enum>
        std::optional<Enum> parseEnum(std::string_view value,
            std::initializer_list<std::pair<std::string_view, Enum>> values) {
            for (const auto& [name, result] : values) {
                if (value == name) return result;
            }
            return std::nullopt;
        }

        void error(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string field, std::string message) {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Error,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        std::string_view semanticName(TextureSemantic value) {
            switch (value) {
            case TextureSemantic::Color: return "color";
            case TextureSemantic::Normal: return "normal";
            case TextureSemantic::Scalar: return "scalar";
            case TextureSemantic::HdrColor: return "hdr_color";
            case TextureSemantic::Data: return "data";
            }
            return "data";
        }

        std::string_view qualityName(TextureCompressionQuality value) {
            switch (value) {
            case TextureCompressionQuality::Preview:
                return "preview";
            case TextureCompressionQuality::Iteration:
                return "iteration";
            case TextureCompressionQuality::Production:
                return "production";
            }
            return "iteration";
        }

        std::string_view mipName(TextureMipPolicy value) {
            switch (value) {
            case TextureMipPolicy::FullChain: return "full_chain";
            case TextureMipPolicy::PreserveSource: return "preserve_source";
            case TextureMipPolicy::None: return "none";
            }
            return "full_chain";
        }

        std::string_view alphaName(TextureAlphaMode value) {
            switch (value) {
            case TextureAlphaMode::Opaque: return "opaque";
            case TextureAlphaMode::Straight: return "straight";
            case TextureAlphaMode::Coverage: return "coverage";
            }
            return "opaque";
        }

        std::string_view colorSpaceName(TextureViewColorSpace value) {
            return value == TextureViewColorSpace::sRGB ? "srgb" : "linear";
        }
    }

    TextureSettingsResult canonicalizeTextureSettings(const nlohmann::json& source) {
        TextureSettingsResult result;
        if (!source.is_object()) {
            error(result.diagnostics, "TEXTURE_SETTINGS_TYPE", "/",
                "Texture import settings must be an object.");
            return result;
        }

        TextureImportSettings settings;
        const auto parseString = [&](const char* field, auto values, auto& destination) {
            const auto found = source.find(field);
            if (found == source.end()) return;
            if (!found->is_string()) {
                error(result.diagnostics, "TEXTURE_SETTINGS_FIELD_TYPE",
                    std::string("/") + field, "Expected a string.");
                return;
            }
            const auto parsed = parseEnum(found->get<std::string>(), values);
            if (!parsed) {
                error(result.diagnostics, "TEXTURE_SETTINGS_ENUM",
                    std::string("/") + field, "Unsupported value.");
                return;
            }
            destination = *parsed;
        };

        parseString("semantic",
            std::initializer_list<std::pair<std::string_view, TextureSemantic>>{
            std::pair{ std::string_view("color"), TextureSemantic::Color },
            { "normal", TextureSemantic::Normal },
            { "scalar", TextureSemantic::Scalar },
            { "hdr_color", TextureSemantic::HdrColor },
            { "data", TextureSemantic::Data },
        }, settings.semantic);
        parseString("quality",
            std::initializer_list<std::pair<std::string_view, TextureCompressionQuality>>{
            std::pair{ std::string_view("preview"), TextureCompressionQuality::Preview },
            { "iteration", TextureCompressionQuality::Iteration },
            { "production", TextureCompressionQuality::Production },
        }, settings.quality);
        parseString("mip_policy",
            std::initializer_list<std::pair<std::string_view, TextureMipPolicy>>{
            std::pair{ std::string_view("full_chain"), TextureMipPolicy::FullChain },
            { "preserve_source", TextureMipPolicy::PreserveSource },
            { "none", TextureMipPolicy::None },
        }, settings.mipPolicy);
        parseString("alpha_mode",
            std::initializer_list<std::pair<std::string_view, TextureAlphaMode>>{
            std::pair{ std::string_view("opaque"), TextureAlphaMode::Opaque },
            { "straight", TextureAlphaMode::Straight },
            { "coverage", TextureAlphaMode::Coverage },
        }, settings.alphaMode);
        parseString("view_color_space",
            std::initializer_list<std::pair<std::string_view, TextureViewColorSpace>>{
            std::pair{ std::string_view("linear"), TextureViewColorSpace::Linear },
            { "srgb", TextureViewColorSpace::sRGB },
        }, settings.viewColorSpace);

        const auto parseBool = [&](const char* field, bool& destination) {
            const auto found = source.find(field);
            if (found == source.end()) return;
            if (!found->is_boolean()) {
                error(result.diagnostics, "TEXTURE_SETTINGS_FIELD_TYPE",
                    std::string("/") + field, "Expected a boolean.");
                return;
            }
            destination = found->get<bool>();
        };
        parseBool("flip_green", settings.flipGreen);
        parseBool("reconstruct_normal_z", settings.reconstructNormalZ);

        if (const auto found = source.find("alpha_coverage_threshold");
            found != source.end()) {
            if (!found->is_number()) {
                error(result.diagnostics, "TEXTURE_SETTINGS_FIELD_TYPE",
                    "/alpha_coverage_threshold", "Expected a number.");
            } else {
                settings.alphaCoverageThreshold = found->get<float>();
                if (!std::isfinite(settings.alphaCoverageThreshold) ||
                    settings.alphaCoverageThreshold <= 0.0f ||
                    settings.alphaCoverageThreshold >= 1.0f) {
                    error(result.diagnostics, "TEXTURE_ALPHA_THRESHOLD",
                        "/alpha_coverage_threshold",
                        "Coverage threshold must be finite and between zero and one.");
                }
            }
        }

        if (settings.semantic != TextureSemantic::Color &&
            settings.semantic != TextureSemantic::HdrColor &&
            settings.viewColorSpace == TextureViewColorSpace::sRGB) {
            error(result.diagnostics, "TEXTURE_SEMANTIC_COLOR_SPACE",
                "/view_color_space",
                "Normal, scalar, and data textures require a linear view.");
        }
        if (settings.semantic == TextureSemantic::HdrColor &&
            settings.viewColorSpace != TextureViewColorSpace::Linear) {
            error(result.diagnostics, "TEXTURE_HDR_COLOR_SPACE",
                "/view_color_space", "HDR texture products require a linear view.");
        }
        if (settings.alphaMode == TextureAlphaMode::Coverage &&
            settings.mipPolicy == TextureMipPolicy::None) {
            error(result.diagnostics, "TEXTURE_COVERAGE_MIPS",
                "/alpha_mode", "Coverage preservation requires mip generation.");
        }
        if (hasCookErrors(result.diagnostics)) return result;

        Json canonical = {
            { "alpha_coverage_threshold", settings.alphaCoverageThreshold },
            { "alpha_mode", alphaName(settings.alphaMode) },
            { "flip_green", settings.flipGreen },
            { "mip_policy", mipName(settings.mipPolicy) },
            { "quality", qualityName(settings.quality) },
            { "reconstruct_normal_z", settings.reconstructNormalZ },
            { "schema", 1 },
            { "semantic", semanticName(settings.semantic) },
            { "view_color_space", colorSpaceName(settings.viewColorSpace) },
        };
        const std::string text = canonical.dump();
        result.canonicalBytes.assign(
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()));
        result.settings = settings;
        return result;
    }

    TextureFormat selectTextureProductFormat(
        const TextureImportSettings& settings) noexcept {
        if (settings.quality ==
            TextureCompressionQuality::Preview) {
            if (settings.semantic ==
                TextureSemantic::HdrColor) {
                return TextureFormat::RGBA16_SFloat;
            }
            return settings.semantic ==
                    TextureSemantic::Color &&
                settings.viewColorSpace ==
                    TextureViewColorSpace::sRGB
                ? TextureFormat::RGBA8_sRGB
                : TextureFormat::RGBA8_UNorm;
        }
        switch (settings.semantic) {
        case TextureSemantic::Color:
            return settings.viewColorSpace == TextureViewColorSpace::sRGB
                ? TextureFormat::BC7_sRGB : TextureFormat::BC7_UNorm;
        case TextureSemantic::Normal: return TextureFormat::BC5_UNorm;
        case TextureSemantic::Scalar: return TextureFormat::BC4_UNorm;
        case TextureSemantic::HdrColor: return TextureFormat::BC6H_UFloat;
        case TextureSemantic::Data: return TextureFormat::BC7_UNorm;
        }
        return TextureFormat::BC7_UNorm;
    }

    std::vector<std::byte> serializeTextureManifest(
        const CookedTextureManifest& manifest) {
        Json root = {
            { "alpha_mode", alphaName(manifest.alphaMode) },
            { "codec_id", manifest.codecId },
            { "codec_version", manifest.codecVersion },
            { "height", manifest.height },
            { "mips", Json::array() },
            { "quality", qualityName(manifest.quality) },
            { "schema", manifest.schemaVersion },
            { "semantic", semanticName(manifest.semantic) },
            { "storage_format", static_cast<uint32_t>(manifest.storageFormat) },
            { "view_color_space", colorSpaceName(manifest.viewColorSpace) },
            { "width", manifest.width },
        };
        for (const CookedTextureMip& mip : manifest.mips) {
            root["mips"].push_back({
                { "byte_offset", mip.byteOffset },
                { "byte_size", mip.byteSize },
                { "height", mip.height },
                { "width", mip.width },
            });
        }
        const std::string text = root.dump();
        return {
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()),
        };
    }

    std::optional<CookedTextureManifest> readTextureManifest(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics) {
        try {
            const Json root = Json::parse(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + bytes.size()));
            CookedTextureManifest result;
            result.schemaVersion = root.at("schema").get<uint32_t>();
            result.width = root.at("width").get<uint32_t>();
            result.height = root.at("height").get<uint32_t>();
            result.storageFormat =
                static_cast<TextureFormat>(root.at("storage_format").get<uint32_t>());
            result.codecId = root.at("codec_id").get<std::string>();
            result.codecVersion = root.at("codec_version").get<uint32_t>();
            const auto semantic = parseEnum<TextureSemantic>(
                root.at("semantic").get<std::string>(), {
                    { "color", TextureSemantic::Color },
                    { "normal", TextureSemantic::Normal },
                    { "scalar", TextureSemantic::Scalar },
                    { "hdr_color", TextureSemantic::HdrColor },
                    { "data", TextureSemantic::Data },
                });
            const auto quality = parseEnum<TextureCompressionQuality>(
                root.at("quality").get<std::string>(), {
                    { "preview", TextureCompressionQuality::Preview },
                    { "iteration", TextureCompressionQuality::Iteration },
                    { "production", TextureCompressionQuality::Production },
                });
            const auto alpha = parseEnum<TextureAlphaMode>(
                root.at("alpha_mode").get<std::string>(), {
                    { "opaque", TextureAlphaMode::Opaque },
                    { "straight", TextureAlphaMode::Straight },
                    { "coverage", TextureAlphaMode::Coverage },
                });
            const auto colorSpace = parseEnum<TextureViewColorSpace>(
                root.at("view_color_space").get<std::string>(), {
                    { "linear", TextureViewColorSpace::Linear },
                    { "srgb", TextureViewColorSpace::sRGB },
                });
            if (!semantic || !quality || !alpha || !colorSpace) {
                throw std::runtime_error("manifest contains an unknown enum value");
            }
            result.semantic = *semantic;
            result.quality = *quality;
            result.alphaMode = *alpha;
            result.viewColorSpace = *colorSpace;
            for (const Json& mip : root.at("mips")) {
                result.mips.push_back({
                    .width = mip.at("width").get<uint32_t>(),
                    .height = mip.at("height").get<uint32_t>(),
                    .byteOffset = mip.at("byte_offset").get<uint64_t>(),
                    .byteSize = mip.at("byte_size").get<uint64_t>(),
                });
            }
            return result;
        } catch (const std::exception& exception) {
            error(diagnostics, "TEXTURE_MANIFEST_PARSE", "/",
                std::string("Invalid cooked texture manifest: ") + exception.what());
            return std::nullopt;
        }
    }

    std::vector<CookDiagnostic> validateTextureProduct(
        const CookedTextureManifest& manifest, uint64_t payloadSize) {
        std::vector<CookDiagnostic> diagnostics;
        if (manifest.schemaVersion != kCookedTextureSchemaVersion) {
            error(diagnostics, "TEXTURE_SCHEMA", "/schema",
                "Unsupported cooked texture schema.");
        }
        if (manifest.width == 0 || manifest.height == 0 || manifest.mips.empty()) {
            error(diagnostics, "TEXTURE_DIMENSIONS", "/mips",
                "Cooked texture dimensions and mip chain must be non-empty.");
            return diagnostics;
        }
        if (manifest.storageFormat != selectTextureProductFormat({
            .semantic = manifest.semantic,
            .quality = manifest.quality,
            .alphaMode = manifest.alphaMode,
            .viewColorSpace = manifest.viewColorSpace,
        })) {
            error(diagnostics, "TEXTURE_FORMAT_SEMANTICS", "/storage_format",
                "Storage format does not match semantic and view color space.");
        }
        uint64_t expectedOffset = 0;
        uint32_t width = manifest.width;
        uint32_t height = manifest.height;
        for (size_t index = 0; index < manifest.mips.size(); ++index) {
            const CookedTextureMip& mip = manifest.mips[index];
            if (mip.width != width || mip.height != height ||
                mip.byteOffset != expectedOffset ||
                mip.byteSize != textureMipDataSize(manifest.storageFormat, width, height)) {
                error(diagnostics, "TEXTURE_MIP_LAYOUT",
                    "/mips/" + std::to_string(index),
                    "Mip dimensions, offset, or block-compressed byte size is invalid.");
                break;
            }
            expectedOffset += mip.byteSize;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        if (expectedOffset != payloadSize) {
            error(diagnostics, "TEXTURE_PAYLOAD_SIZE", "/mips",
                "Mip layout does not cover the payload exactly.");
        }
        if (manifest.codecId.empty() || manifest.codecVersion == 0) {
            error(diagnostics, "TEXTURE_CODEC_IDENTITY", "/codec_id",
                "Codec identity and version are required.");
        }
        return diagnostics;
    }

    CookProduct makeCookedTextureProduct(
        const CookedTextureManifest& manifest,
        std::span<const std::byte> payload) {
        CookProduct product{
            .artifactType = "iridium.texture",
            .artifactSchemaVersion = kCookedTextureSchemaVersion,
        };
        product.diagnostics = validateTextureProduct(manifest, payload.size());
        if (hasCookErrors(product.diagnostics)) return product;
        product.sections = {
            {
                .id = kCookedTextureManifestSection,
                .schemaVersion = kCookedTextureSchemaVersion,
                .alignment = 16,
                .bytes = serializeTextureManifest(manifest),
            },
            {
                .id = kCookedTexturePayloadSection,
                .schemaVersion = kCookedTextureSchemaVersion,
                .alignment = 256,
                .bytes = { payload.begin(), payload.end() },
            },
        };
        return product;
    }

} // namespace Iridium
