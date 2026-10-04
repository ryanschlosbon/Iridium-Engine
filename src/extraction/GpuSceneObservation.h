#pragma once

// M7R R5c.5 (design section 5.1/5.2): the GPU-scene observation of the scene's
// mesh instances, change-driven.
//
// The observation is one GpuSceneObservedInstance per accepted mesh entity, in
// mesh-pool order, with a per-slot revision the publisher trusts. Until R5c.5
// the application rebuilt every observation every frame and compared it with
// memcmp (the "full walk", kept verbatim here). Now a frame walks the whole pool
// only on a structural change and otherwise re-runs the full walk's per-entity
// code only for the pool runs that can have changed:
//
//   - A run is the sequence of pool entities processed at one observation slot:
//     skipped entities, then the entity accepted at that slot (the tail run has
//     no accepted entity). The full walk's result for a run depends only on the
//     run's inputs and the slot's previous state.
//   - Change sources: the mesh pool's write journal (every mutable access,
//     ComponentWriteJournal), the transform system's changed-entity journal,
//     the previous and current selection, and entities with material
//     overrides (their override bindings come from the asset manager), whose
//     runs are re-run every frame. Runs that hold an entity whose metadata is
//     invalid are re-run every frame, because the full walk lets that entity
//     overwrite the slot's metadata (its successor then rebuilds every frame).
//   - Full walk triggers: a changed mesh, transform or instance-batch pool
//     (pointer or structure revision), scene epoch, an overflowed journal, a
//     referenced model whose geometry, cook key or asset GUID changed in place
//     or that expired, a visited entity whose model pointer or override
//     presence changed, and a run whose entities would change kind (skip,
//     fallback, invalid, accepted). Kinds are checked by a side-effect-free
//     simulation before any state is touched.
//
// Observations, revisions and the direct-fallback count are therefore exactly
// the full walk's. The qualification ExtractionVerifier
// (--qualification-extraction-verifier) runs a second instance in full-walk
// mode every frame and compares both, field by field.

#include "ecs/Entity.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/scene/GpuScenePublisher.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

class Registry;
struct MeshComponent;

namespace Iridium {

    class AssetManager;
    class SceneWorld;
    struct ModelAsset;

    struct GpuSceneObservationInputs {
        // Material override bindings (may be null before assets exist).
        const AssetManager* assets = nullptr;
        Entity selectedEntity = NULL_ENTITY;
        // The transform system's changed-entity journal for this frame.
        std::span<const Entity> changedTransforms{};
    };

    class GpuSceneObservation {
    public:
        enum class Mode : uint8_t {
            ChangeDriven,
            // Every frame is a full walk (the reference the verifier uses).
            FullWalk,
        };
        struct Stats {
            uint64_t fullWalks = 0;
            uint64_t changeDrivenFrames = 0;
            uint64_t visitedRuns = 0;
        };

        GpuSceneObservation(SceneWorld& scene, Mode mode) noexcept;

        // Updates the observations and the direct-fallback count.
        void observe(const GpuSceneObservationInputs& inputs);

        [[nodiscard]] std::span<const GpuSceneObservedInstance>
            observations() const noexcept { return observations_; }
        [[nodiscard]] uint32_t directFallbackCount() const noexcept {
            return directFallbackCount_;
        }
        [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

        // Empty when equal; otherwise the first differing field.
        [[nodiscard]] static std::string compare(
            const GpuSceneObservation& expected,
            const GpuSceneObservation& actual);

    private:
        enum class Kind : uint8_t {
            EarlySkip,      // disabled, no model, invalid geometry, no transform
            BatchFallback,  // render instance batch
            OwnerFallback,  // nil persistent identity
            InvalidMetadata,
            Accepted,
        };
        struct Metadata {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            GeometryHandle geometry;
            std::string cookKey;
            uint64_t materialOverrideSignature = 0;
            uint64_t observationRevision = 0;
            glm::vec3 localMinimum{ 0.0f };
            glm::vec3 localMaximum{ 0.0f };
            uint32_t baseConsumerMask = 0;
            bool valid = false;
        };
        struct EntityRecord {
            Entity entity = NULL_ENTITY;
            const ModelAsset* model = nullptr;
            uint32_t slot = 0;
            Kind kind = Kind::EarlySkip;
            bool hasOverrides = false;
        };
        struct ModelRecord {
            std::weak_ptr<ModelAsset> model;
            const ModelAsset* address = nullptr;
            GeometryHandle geometry;
            std::string cookKey;
            AssetGuid assetGuid;
        };
        struct Pools;

        void fullWalk(const Pools& pools, Entity selectedEntity);
        [[nodiscard]] bool changeDrivenWalk(const Pools& pools,
            const GpuSceneObservationInputs& inputs);
        // The full walk's per-entity step at `observationCount` (advanced
        // when the entity is accepted).
        Kind processEntity(const Pools& pools, Entity entity,
            Entity selectedEntity, size_t& observationCount);
        // The kinds `run` would produce, from the slot's current metadata,
        // without changing any state; false when one differs from the record.
        [[nodiscard]] bool runKindsUnchanged(const Pools& pools, uint32_t run) const;
        [[nodiscard]] uint64_t overrideSignature(
            const MeshComponent& mesh) const;
        [[nodiscard]] bool modelsUnchanged() const;
        // Appends a default slot reusing the spare storage; truncation keeps
        // the first removed slot's storage as the spare (M7R R5c.8).
        void appendSlot();
        void truncateSlots(size_t count);

        SceneWorld& scene_;
        Mode mode_;
        const AssetManager* assets_ = nullptr;   // this call's
        std::vector<GpuSceneObservedInstance> observations_;
        std::vector<Metadata> metadata_;
        uint32_t directFallbackCount_ = 0;

        // The last walk's structure (valid while `walked_`).
        bool walked_ = false;
        const void* meshPool_ = nullptr;
        const void* transformPool_ = nullptr;
        const void* batchPool_ = nullptr;
        uint64_t meshStructure_ = 0;
        uint64_t transformStructure_ = 0;
        uint64_t batchStructure_ = 0;
        uint64_t sceneEpoch_ = 0;
        Entity selected_ = NULL_ENTITY;
        std::vector<EntityRecord> records_;         // by mesh-pool dense index
        std::vector<uint32_t> runFirst_;            // slot -> first dense index; [slots + 1] = pool size
        std::vector<uint32_t> volatileRuns_;        // runs re-run every frame
        std::vector<uint32_t> overrideEntities_;    // dense indices
        std::vector<ModelRecord> models_;
        // Storage of the last truncated slot (strings and vectors only).
        GpuSceneObservedInstance spareObservation_;
        Metadata spareMetadata_;
        // Per-frame scratch.
        std::vector<uint32_t> runStamp_;
        uint32_t stamp_ = 0;
        std::vector<uint32_t> runs_;
        Stats stats_{};
    };

} // namespace Iridium
