#pragma once

// Test-only reference: the full-scan extractReflectionProbes and
// ReflectionProbePublisher as of f2cc899 (the M7R R5c.6 baseline), kept
// verbatim apart from the names so the replay tests can compare the
// change-driven production path against them byte for byte. Never linked into a
// production target.

#include "renderer/lighting/ReflectionProbe.h"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Iridium {

    [[nodiscard]] ReflectionProbeFramePacket referenceExtractReflectionProbes(
        const SceneWorld& world,
        ReflectionProbeResidencyFn residency = {});

    class ReferenceReflectionProbePublisher final {
    public:
        explicit ReferenceReflectionProbePublisher(
            ReflectionProbePublicationConfig config = {});

        [[nodiscard]] ReflectionProbeGpuFramePacket publish(
            std::span<const ReflectionProbeCandidate> candidates,
            const ReflectionProbeEnvironmentSlotFn& environmentSlot);
        void reset() noexcept;

        [[nodiscard]] std::optional<uint32_t> slotFor(
            SceneEntityUuid owner) const;

    private:
        struct PublishCandidate {
            const ReflectionProbeCandidate* source = nullptr;
            uint32_t environmentSlot = kInvalidEnvironmentTableSlot;
            float influenceVolume = 0.0f;
            uint32_t selectionRank = 0;
        };

        void ensureCapacity(uint32_t required);
        void writeRecord(uint32_t slot,
            const PackedGpuReflectionProbe& record);
        void clearRecord(uint32_t slot);
        void buildChangedRanges();
        void advanceRevision(uint64_t& value) noexcept;

        ReflectionProbePublicationConfig config_;
        uint64_t nextRevision_ = 0;
        uint64_t activeListRevision_ = 0;
        std::vector<PackedGpuReflectionProbe> records_;
        std::vector<uint64_t> recordRevisions_;
        std::vector<ReflectionProbeSelectionMetadata> selectionMetadata_;
        std::vector<uint32_t> activeSlots_;
        std::vector<uint32_t> previousActiveSlots_;
        std::vector<uint32_t> changedSlots_;
        std::vector<ReflectionProbeRecordRange> changedRanges_;
        std::vector<uint8_t> occupiedSlots_;
        std::vector<PublishCandidate> publishCandidates_;
        std::vector<PublishCandidate*> newCandidates_;
        std::vector<SceneEntityUuid> removedOwners_;
        std::unordered_map<SceneEntityUuid, uint32_t, SceneEntityUuidHash>
            slotsByOwner_;
        ReflectionProbePublicationStats stats_;
    };

} // namespace Iridium
