#include "qualification/harness/BenchmarkModelMatching.h"

#include "assets/AssetMetadata.h"

#include <stdexcept>

namespace Iridium {

    namespace {

        std::string describeArtifact(const BenchmarkModelArtifact& artifact) {
            return artifact.artifactPath.generic_string() + " (asset " +
                artifact.assetGuid.toString() + ")";
        }

    } // namespace

    std::vector<size_t> matchBenchmarkModelArtifacts(
        std::span<const BenchmarkModelSource> sources,
        std::span<const BenchmarkModelArtifact> artifacts,
        const BenchmarkDependencyHasher& hashDependency) {
        for (size_t index = 0; index < sources.size(); ++index) {
            const BenchmarkModelSource& source = sources[index];
            if (!source.assetGuid) {
                throw std::runtime_error("Benchmark source " +
                    source.sourceAsset.generic_string() +
                    " has no readable metadata sidecar, so no cooked model "
                    "artifact can be identified for it");
            }
            for (size_t other = 0; other < index; ++other) {
                if (sources[other].assetGuid == source.assetGuid) {
                    throw std::runtime_error("Benchmark sources " +
                        sources[other].sourceAsset.generic_string() + " and " +
                        source.sourceAsset.generic_string() +
                        " share asset " + source.assetGuid->toString());
                }
            }
        }
        constexpr size_t kUnmatched = static_cast<size_t>(-1);
        std::vector<size_t> matches(sources.size(), kUnmatched);
        for (size_t artifactIndex = 0; artifactIndex < artifacts.size();
            ++artifactIndex) {
            const BenchmarkModelArtifact& artifact = artifacts[artifactIndex];
            size_t sourceIndex = 0;
            while (sourceIndex < sources.size() &&
                *sources[sourceIndex].assetGuid != artifact.assetGuid) {
                ++sourceIndex;
            }
            if (sourceIndex == sources.size()) {
                throw std::runtime_error("Cooked model artifact " +
                    describeArtifact(artifact) +
                    " matches no source asset of the benchmark fixture");
            }
            if (matches[sourceIndex] != kUnmatched) {
                throw std::runtime_error("Cooked model artifacts " +
                    describeArtifact(artifacts[matches[sourceIndex]]) + " and " +
                    describeArtifact(artifact) + " both match benchmark source " +
                    sources[sourceIndex].sourceAsset.generic_string());
            }
            matches[sourceIndex] = artifactIndex;
        }
        for (size_t index = 0; index < sources.size(); ++index) {
            const BenchmarkModelSource& source = sources[index];
            if (matches[index] == kUnmatched) {
                throw std::runtime_error("No cooked model artifact "
                    "(--cooked-model-artifact or --benchmark-model-artifact) "
                    "for benchmark source " + source.sourceAsset.generic_string() +
                    " (asset " + source.assetGuid->toString() + ")");
            }
            const BenchmarkModelArtifact& artifact = artifacts[matches[index]];
            const std::filesystem::path directory =
                source.sourceAsset.parent_path();
            for (const AssetDependency& dependency : artifact.dependencies) {
                if (dependency.type != AssetDependencyType::SourceFile ||
                    dependency.location.empty() ||
                    dependency.contentHash.empty()) continue;
                const std::optional<std::string> hash =
                    hashDependency(directory / dependency.location);
                if (hash && *hash != dependency.contentHash) {
                    throw std::runtime_error("Cooked model artifact " +
                        describeArtifact(artifact) +
                        " is stale for benchmark source " +
                        source.sourceAsset.generic_string() + ": dependency " +
                        dependency.location + " changed since it was cooked");
                }
            }
        }
        return matches;
    }

    BenchmarkModelSource readBenchmarkModelSource(
        const std::filesystem::path& sourceAsset) {
        BenchmarkModelSource source{ .sourceAsset = sourceAsset };
        const std::filesystem::path sidecar =
            assetMetadataSidecarPath(sourceAsset);
        std::error_code error;
        if (!std::filesystem::is_regular_file(sidecar, error)) return source;
        const AssetMetadataReadResult metadata = readAssetMetadata(sidecar);
        if (metadata.metadata && !metadata.hasErrors()) {
            source.assetGuid = metadata.metadata->assetGuid;
        }
        return source;
    }

} // namespace Iridium
