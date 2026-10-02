#pragma once

#include "assets/AssetGuid.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Iridium {

    enum class CookDiagnosticSeverity : uint8_t {
        Info,
        Warning,
        Error,
    };

    struct CookDiagnostic {
        CookDiagnosticSeverity severity = CookDiagnosticSeverity::Error;
        std::string code;
        std::string field;
        std::string message;
    };

    [[nodiscard]] bool hasCookErrors(
        const std::vector<CookDiagnostic>& diagnostics) noexcept;

    enum class AssetDependencyType : uint8_t {
        SourceFile,
        Asset,
        Tool,
        OptionalAsset,
    };

    struct AssetDependency {
        AssetDependencyType type = AssetDependencyType::SourceFile;
        std::optional<AssetGuid> assetGuid;
        std::string location;
        std::string contentHash;
        std::string artifactHash;

        auto operator<=>(const AssetDependency&) const = default;
    };

    struct CookTarget {
        std::string platform;
        std::string profile;
        std::string qualityPolicy;
        uint32_t artifactContainerVersion = 1;
        uint32_t materialSchemaVersion = 2;

        auto operator<=>(const CookTarget&) const = default;
    };

    struct CookSection {
        uint32_t id = 0;
        uint32_t schemaVersion = 1;
        uint32_t alignment = 1;
        std::vector<std::byte> bytes;

        auto operator<=>(const CookSection&) const = default;
    };

    struct CookProduct {
        std::string artifactType;
        uint32_t artifactSchemaVersion = 1;
        std::vector<CookSection> sections;
        std::vector<CookDiagnostic> diagnostics;
    };

} // namespace Iridium
