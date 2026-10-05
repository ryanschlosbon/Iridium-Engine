#pragma once

#include "renderer/rhi/GpuScene.h"

#include <span>
#include <vector>

namespace Iridium {

    // Geometry-only child observation: never creates an instantiated primitive
    // or a material binding. Canonical LOD0 remains the consumer's identity.
    struct GpuSceneObservedLodGeometry {
        GpuSceneGeometryIdentity identity;
        GeometryHandle geometry;
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t indexType = 1, vertexLayout = 0;
        glm::vec4 localBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        glm::vec4 localBoundsMin{ 0.0f }, localBoundsMax{ 0.0f };
        float geometricError = 0.0f;
    };

    struct GpuSceneObservedPrimitive {
        GpuScenePrimitiveIdentity identity;
        GpuSceneGeometryIdentity geometryIdentity;
        GeometryHandle legacyGeometry;
        MaterialHandle material;
        PipelineHandle pipeline;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t indexType = 1;
        uint32_t vertexLayout = 0;
        uint32_t primitiveFlags = GpuScenePrimitiveOpaque;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque;
        glm::vec4 localBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        glm::vec4 localBoundsMin{ 0.0f };
        glm::vec4 localBoundsMax{ 0.0f };
        uint32_t geometryProductRevision = 0;
        uint32_t materialRevision = 0;
        std::vector<GpuSceneObservedLodGeometry> lodChildren;
    };

    struct GpuSceneObservedInstance {
        GpuSceneInstanceIdentity identity;
        // Caller-owned exact content revision. Zero disables the unchanged fast
        // path; every observation-visible mutation must advance this value.
        // The publisher trusts it per observation index: an observation whose
        // index, owner and nonzero revision equal the previous call's is
        // treated as unchanged and is not re-read (M7R R5c.5).
        uint64_t observationRevision = 0;
        GpuSceneMobility mobility = GpuSceneMobility::Movable;
        uint32_t flags = GpuSceneInstanceEnabled;
        uint32_t maximumLod = MaximumGpuSceneLodLevels - 1u;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque |
            GpuSceneConsumerShadow | GpuSceneConsumerProbe |
            GpuSceneConsumerSelection;
        glm::mat4 worldTransform{ 1.0f };
        glm::vec4 worldBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        glm::vec4 worldBoundsMin{ 0.0f };
        glm::vec4 worldBoundsMax{ 0.0f };
        std::vector<GpuSceneObservedPrimitive> primitives;
    };

    struct GpuScenePublisherStats {
        uint32_t activeInstances = 0;
        uint32_t activePrimitives = 0;
        uint32_t activeGeometries = 0;
        uint32_t changedInstances = 0;
        uint32_t changedTransforms = 0;
        uint32_t changedPrimitives = 0;
        uint32_t changedGeometries = 0;
        uint32_t retiredInstances = 0;
        uint32_t retiredPrimitives = 0;
        uint32_t retiredGeometries = 0;
        uint32_t capacityFallbackInstances = 0;
        uint32_t unchangedFastPath = 0;
        uint64_t changedInstanceBytes = 0;
        uint64_t changedTransformBytes = 0;
        uint64_t changedPrimitiveBytes = 0;
        uint64_t changedGeometryBytes = 0;
    };

    // Incremental publication (M7R R5c.5). State lives in dense pools with
    // sorted flat key arrays: instances by owner, primitives per instance by
    // primitive GUID, geometries by identity. A changed publication reads only
    // the observations whose (index, owner, revision) changed; unchanged ones
    // keep their records. Without an instance, primitive or geometry added or
    // removed, the touched records are rewritten in place and only they are
    // revised; otherwise the tables are repacked from the pools. The packed
    // tables, revisions, slot handles and statistics are byte-identical to the
    // full-scan publisher this replaced, which tests keep as a reference.
    class GpuScenePublisher {
    public:
        explicit GpuScenePublisher(GpuSceneCapacity capacity);

        const GpuScenePackedTables& synchronize(uint64_t sceneEpoch,
            std::span<const GpuSceneObservedInstance> observations,
            uint64_t retireAfterSerial, uint64_t completedSerial);
        [[nodiscard]] const GpuScenePublisherStats& stats() const noexcept {
            return stats_;
        }
        [[nodiscard]] std::span<const SceneEntityUuid>
            directFallbackOwners() const noexcept { return fallbackOwners_; }

    private:
        static constexpr uint32_t InvalidPoolIndex = UINT32_MAX;

        // The geometry fields the full-scan publisher compared and packed.
        struct GeometryValue {
            GeometryHandle legacyGeometry;
            uint32_t firstIndex = 0, indexCount = 0;
            int32_t vertexOffset = 0;
            uint32_t indexType = 1, vertexLayout = 0;
            uint32_t geometryProductRevision = 0;
            glm::vec4 localBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
            glm::vec4 localBoundsMin{ 0.0f };
            glm::vec4 localBoundsMax{ 0.0f };
        };
        struct GeometryState {
            GpuSceneGeometryIdentity identity;
            GpuSceneGeometryHandle handle;
            GeometryValue value;
            GpuSceneGeometryIdentity coarserIdentity;
            bool hasCoarser = false;
            float geometricError = 0.0f;
            uint32_t lodLevel = 0;
            uint64_t revision = 1;
            uint64_t hash = 0;
            // Chain references from accepted primitives (base and LOD levels).
            uint32_t references = 0;
            uint32_t denseIndex = InvalidGpuSceneIndex;
            uint64_t observedPass = 0;
            uint64_t changedPass = 0;
            uint32_t releasedScratch = 0;
        };
        struct PrimitiveState {
            GpuScenePrimitiveHandle handle;
            GpuScenePrimitiveIdentity identity;
            GpuSceneGeometryIdentity geometryIdentity;
            MaterialHandle material;
            PipelineHandle pipeline;
            uint32_t primitiveFlags = GpuScenePrimitiveOpaque;
            uint32_t consumerMask = GpuSceneConsumerMainOpaque;
            uint32_t geometryProductRevision = 0;
            uint32_t materialRevision = 0;
            uint64_t revision = 1;
            uint32_t geometry = InvalidPoolIndex;
        };
        struct InstanceState {
            SceneEntityUuid owner;
            GpuSceneInstanceHandle handle;
            GpuSceneInstanceIdentity identity;
            GpuSceneMobility mobility = GpuSceneMobility::Movable;
            uint32_t flags = 0;
            uint32_t maximumLod = MaximumGpuSceneLodLevels - 1u;
            uint32_t consumerMask = 0;
            glm::mat4 currentWorld{ 1.0f };
            glm::mat4 previousWorld{ 1.0f };
            glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, -1.0f };
            glm::vec4 minimum{ 0.0f };
            glm::vec4 maximum{ 0.0f };
            uint64_t instanceRevision = 1;
            uint64_t currentTransformRevision = 1;
            uint64_t previousTransformRevision = 1;
            // Primitive pool indices in primitive-GUID order.
            std::vector<uint32_t> primitives;
            // Geometry pool indices observed by those primitives (base, then
            // LOD levels), one entry per observation.
            std::vector<uint32_t> geometryReferences;
            size_t observationIndex = 0;
            uint64_t observationRevision = 0;
            uint64_t matchedPass = 0;
            uint32_t denseIndex = InvalidGpuSceneIndex;
            uint32_t firstPrimitive = 0;
        };
        enum class ObservationKind : uint8_t { Fallback, Clean, Dirty };
        struct ObservationEntry {
            const GpuSceneObservedInstance* observation = nullptr;
            uint32_t existing = InvalidPoolIndex;
            // Geometry lookups made by the divergence check (base then LOD
            // levels per primitive, in observation order), reused afterwards.
            uint32_t lookupOffset = InvalidPoolIndex;
            ObservationKind kind = ObservationKind::Dirty;
        };

        GpuSceneCapacity capacity_;
        GpuSceneSlotAllocator<GpuSceneInstanceHandle> instanceSlots_;
        GpuSceneSlotAllocator<GpuScenePrimitiveHandle> primitiveSlots_;
        GpuSceneSlotAllocator<GpuSceneGeometryHandle> geometrySlots_;
        std::vector<InstanceState> instancePool_;
        std::vector<PrimitiveState> primitivePool_;
        std::vector<GeometryState> geometryPool_;
        std::vector<uint32_t> instanceFree_, primitiveFree_, geometryFree_;
        // Owner order (= dense instance order) and identity order (= dense
        // geometry order); geometries created in a pass wait in the pending
        // array until the pass ends.
        std::vector<uint32_t> instanceOrder_, instanceOrderScratch_;
        std::vector<uint32_t> geometryOrder_, geometryPending_,
            geometryOrderScratch_;
        // Open-addressed identity -> geometry pool index (linear probing).
        std::vector<uint32_t> geometryTable_;
        uint32_t geometryTableCount_ = 0;
        std::vector<const GpuSceneObservedInstance*> observationOrderScratch_;
        std::vector<ObservationEntry> observationEntries_;
        std::vector<const GpuSceneObservedPrimitive*> primitiveOrderScratch_;
        std::vector<GpuSceneGeometryIdentity> preflightGeometryScratch_;
        std::vector<uint32_t> primitiveListScratch_;
        std::vector<uint32_t> checkedLookupScratch_, chainOffsetScratch_,
            chainLookupScratch_;
        std::vector<uint32_t> retiredInstanceScratch_, retiredPrimitiveScratch_,
            releasedGeometryScratch_, deadGeometryCandidateScratch_,
            touchedInstanceScratch_, changedGeometryScratch_;
        GpuScenePackedTables packed_;
        // Double-buffer only the hot records/revisions on repacked
        // publications. Dense relocation is a byte change even when logical
        // source data did not change. Cold identity tables are not duplicated.
        GpuScenePackedTables packedHistoryScratch_;
        GpuScenePublisherStats stats_;
        std::vector<SceneEntityUuid> fallbackOwners_;
        // The previous call's (owner, revision) per observation index.
        std::vector<SceneEntityUuid> cachedOwners_;
        std::vector<uint64_t> cachedRevisions_;
        uint32_t cachedCapacityFallbackInstances_ = 0;
        bool cachedObservationsValid_ = false;
        // Every accepted observation of each geometry carries its stored
        // value. Cleared by a pass that observes one geometry with two
        // values; the next changed publication then re-reads everything.
        bool coherent_ = true;
        bool passConflict_ = false;
        bool topologyChanged_ = false;
        uint64_t pass_ = 0;
        uint64_t publicationRevision_ = 0;
        uint64_t recordRevision_ = 0;
        uint64_t sceneEpoch_ = 0;

        void retireAllForEpochChange(uint64_t retireAfterSerial);
        void classifyObservations(
            std::span<const GpuSceneObservedInstance> observations,
            bool fullScan);
        [[nodiscard]] bool incrementalWouldDiverge();
        uint32_t processObservation(const ObservationEntry& entry,
            size_t observationIndex);
        void releaseInstance(uint32_t instance);
        void releaseGeometry(uint32_t geometry);
        uint32_t observeGeometry(const GpuSceneGeometryIdentity& identity,
            const GeometryValue& value, const GpuSceneGeometryIdentity* coarser,
            float geometricError, uint32_t lodLevel, uint32_t knownGeometry);
        [[nodiscard]] uint32_t findGeometry(
            const GpuSceneGeometryIdentity& identity) const noexcept;
        void insertGeometryKey(uint32_t geometry);
        void eraseGeometryKey(uint32_t geometry) noexcept;
        void finishGeometries(uint64_t retireAfterSerial);
        void packAll();
        void packTouched();
        [[nodiscard]] GpuSceneInstanceRecord instanceRecord(
            const InstanceState& instance) const noexcept;
        [[nodiscard]] GpuScenePrimitiveRecord primitiveRecord(
            const PrimitiveState& primitive, uint32_t denseInstance,
            uint32_t densePrimitive) const noexcept;
        [[nodiscard]] GpuSceneGeometryRecord geometryRecord(
            const GeometryState& geometry) const;
        uint32_t allocateInstanceState();
        uint32_t allocatePrimitiveState();
        uint32_t allocateGeometryState();
    };

} // namespace Iridium
