#pragma once

#include "assets/cooker/ImporterRegistry.h"

namespace Iridium {

    class EnvironmentImporter final : public AssetImporter {
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
