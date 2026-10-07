#pragma once

// M7C P1 multi-model benchmark fixtures: which cooked model artifact backs
// which fixture source asset. Matching is by asset identity (the GUID in the
// source's metadata sidecar, which the cooked artifact carries), never by
// argument order. Qualification-only.

#include "assets/cooker/CookTypes.h"
#include "core/types/AssetGuid.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    struct BenchmarkModelSource {
        // The resolved manifest source asset (BenchmarkFixture paths).
        std::filesystem::path sourceAsset;
        // From the source's metadata sidecar; empty when it has none.
        std::optional<AssetGuid> assetGuid;
    };

    struct BenchmarkModelArtifact {
        std::filesystem::path artifactPath;
        AssetGuid assetGuid;
        std::vector<AssetDependency> dependencies;
    };

    // SHA-256 of a dependency file, or empty when the file does not exist.
    using BenchmarkDependencyHasher = std::function<
        std::optional<std::string>(const std::filesystem::path&)>;

    // For each source, the index of the artifact backing it. Every source must
    // have a sidecar GUID, sources must have distinct GUIDs, and each source
    // must match exactly one artifact and each artifact exactly one source.
    // A matched artifact's source-file dependencies are also checked against
    // the files beside its source (location relative to the source's
    // directory): a dependency present there with different content means the
    // artifact is stale. Throws std::runtime_error naming the source and
    // artifact otherwise.
    [[nodiscard]] std::vector<size_t> matchBenchmarkModelArtifacts(
        std::span<const BenchmarkModelSource> sources,
        std::span<const BenchmarkModelArtifact> artifacts,
        const BenchmarkDependencyHasher& hashDependency);

    // The sidecar GUID of `sourceAsset` (empty when it has no readable
    // sidecar), as matchBenchmarkModelArtifacts expects.
    [[nodiscard]] BenchmarkModelSource readBenchmarkModelSource(
        const std::filesystem::path& sourceAsset);

} // namespace Iridium
