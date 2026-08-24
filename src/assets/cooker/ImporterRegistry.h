#pragma once

#include "assets/AssetMetadata.h"
#include "assets/cooker/CanonicalSettings.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Iridium {

    class DerivedDataCache;

    enum class ImportProbeResult : uint8_t {
        Unsupported,
        Supported,
    };

    struct ImporterDescriptor {
        std::string id;
        uint32_t implementationVersion = 0;
        uint32_t currentSettingsSchemaVersion = 0;
        std::vector<std::string> assetTypes;
        std::vector<std::string> extensions;

        auto operator<=>(const ImporterDescriptor&) const = default;
    };

    struct NormalizedImportSettings {
        uint32_t schemaVersion = 0;
        nlohmann::json values = nlohmann::json::object();
        std::vector<std::byte> canonicalBytes;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return !hasCookErrors(diagnostics);
        }
    };

    struct ParsedSourceAsset {
        struct SubassetPayload {
            std::string sourceKey;
            std::filesystem::path suggestedPath;
            std::vector<std::byte> bytes;
            std::vector<std::byte> parsedBytes;
        };

        std::vector<std::byte> documentBytes;
        std::vector<SubassetPayload> subassetPayloads;
        std::vector<AssetDependency> dependencies;
        std::vector<DiscoveredSubasset> discoveredSubassets;
        std::vector<CookDiagnostic> diagnostics;
    };

    struct ImportSource {
        std::filesystem::path relativePath;
        std::filesystem::path resolvedPath;
        std::span<const std::byte> bytes;
        std::stop_token stopToken;
        bool metadataOnly = false;
    };

    struct AssetCookContext {
        struct Progress {
            std::string stage;
            uint64_t completed = 0;
            uint64_t total = 0;
            std::string detail;
        };

        AssetGuid assetGuid;
        std::vector<SubassetMetadata> subassets;
        // Optional cooker-owned cache for deterministic derived subproducts.
        // Importers may use it to reuse expensive immutable work, but product
        // correctness and bytes must never depend on cache availability.
        DerivedDataCache* derivedDataCache = nullptr;
        // Optional thread-safe observer for coarse cook stages and bounded
        // sub-work progress. Importers may call this from worker threads.
        std::function<void(const Progress&)> progress;
    };

    class AssetImporter {
    public:
        virtual ~AssetImporter() = default;

        [[nodiscard]] virtual const ImporterDescriptor& descriptor() const noexcept = 0;
        [[nodiscard]] virtual ImportProbeResult probe(
            const std::filesystem::path& relativePath,
            std::span<const std::byte> sourceBytes) const = 0;
        [[nodiscard]] virtual NormalizedImportSettings normalizeSettings(
            uint32_t sourceSchemaVersion, const nlohmann::json& settings,
            bool strict) const = 0;
        [[nodiscard]] virtual ParsedSourceAsset parse(
            const ImportSource& source,
            const NormalizedImportSettings& settings) const = 0;
        [[nodiscard]] virtual CookProduct cook(
            const ParsedSourceAsset& source,
            const NormalizedImportSettings& settings,
            const CookTarget& target,
            const AssetCookContext& context,
            std::stop_token stopToken = {}) const = 0;
    };

    struct ImporterSelection {
        std::shared_ptr<const AssetImporter> importer;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return importer != nullptr && !hasCookErrors(diagnostics);
        }
    };

    class ImporterRegistry {
    public:
        void registerImporter(std::shared_ptr<const AssetImporter> importer);

        [[nodiscard]] std::vector<ImporterDescriptor> descriptors() const;
        [[nodiscard]] ImporterSelection selectExplicit(
            std::string_view importerId, uint32_t implementationVersion) const;
        [[nodiscard]] ImporterSelection selectAutomatic(
            const std::filesystem::path& relativePath,
            std::span<const std::byte> sourceBytes) const;

    private:
        std::vector<std::shared_ptr<const AssetImporter>> m_importers;
    };

} // namespace Iridium
