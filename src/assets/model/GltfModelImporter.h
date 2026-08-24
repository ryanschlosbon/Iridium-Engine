#pragma once

#include "assets/cooker/ImporterRegistry.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kGltfModelImporterVersion = 6;

    struct TriangleConnectedComponent {
        uint32_t sourceTriangleSeed = 0;
        std::vector<uint32_t> sourceTriangleIndices;

        bool operator==(const TriangleConnectedComponent&) const = default;
    };

    [[nodiscard]] std::vector<TriangleConnectedComponent>
        findTriangleConnectedComponents(
            std::span<const uint32_t> triangleIndices);

    struct ClosedTriangleTopologyAnalysis {
        uint32_t triangleCount = 0;
        uint32_t boundaryEdgeCount = 0;
        uint32_t nonManifoldEdgeCount = 0;
        uint32_t inconsistentOrientationEdgeCount = 0;
        uint32_t degenerateTriangleCount = 0;
        double signedVolume = 0.0;

        [[nodiscard]] bool validClosed() const noexcept {
            return triangleCount >= 4 && boundaryEdgeCount == 0 &&
                nonManifoldEdgeCount == 0 &&
                inconsistentOrientationEdgeCount == 0 &&
                degenerateTriangleCount == 0 && signedVolume != 0.0;
        }
    };

    // Proves that one canonical triangle component is a consistently oriented,
    // edge-manifold closed volume. Geometry is used to reject zero-area and
    // zero-volume shells; self-intersection remains outside this bounded proof.
    [[nodiscard]] ClosedTriangleTopologyAnalysis analyzeClosedTriangleTopology(
        std::span<const glm::vec3> positions,
        std::span<const uint32_t> triangleIndices,
        std::span<const uint32_t> sourceTriangleIndices);

    // Source parsing produces a renderer-independent, deterministic intermediate
    // document. CPU cooking then canonicalizes topology, tangents, transforms,
    // bounds, material GUID references, and RT reconstruction streams.
    class GltfModelImporter final : public AssetImporter {
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
