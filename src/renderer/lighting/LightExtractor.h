#pragma once

#include "renderer/rhi/LightingTypes.h"
#include "scene/SceneWorld.h"
#include "scene/components/LightComponent.h"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Iridium {

    struct LightExtractionConfig {
        uint32_t initialCapacity = kInitialGpuLightCapacity;
        uint32_t maximumCapacity = kMaximumGpuLightCapacity;
    };

    // M7R R5c.6: change-driven. Each light's packed candidate is memoized per
    // light-pool slot against every input that determines it (entity, persistent
    // identity, LightComponent, world position and the emission rotation chain),
    // so only lights whose inputs changed are rebuilt. Slot records, record
    // revisions, the active list and its revision are exactly those of the full
    // scan (the test-only ReferenceLightExtractor). The removal walk and the
    // active-list rebuild run only when membership can have changed. Steady
    // frames do not allocate, including frames that repeat diagnostics.
    // M7.10.5: each slot also carries a shadow revision that advances only
    // when the record's shadow geometry changes (sameLightShadowGeometry), on
    // its own counter so record revisions are unchanged.
    class LightExtractor final {
    public:
        explicit LightExtractor(LightExtractionConfig config = {});

        [[nodiscard]] LightingFramePacket extract(const SceneWorld& world);
        void reset() noexcept;

        [[nodiscard]] std::span<const LightExtractionDiagnostic>
            diagnostics() const noexcept {
            return { diagnostics_.data(), diagnosticCount_ };
        }
        [[nodiscard]] std::optional<uint32_t> slotFor(
            SceneEntityUuid owner) const;

    private:
        static constexpr uint32_t kNoSlot = 0xffff'ffffu;
        // Deeper emission-rotation chains are rebuilt every frame.
        static constexpr uint32_t kCachedChainLinks = 4;

        struct Candidate {
            SceneEntityUuid owner;
            PackedGpuLight record;
            int32_t priority = 0;
            bool castsShadows = false;
            uint32_t cacheIndex = 0;
        };
        struct ChainLink {
            Entity entity;
            glm::vec3 rotation{ 0.0f };
        };
        // Memo for one light-pool dense index.
        struct CachedLight {
            Entity entity;
            SceneEntityUuid owner;
            LightComponent light;
            glm::vec3 position{ 0.0f };
            std::array<ChainLink, kCachedChainLinks> chain{};
            uint32_t chainLength = 0;
            uint32_t slotHint = kNoSlot;
            PackedGpuLight record;
            bool valid = false;
        };

        [[nodiscard]] bool buildCandidate(const SceneWorld& world,
            Entity entity, Candidate& candidate);
        [[nodiscard]] bool worldEmissionDirection(
            const SceneWorld& world, Entity entity, glm::vec3& direction);
        [[nodiscard]] bool cachedInputsMatch(const SceneWorld& world,
            Entity entity, const LightComponent& light,
            const CachedLight& cached) const;
        void storeCache(const SceneWorld& world, Entity entity,
            const LightComponent& light, const Candidate& candidate,
            CachedLight& cached);
        void pushDiagnostic(LightExtractionDiagnosticCode code,
            SceneEntityUuid owner, const char* propertyPath,
            const char* message);
        void resetForWorld(const SceneWorld& world);
        void ensureCapacity(uint32_t required);
        void writeRecord(uint32_t slot, const PackedGpuLight& record);
        void clearRecord(uint32_t slot);
        void buildChangedRanges();
        void advanceRevision(uint64_t& value) noexcept;
        void advanceShadowRevision(uint64_t& value) noexcept;

        LightExtractionConfig config_;
        const SceneWorld* world_ = nullptr;
        uint64_t worldEpoch_ = 0;
        uint64_t nextRevision_ = 0;
        uint64_t nextShadowRevision_ = 0;
        uint64_t activeListRevision_ = 0;
        std::vector<PackedGpuLight> records_;
        std::vector<uint64_t> recordRevisions_;
        std::vector<uint64_t> shadowRevisions_;
        std::vector<LightSelectionMetadata> selectionMetadata_;
        std::vector<uint32_t> activeSlots_;
        std::vector<uint32_t> previousActiveSlots_;
        std::vector<uint32_t> changedSlots_;
        std::vector<LightRecordRange> changedRanges_;
        // Diagnostics are written in place over persistent storage; the first
        // diagnosticCount_ entries are this frame's.
        std::vector<LightExtractionDiagnostic> diagnostics_;
        size_t diagnosticCount_ = 0;
        std::vector<Entity> transformChain_;
        std::vector<Candidate> candidates_;
        std::vector<Candidate*> newCandidates_;
        std::vector<SceneEntityUuid> removedOwners_;
        std::vector<uint8_t> occupiedSlots_;
        std::vector<CachedLight> lightCache_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash>
            slotsByOwner_;
        // Membership (the owner->slot map) changed since the active list was
        // last rebuilt.
        bool membershipChanged_ = true;
        uint32_t directionalCount_ = 0;
        uint32_t localCount_ = 0;
        LightExtractionStats stats_;
    };

} // namespace Iridium
