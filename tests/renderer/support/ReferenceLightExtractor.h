#pragma once

// Test-only reference: the LightExtractor full-scan implementation as of
// f2cc899 (the M7R R5c.6 baseline), kept verbatim apart from the class name so
// the replay tests can compare the change-driven production extractor against
// it byte for byte. It is never linked into a production target. M7.10.5
// adds only the per-slot shadow revision (its own counter, advanced when
// sameLightShadowGeometry() fails), so the replay tests also cover it.

#include "renderer/lighting/LightExtractor.h"
#include "renderer/rhi/LightingTypes.h"
#include "scene/SceneWorld.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Iridium {

    class ReferenceLightExtractor final {
    public:
        explicit ReferenceLightExtractor(LightExtractionConfig config = {});

        [[nodiscard]] LightingFramePacket extract(const SceneWorld& world);
        void reset() noexcept;

        [[nodiscard]] std::span<const LightExtractionDiagnostic>
            diagnostics() const noexcept { return diagnostics_; }
        [[nodiscard]] std::optional<uint32_t> slotFor(
            SceneEntityUuid owner) const;

    private:
        struct Candidate {
            SceneEntityUuid owner;
            PackedGpuLight record;
            int32_t priority = 0;
            bool castsShadows = false;
        };

        [[nodiscard]] bool buildCandidate(const SceneWorld& world,
            Entity entity, Candidate& candidate);
        [[nodiscard]] bool worldEmissionDirection(
            const SceneWorld& world, Entity entity, glm::vec3& direction);
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
        std::vector<LightExtractionDiagnostic> diagnostics_;
        std::vector<Entity> transformChain_;
        std::vector<Candidate> candidates_;
        std::vector<Candidate*> newCandidates_;
        std::vector<SceneEntityUuid> removedOwners_;
        std::vector<uint8_t> occupiedSlots_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash>
            slotsByOwner_;
        LightExtractionStats stats_;
    };

} // namespace Iridium
