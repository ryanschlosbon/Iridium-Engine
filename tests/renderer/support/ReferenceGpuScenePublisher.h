#pragma once

// Test-only reference: the GpuScenePublisher full-path implementation as of
// 9f2a28e (the M7R R5c.5 baseline), kept verbatim apart from the class name so
// the replay tests can compare the incremental production publisher against it
// byte for byte. It is never linked into a production target.

#include "renderer/scene/GpuScenePublisher.h"

#include <map>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    class ReferenceGpuScenePublisher {
    public:
        explicit ReferenceGpuScenePublisher(GpuSceneCapacity capacity);

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
            uint64_t movedPass = 0;   // M9 G3
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
        GpuScenePackedTables packedHistoryScratch_;
        GpuScenePublisherStats stats_;
        std::vector<SceneEntityUuid> fallbackOwners_;
        std::vector<GpuSceneObservedInstance> cachedObservations_;
        uint32_t cachedCapacityFallbackInstances_ = 0;
        bool cachedObservationsValid_ = false;
        uint64_t publicationRevision_ = 0;
        uint64_t recordRevision_ = 0;
        uint64_t pass_ = 0;   // M9 G3: one per non-fast-path synchronize
        bool settlePending_ = false;
        uint64_t sceneEpoch_ = 0;

        void packActiveSources();
        void observeGeometry(const GpuSceneObservedPrimitive& value,
            std::optional<GpuSceneGeometryIdentity> coarserIdentity,
            float geometricError, uint32_t lodLevel);
    };

} // namespace Iridium
