#pragma once

#include "assets/AssetGuid.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Iridium {

    inline constexpr uint32_t kAssetMetadataSchemaVersion = 1;

    struct SubassetMetadata {
        AssetGuid guid;
        std::string assetType;
        std::string sourceKey;
        std::string structuralFingerprint;

        auto operator<=>(const SubassetMetadata&) const = default;
    };

    struct AssetMetadata {
        uint32_t schemaVersion = kAssetMetadataSchemaVersion;
        AssetGuid assetGuid;
        std::string assetType;
        std::string importerId;
        uint32_t importerVersion = 0;
        uint32_t settingsSchemaVersion = 1;
        nlohmann::json settings = nlohmann::json::object();
        std::vector<SubassetMetadata> subassets;
        std::vector<std::string> tags;
    };

    enum class AssetMetadataSeverity : uint8_t {
        Warning,
        Error,
    };

    struct AssetMetadataDiagnostic {
        AssetMetadataSeverity severity = AssetMetadataSeverity::Error;
        std::string code;
        std::string field;
        std::string message;
    };

    struct AssetMetadataReadResult {
        std::optional<AssetMetadata> metadata;
        std::vector<AssetMetadataDiagnostic> diagnostics;

        [[nodiscard]] bool hasErrors() const noexcept;
    };

    [[nodiscard]] std::filesystem::path assetMetadataSidecarPath(
        const std::filesystem::path& sourcePath);
    [[nodiscard]] std::string serializeAssetMetadata(const AssetMetadata& metadata);
    [[nodiscard]] AssetMetadataReadResult parseAssetMetadata(std::string_view text);
    [[nodiscard]] AssetMetadataReadResult readAssetMetadata(
        const std::filesystem::path& sidecarPath);
    [[nodiscard]] bool writeAssetMetadataAtomic(const std::filesystem::path& sidecarPath,
        const AssetMetadata& metadata, std::string& error);

    enum class SubassetMatchMethod : uint8_t {
        ExactSourceKey,
        UniqueStructuralFingerprint,
        NewSubasset,
        Ambiguous,
    };

    struct DiscoveredSubasset {
        std::string assetType;
        std::string sourceKey;
        std::string structuralFingerprint;
    };

    struct SubassetMatch {
        DiscoveredSubasset discovered;
        std::optional<AssetGuid> existingGuid;
        SubassetMatchMethod method = SubassetMatchMethod::NewSubasset;
        std::vector<AssetGuid> ambiguousCandidates;
    };

    [[nodiscard]] std::vector<SubassetMatch> matchSubassets(
        std::span<const SubassetMetadata> previous,
        std::span<const DiscoveredSubasset> discovered);

} // namespace Iridium
