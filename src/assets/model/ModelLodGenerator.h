#pragma once

#include "assets/model/ModelProduct.h"

#include <array>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t ModelLodGeneratorAbiVersion = 5;

    struct ModelLodGenerationSettings {
        // Object-space error budgets relative to the source AABB diagonal.
        std::array<float, 3> maximumRelativeErrors{
            0.005f, 0.02f, 0.05f };
        uint32_t minimumSourceTriangles = 64;
        float minimumNormalCosine = 0.9659258f; // 15 degrees
        float maximumTexCoordDelta = 0.25f;
        float maximumColorDelta = 0.1f;
        // Disabled by default to preserve v1 output. When enabled, only source
        // boundary edges may merge two boundary groups; boundary/interior merges
        // remain forbidden and the topology/displacement proofs still apply.
        bool allowBoundaryEdgeCollapse = false;
        // Disabled by default. Generated children may duplicate a source vertex
        // whose incident triangles form multiple disconnected boundary fans, so
        // each fan receives an independently provable manifold representative.
        // Canonical LOD0 and RT streams are never rewritten.
        bool splitNonManifoldBoundaryFans = false;
        // Disabled by default. Validate each proposed union against all incident
        // source triangles and skip only orientation-changing unions instead of
        // allowing one unsafe union to reject the completed level.
        bool preventOrientationChangingMerges = false;
        // Disabled by default. Require each proposed union to satisfy the local
        // triangle-mesh link condition before it can participate in a level.
        // The completed level still has to match the full source signature.
        bool preventTopologyChangingMerges = false;
    };

    struct GeneratedModelLod {
        std::vector<CookedModelVertex> vertices;
        // Primitive-local indices into vertices.
        std::vector<uint32_t> indices;
        float geometricError = 0.0f;

        bool operator==(const GeneratedModelLod&) const = default;
    };

    enum class ModelLodTopologyFailure : uint8_t {
        MixedComponentTriangle,
        NonManifoldEdge,
        InconsistentEdgeOrientation,
        BoundaryDegree,
        VertexLinkDegree,
        VertexLinkDisconnected,
        DisconnectedComponent,
        Count,
    };

    inline constexpr size_t ModelLodTopologyFailureCount =
        static_cast<size_t>(ModelLodTopologyFailure::Count);

    enum class ModelLodLevelOutcome : uint8_t {
        NotAttempted,
        Accepted,
        OrientationRejected,
        NoReduction,
        BelowMinimumReduction,
        CorrespondenceRejected,
        ReducedTopologyInvalid,
        TopologySignatureMismatch,
        Count,
    };

    inline constexpr size_t ModelLodLevelOutcomeCount =
        static_cast<size_t>(ModelLodLevelOutcome::Count);
    inline constexpr size_t ModelLodBudgetCount = 3u;

    // Aggregate, deterministic diagnostics for one generator invocation or an
    // appendGeneratedModelLods pass. Counts describe decisions only; they do not
    // participate in cooked bytes or asset identity.
    struct ModelLodGenerationStatistics {
        uint64_t canonicalPrimitiveCount = 0;
        uint64_t canonicalTriangleCount = 0;
        uint64_t skippedNonOpaquePrimitiveCount = 0;
        uint64_t skippedNonTrianglePrimitiveCount = 0;
        uint64_t skippedExistingChainPrimitiveCount = 0;
        uint64_t attemptedPrimitiveCount = 0;
        uint64_t attemptedTriangleCount = 0;
        uint64_t belowMinimumPrimitiveCount = 0;
        uint64_t belowMinimumTriangleCount = 0;
        uint64_t invalidExtentPrimitiveCount = 0;
        uint64_t invalidExtentTriangleCount = 0;
        uint64_t degenerateSourcePrimitiveCount = 0;
        uint64_t degenerateSourceTriangleCount = 0;
        uint64_t invalidSourceTopologyPrimitiveCount = 0;
        uint64_t invalidSourceTopologyTriangleCount = 0;
        std::array<uint64_t, ModelLodTopologyFailureCount>
            sourceTopologyFailurePrimitiveCounts{};
        std::array<uint64_t, ModelLodTopologyFailureCount>
            sourceTopologyFailureTriangleCounts{};
        uint64_t splitBoundaryFanVertexCount = 0;
        uint64_t duplicatedBoundaryFanVertexCount = 0;
        uint64_t noAcceptedLevelPrimitiveCount = 0;
        uint64_t noAcceptedLevelTriangleCount = 0;
        uint64_t generatedChainCount = 0;
        uint64_t generatedBaseTriangleCount = 0;
        uint64_t generatedLevelCount = 0;
        uint64_t edgeConsiderationCount = 0;
        uint64_t boundaryRejectedEdgeCount = 0;
        uint64_t attributeRejectedEdgeCount = 0;
        uint64_t budgetRejectedEdgeCount = 0;
        uint64_t acceptedMergeCount = 0;
        uint64_t orientationRejectedEdgeCount = 0;
        uint64_t correspondenceRejectedEdgeCount = 0;
        uint64_t topologyRejectedEdgeCount = 0;
        uint64_t orientationRejectedLevelCount = 0;
        uint64_t noReductionLevelCount = 0;
        uint64_t belowMinimumReductionLevelCount = 0;
        uint64_t correspondenceRejectedLevelCount = 0;
        uint64_t topologyRejectedLevelCount = 0;
        uint64_t reducedTopologyInvalidLevelCount = 0;
        uint64_t topologySignatureMismatchLevelCount = 0;
        uint64_t topologyComponentMismatchLevelCount = 0;
        uint64_t topologyEulerMismatchLevelCount = 0;
        uint64_t topologyBoundaryMismatchLevelCount = 0;
        std::array<std::array<uint64_t, ModelLodLevelOutcomeCount>,
            ModelLodBudgetCount> levelOutcomeCounts{};
        std::array<std::array<uint64_t, ModelLodLevelOutcomeCount>,
            ModelLodBudgetCount> levelOutcomeTriangleCounts{};
        std::array<uint64_t, ModelLodLevelOutcomeCount>
            noAcceptedTerminalOutcomePrimitiveCounts{};
        std::array<uint64_t, ModelLodLevelOutcomeCount>
            noAcceptedTerminalOutcomeTriangleCounts{};
        std::array<uint64_t, ModelLodTopologyFailureCount>
            reducedTopologyFailureLevelCounts{};

        bool operator==(const ModelLodGenerationStatistics&) const = default;
    };

    // Deterministic, conservative endpoint-collapse generator for opaque
    // triangle primitives. It retains exact source vertex attributes at each
    // representative, protects discontinuities, locks boundaries by default,
    // rejects orientation changes, and reports a conservative object-space
    // displacement bound. Empty output means no fidelity-safe reduction met the
    // contract.
    [[nodiscard]] std::vector<GeneratedModelLod> generateModelLods(
        std::span<const CookedModelVertex> vertices,
        std::span<const uint32_t> localTriangleIndices,
        const ModelLodGenerationSettings& settings = {},
        std::stop_token stopToken = {},
        ModelLodGenerationStatistics* statistics = nullptr);

    // Appends parent-contained children to a canonical CPU product. Existing
    // authored chains and non-opaque primitives are left intact. Publication is
    // still owned by the caller's newest-complete-revision transaction.
    [[nodiscard]] size_t appendGeneratedModelLods(
        CookedModelProductData& product,
        const ModelLodGenerationSettings& settings = {},
        std::stop_token stopToken = {},
        ModelLodGenerationStatistics* statistics = nullptr);

} // namespace Iridium
