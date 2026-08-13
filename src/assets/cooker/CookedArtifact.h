#pragma once

#include "assets/cooker/CookTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kCookedArtifactContainerVersion = 1;
    inline constexpr size_t kCookedArtifactHeaderSize = 240;

    struct CookedArtifact {
        AssetGuid assetGuid;
        std::string artifactType;
        uint32_t artifactSchemaVersion = 1;
        CookTarget target;
        std::string cookKey;
        std::vector<AssetDependency> dependencies;
        std::vector<CookSection> sections;
    };

    struct CookedArtifactBlob {
        std::vector<std::byte> bytes;
        std::string artifactHash;
    };

    struct CookedArtifactReadResult {
        std::optional<CookedArtifact> artifact;
        std::string artifactHash;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return artifact.has_value() && !hasCookErrors(diagnostics);
        }
    };

    struct CookedArtifactHeaderProbe {
        bool valid = false;
        std::string cookKey;
        uint64_t totalSize = 0;
        std::vector<CookDiagnostic> diagnostics;
    };

    [[nodiscard]] CookedArtifactBlob serializeCookedArtifact(
        const CookedArtifact& artifact);
    [[nodiscard]] CookedArtifactBlob readCookedArtifactBlobFile(
        const std::filesystem::path& path);
    [[nodiscard]] CookedArtifactReadResult readCookedArtifact(
        std::span<const std::byte> bytes,
        std::optional<std::string_view> expectedArtifactHash = std::nullopt);
    [[nodiscard]] CookedArtifactHeaderProbe probeCookedArtifactHeader(
        std::span<const std::byte> headerBytes, uint64_t fileSize,
        std::optional<std::string_view> expectedCookKey = std::nullopt);

} // namespace Iridium
