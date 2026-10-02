#pragma once

#include "renderer/rhi/GpuScene.h"

#include <map>
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
        struct PrimitiveKey {
            SceneEntityUuid owner;
            AssetGuid primitiveGuid;
            auto operator<=>(const PrimitiveKey&) const = default;
        };
        struct GeometryState {
            GpuSceneGeometryHandle handle;
            GpuSceneObservedPrimitive value;
            uint64_t revision = 1;
            bool seen = false;
            std::optional<GpuSceneGeometryIdentity> coarserIdentity;
            float geometricError = 0.0f;
            uint32_t lodLevel = 0;
        };
        struct PrimitiveState {
            GpuScenePrimitiveHandle handle;
            GpuSceneObservedPrimitive value;
            uint64_t revision = 1;
            bool seen = false;
        };
        struct InstanceState {
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
            std::vector<PrimitiveKey> activePrimitives;
            bool seen = false;
        };

        GpuSceneCapacity capacity_;
        GpuSceneSlotAllocator<GpuSceneInstanceHandle> instanceSlots_;
        GpuSceneSlotAllocator<GpuScenePrimitiveHandle> primitiveSlots_;
        GpuSceneSlotAllocator<GpuSceneGeometryHandle> geometrySlots_;
        std::map<SceneEntityUuid, InstanceState> instances_;
        std::map<PrimitiveKey, PrimitiveState> primitives_;
        std::map<GpuSceneGeometryIdentity, GeometryState> geometries_;
        std::vector<GpuSceneGeometrySource> geometrySources_;
        std::vector<GpuSceneInstanceSource> instanceSources_;
        std::vector<const GpuSceneObservedInstance*> observationOrderScratch_;
        std::vector<const GpuSceneObservedPrimitive*> primitiveOrderScratch_;
        std::vector<GpuSceneGeometryIdentity> preflightGeometryScratch_;
        std::vector<std::pair<GpuSceneGeometryHandle, uint32_t>>
            geometryLookupScratch_;
        GpuScenePackedTables packed_;
        // Double-buffer only the hot records/revisions on changed publications.
        // Dense relocation is a byte change even when logical source data did
        // not change. Cold identity tables are not duplicated here.
        GpuScenePackedTables packedHistoryScratch_;
        GpuScenePublisherStats stats_;
        std::vector<SceneEntityUuid> fallbackOwners_;
        std::vector<GpuSceneObservedInstance> cachedObservations_;
        uint32_t cachedCapacityFallbackInstances_ = 0;
        bool cachedObservationsValid_ = false;
        uint64_t publicationRevision_ = 0;
        uint64_t recordRevision_ = 0;
        uint64_t sceneEpoch_ = 0;

        void packActiveSources();
        void observeGeometry(const GpuSceneObservedPrimitive& value,
            std::optional<GpuSceneGeometryIdentity> coarserIdentity,
            float geometricError, uint32_t lodLevel);
    };

} // namespace Iridium
