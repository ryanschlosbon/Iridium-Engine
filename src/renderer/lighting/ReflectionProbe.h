#pragma once

#include "renderer/rhi/LightingTypes.h"
#include "renderer/rhi/ReflectionProbeTypes.h"
#include "core/types/SceneEntityUuid.h"
#include "scene/SceneWorld.h"
#include "scene/components/ReflectionProbeComponent.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Iridium {

    inline constexpr uint32_t kMaximumReflectionProbeCandidates = 4;
    inline constexpr uint32_t kMaximumBlendedReflectionProbes = 2;

    struct ReflectionProbeCandidate {
        SceneEntityUuid owner;
        ReflectionProbeComponent probe;
        glm::mat4 worldToProbe{ 1.0f };
        glm::mat4 probeToWorld{ 1.0f };
        bool resident = false;
        // Runtime captures publish directly into the backend-neutral indexed
        // environment table without inventing an asset GUID.
        std::optional<uint32_t> runtimeEnvironmentSlot;
    };

    struct SelectedReflectionProbe {
        uint32_t candidateIndex = 0;
        SceneEntityUuid owner;
        float influence = 0.0f;
        float weight = 0.0f;
    };

    struct ReflectionProbeSelection {
        std::array<SelectedReflectionProbe,
            kMaximumBlendedReflectionProbes> probes{};
        uint32_t count = 0;
        uint32_t influencingCandidateCount = 0;
        float globalEnvironmentWeight = 1.0f;
        bool useGlobalEnvironment = true;
    };

    enum class ReflectionProbeExtractionDiagnosticCode {
        MissingIdentity,
        MissingTransform,
        InvalidTransform,
        InvalidProbe,
    };

    struct ReflectionProbeExtractionDiagnostic {
        ReflectionProbeExtractionDiagnosticCode code =
            ReflectionProbeExtractionDiagnosticCode::InvalidProbe;
        SceneEntityUuid owner;
        std::string propertyPath;
        std::string message;
    };

    struct ReflectionProbeExtractionStats {
        uint32_t sceneProbeCount = 0;
        uint32_t candidateCount = 0;
        uint32_t residentCount = 0;
        uint32_t omittedCount = 0;
    };

    struct ReflectionProbeFramePacket {
        std::vector<ReflectionProbeCandidate> candidates;
        std::vector<ReflectionProbeExtractionDiagnostic> diagnostics;
        ReflectionProbeExtractionStats stats;
    };

    using ReflectionProbeResidencyFn =
        std::function<bool(AssetGuid environment)>;
    using ReflectionProbeEnvironmentSlotFn =
        std::function<std::optional<uint32_t>(AssetGuid environment)>;

    // Extracts deterministic, scale-independent probe transforms. Without an
    // explicit residency callback, only the component's matching resolved GUID
    // is considered resident.
    [[nodiscard]] ReflectionProbeFramePacket extractReflectionProbes(
        const SceneWorld& world,
        ReflectionProbeResidencyFn residency = {});

    // M7R R5c.6: the same extraction written over a persistent packet. The
    // result equals extractReflectionProbes(world, residency); the packet's
    // candidate and diagnostic storage (including their strings) is reused, so
    // a steady frame does not allocate.
    void extractReflectionProbes(const SceneWorld& world,
        const ReflectionProbeResidencyFn& residency,
        ReflectionProbeFramePacket& packet);

    struct ReflectionProbePublicationConfig {
        uint32_t initialCapacity = kInitialGpuReflectionProbeCapacity;
        uint32_t maximumCapacity = kMaximumGpuReflectionProbeCapacity;
    };

    // M7R R5c.6: change-driven. Each slot remembers the exact inputs its record
    // and selection metadata were packed from; an existing probe whose inputs are
    // bit-identical is not re-packed (the full path's write would compare equal).
    // The removal walk and active-list rebuild run only when membership can have
    // changed. Records, revisions, the active list and its revision are exactly
    // those of the full path (the test-only ReferenceReflectionProbePublisher).
    class ReflectionProbePublisher final {
    public:
        explicit ReflectionProbePublisher(
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
        // Every value packedProbe() and the selection metadata read, laid out
        // without padding so it compares bitwise.
        struct PackInputs {
            SceneEntityUuid owner;
            glm::mat4 worldToProbe{ 1.0f };
            glm::vec3 position{ 0.0f };
            float sphereRadiusMeters = 0.0f;
            glm::vec3 boxExtentsMeters{ 0.0f };
            float blendDistanceMeters = 0.0f;
            float intensity = 0.0f;
            uint32_t shape = 0;
            uint32_t parallaxMode = 0;
            int32_t priority = 0;
            uint32_t environmentSlot = 0;
            uint32_t selectionRank = 0;
        };
        static_assert(sizeof(PackInputs) == 16 + 64 + 12 + 4 + 12 + 4 * 7,
            "PackInputs must have no padding");

        [[nodiscard]] static PackInputs packInputs(
            const PublishCandidate& candidate) noexcept;
        void packSlot(uint32_t slot, const PublishCandidate& candidate);
        void ensureCapacity(uint32_t required);
        void writeRecord(uint32_t slot,
            const PackedGpuReflectionProbe& record);
        void clearRecord(uint32_t slot);
        void buildChangedRanges();
        void advanceRevision(uint64_t& value) noexcept;

        ReflectionProbePublicationConfig config_;
        // The inputs records_[slot] was last written from; slotPacked_[slot]
        // is 0 after a clear, a reset or a capacity growth.
        std::vector<PackInputs> slotInputs_;
        std::vector<uint8_t> slotPacked_;
        bool membershipChanged_ = true;
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

    [[nodiscard]] float reflectionProbeInfluence(
        const ReflectionProbeCandidate& candidate,
        glm::vec3 worldPosition) noexcept;

    [[nodiscard]] ReflectionProbeSelection selectReflectionProbes(
        std::span<const ReflectionProbeCandidate> candidates,
        glm::vec3 worldPosition) noexcept;

    // Returns the direction from the probe center to the box-projected hit.
    // Invalid/outside inputs safely retain the normalized reflection direction.
    [[nodiscard]] glm::vec3 boxProjectedReflectionDirection(
        const ReflectionProbeCandidate& candidate,
        glm::vec3 worldPosition,
        glm::vec3 worldReflectionDirection) noexcept;

    // M7.10.5: true unless the light provably contributes nothing to a
    // capture at capturePosition. Directional lights always reach it. The
    // capture clips depth (no depth clamp), so it shades only points inside
    // the cube of half-extent captureFarMeters around the capture position,
    // and a local light contributes W(d,r) = 0 at d >= r; a light whose range
    // sphere misses that cube (with a conservative margin) cannot reach it.
    [[nodiscard]] bool lightCanReachReflectionProbeCapture(
        const PackedGpuLight& light, glm::vec3 capturePosition,
        float captureFarMeters) noexcept;

    // M7.10.5: the light-record slots a probe capture evaluates: every slot
    // below the highest active slot + 1 (cleared slots contribute nothing),
    // so an active light in a high slot left by a removal is still lit.
    [[nodiscard]] uint32_t reflectionProbeCaptureLightSlotBound(
        const LightingFramePacket& lights) noexcept;

    // M7.10.5: the lighting revision of one realtime probe capture. Hashes
    // (slot, record revision, evaluated) for every active light that can
    // reach the capture, so an edit to an out-of-reach light, or adding or
    // removing one, leaves it unchanged, while an edit to a reaching light
    // or a light entering or leaving reach changes it. Every active light
    // lies inside the capture's slot bound. Allocation-free; never zero.
    [[nodiscard]] uint64_t reflectionProbeCaptureLightingRevision(
        const LightingFramePacket& lights, glm::vec3 capturePosition,
        float captureFarMeters) noexcept;

} // namespace Iridium
