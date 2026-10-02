#pragma once

#include "renderer/rhi/ReflectionProbeTypes.h"
#include "scene/SceneEntityUuid.h"
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

    struct ReflectionProbePublicationConfig {
        uint32_t initialCapacity = kInitialGpuReflectionProbeCapacity;
        uint32_t maximumCapacity = kMaximumGpuReflectionProbeCapacity;
    };

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

} // namespace Iridium
