#pragma once

#include "assets/cooker/CookTypes.h"
#include "renderer/rhi/TextureTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Iridium {

    inline constexpr uint32_t kCookedTextureSchemaVersion = 1;
    inline constexpr uint32_t kCookedTextureManifestSection = 0x54584d31; // TXM1
    inline constexpr uint32_t kCookedTexturePayloadSection = 0x54585031;  // TXP1
    inline constexpr uint32_t
        kEditorPreviewTextureMaxDimension = 512;

    enum class TextureSemantic : uint8_t {
        Color,
        Normal,
        Scalar,
        HdrColor,
        Data,
    };

    enum class TextureCompressionQuality : uint8_t {
        Preview,
        Iteration,
        Production,
    };

    enum class TextureMipPolicy : uint8_t {
        FullChain,
        PreserveSource,
        None,
    };

    enum class TextureAlphaMode : uint8_t {
        Opaque,
        Straight,
        Coverage,
    };

    struct TextureImportSettings {
        TextureSemantic semantic = TextureSemantic::Color;
        TextureCompressionQuality quality = TextureCompressionQuality::Iteration;
        TextureMipPolicy mipPolicy = TextureMipPolicy::FullChain;
        TextureAlphaMode alphaMode = TextureAlphaMode::Opaque;
        TextureViewColorSpace viewColorSpace = TextureViewColorSpace::sRGB;
        float alphaCoverageThreshold = 0.5f;
        bool flipGreen = false;
        bool reconstructNormalZ = true;

        constexpr bool operator==(const TextureImportSettings&) const noexcept = default;
    };

    struct CookedTextureMip {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t byteOffset = 0;
        uint64_t byteSize = 0;

        auto operator<=>(const CookedTextureMip&) const = default;
    };

    struct CookedTextureManifest {
        uint32_t schemaVersion = kCookedTextureSchemaVersion;
        uint32_t width = 0;
        uint32_t height = 0;
        TextureFormat storageFormat = TextureFormat::RGBA8_UNorm;
        TextureViewColorSpace viewColorSpace = TextureViewColorSpace::Linear;
        TextureSemantic semantic = TextureSemantic::Data;
        TextureCompressionQuality quality = TextureCompressionQuality::Iteration;
        TextureAlphaMode alphaMode = TextureAlphaMode::Opaque;
        std::string codecId;
        uint32_t codecVersion = 0;
        std::vector<CookedTextureMip> mips;

        auto operator<=>(const CookedTextureManifest&) const = default;
    };

    struct TextureSettingsResult {
        std::optional<TextureImportSettings> settings;
        std::vector<std::byte> canonicalBytes;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return settings.has_value() && !hasCookErrors(diagnostics);
        }
    };

    [[nodiscard]] TextureSettingsResult canonicalizeTextureSettings(
        const nlohmann::json& source);
    [[nodiscard]] TextureFormat selectTextureProductFormat(
        const TextureImportSettings& settings) noexcept;
    [[nodiscard]] std::vector<std::byte> serializeTextureManifest(
        const CookedTextureManifest& manifest);
    [[nodiscard]] std::optional<CookedTextureManifest> readTextureManifest(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<CookDiagnostic> validateTextureProduct(
        const CookedTextureManifest& manifest, uint64_t payloadSize);
    [[nodiscard]] CookProduct makeCookedTextureProduct(
        const CookedTextureManifest& manifest,
        std::span<const std::byte> payload);

} // namespace Iridium
