#pragma once

#include "assets/cooker/ImporterRegistry.h"

namespace Iridium {

    inline constexpr uint32_t kDirectXTexCodecVersion = 20260508;
    inline constexpr const char* kDirectXTexCodecId =
        "microsoft.directxtex.cpu.4feb3e11a020f35b796fc769a74216a555d4f5ef";
    inline constexpr const char*
        kDirectXTexCodecContentHash =
            "9e1ad29041db6629ccab0d9d465a3e1a24a8ffb6e3d8edcc24e6a30545d0e71e";

    // Production Windows texture importer. Source decoding and canonical float
    // extraction happen in parse(); semantic mip processing and block compression
    // happen in cook(). No renderer or Vulkan type crosses this boundary.
    class TextureImporter final : public AssetImporter {
    public:
        [[nodiscard]] const ImporterDescriptor& descriptor() const noexcept override;
        [[nodiscard]] ImportProbeResult probe(
            const std::filesystem::path& relativePath,
            std::span<const std::byte> sourceBytes) const override;
        [[nodiscard]] NormalizedImportSettings normalizeSettings(
            uint32_t sourceSchemaVersion, const nlohmann::json& settings,
            bool strict) const override;
        [[nodiscard]] ParsedSourceAsset parse(
            const ImportSource& source,
            const NormalizedImportSettings& settings) const override;
        [[nodiscard]] CookProduct cook(
            const ParsedSourceAsset& source,
            const NormalizedImportSettings& settings,
            const CookTarget& target,
            const AssetCookContext& context,
            std::stop_token stopToken = {}) const override;
    };

} // namespace Iridium
