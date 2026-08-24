#pragma once

#include "renderer/rhi/DrawPacket.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Iridium {

    inline constexpr uint32_t kLayeredInterfaceOrientationBit = 0x80000000u;
    inline constexpr uint32_t kLayeredInterfaceWorkMask = 0x7fffffffu;
    inline constexpr uint32_t kMaximumLayeredWorkTableIndex = 0x7ffffffeu;
    inline constexpr uint32_t kMaximumLayeredInterfaceCount = 8u;
    inline constexpr uint32_t kLayeredCaptureMirrored = 1u << 0u;
    inline constexpr uint32_t kLayeredCaptureHasPrevious = 1u << 1u;
    inline constexpr uint32_t kLayeredCaptureRequirePairedOrientation =
        1u << 2u;
    inline constexpr uint32_t kLayeredCaptureHasTerminationMask = 1u << 3u;
    inline constexpr uint32_t kLayeredCaptureExit =
        kLayeredCaptureHasPrevious;

    // Deep 4/8-interface tiers reuse the R32 identity attachment to carry the
    // bounded work identity plus conservative transport state. Ordinary2 keeps
    // the original 31-bit work identity contract above. The deep atlas admits
    // at most 4096 work items, so thirteen bits retain a zero sentinel and all
    // one-based work identities.
    inline constexpr uint32_t kDeepLayeredWorkMask = 0x00001fffu;
    inline constexpr uint32_t kDeepLayeredTransmissionShift = 13u;
    inline constexpr uint32_t kDeepLayeredTransmissionBits = 14u;
    inline constexpr uint32_t kDeepLayeredTransmissionMaximum =
        (1u << kDeepLayeredTransmissionBits) - 1u;
    inline constexpr uint32_t kDeepLayeredTransmissionMask =
        kDeepLayeredTransmissionMaximum <<
        kDeepLayeredTransmissionShift;
    inline constexpr uint32_t kDeepLayeredOpenCountShift = 27u;
    inline constexpr uint32_t kDeepLayeredOpenCountMask = 0x78000000u;
    inline constexpr uint32_t kDeepLayeredInvalidOpenCount = 15u;
    inline constexpr uint32_t kDeepLayeredEarlyTerminationTileSize = 16u;
    inline constexpr uint32_t kDeepLayeredTerminationThresholdQuantized =
        (kDeepLayeredTransmissionMaximum + 1023u) / 1024u;

    [[nodiscard]] constexpr uint32_t deepLayeredWorkIdentity(
        uint32_t packed) noexcept {
        return packed & kDeepLayeredWorkMask;
    }

    [[nodiscard]] constexpr uint32_t deepLayeredTransmissionQuantized(
        uint32_t packed) noexcept {
        return (packed & kDeepLayeredTransmissionMask) >>
            kDeepLayeredTransmissionShift;
    }

    [[nodiscard]] constexpr uint32_t deepLayeredOpenCount(
        uint32_t packed) noexcept {
        return (packed & kDeepLayeredOpenCountMask) >>
            kDeepLayeredOpenCountShift;
    }

    [[nodiscard]] constexpr bool deepLayeredTerminationInterface(
        uint32_t interfaceIndex, uint32_t interfaceCount) noexcept {
        return (interfaceIndex & 1u) != 0u &&
            interfaceIndex + 1u < interfaceCount;
    }

    [[nodiscard]] constexpr uint32_t deepLayeredTerminationMaskCount(
        uint32_t interfaceCount) noexcept {
        return interfaceCount >= 2u ? (interfaceCount - 2u) / 2u : 0u;
    }

    [[nodiscard]] constexpr uint32_t deepLayeredPriorTerminationInterface(
        uint32_t interfaceIndex) noexcept {
        return interfaceIndex < 2u
            ? 0u : ((interfaceIndex - 2u) / 2u) * 2u + 1u;
    }

    enum class LayeredFaceOrientation : uint8_t {
        Entry,
        Exit,
    };

    [[nodiscard]] constexpr LayeredFaceOrientation layeredFaceOrientation(
        bool rasterFrontFacing, bool mirroredTransform) noexcept {
        return rasterFrontFacing != mirroredTransform
            ? LayeredFaceOrientation::Entry
            : LayeredFaceOrientation::Exit;
    }

    // R32_UINT peel identity. Zero is the cleared/unoccupied value. The low
    // 31 bits store a one-based index into the frame's TransparentWorkIdentity
    // table and the high bit stores semantic entry/exit orientation. Keeping
    // the GUID tuple in a separate table preserves stable identity without
    // expanding every per-pixel interface record beyond the approved D32+R32.
    struct PackedLayeredInterfaceIdentity {
        uint32_t value = 0u;

        [[nodiscard]] constexpr bool occupied() const noexcept {
            return (value & kLayeredInterfaceWorkMask) != 0u;
        }

        [[nodiscard]] constexpr LayeredFaceOrientation orientation() const
            noexcept {
            return (value & kLayeredInterfaceOrientationBit) != 0u
                ? LayeredFaceOrientation::Exit
                : LayeredFaceOrientation::Entry;
        }

        [[nodiscard]] constexpr uint32_t workTableIndex() const noexcept {
            const uint32_t oneBased = value & kLayeredInterfaceWorkMask;
            return oneBased == 0u ? 0u : oneBased - 1u;
        }

        friend constexpr bool operator==(
            PackedLayeredInterfaceIdentity,
            PackedLayeredInterfaceIdentity) = default;
    };

    [[nodiscard]] constexpr PackedLayeredInterfaceIdentity
        packLayeredInterfaceIdentity(uint32_t workTableIndex,
            LayeredFaceOrientation orientation) noexcept {
        if (workTableIndex > kMaximumLayeredWorkTableIndex)
            return {};
        return {
            (workTableIndex + 1u) |
            (orientation == LayeredFaceOrientation::Exit
                ? kLayeredInterfaceOrientationBit
                : 0u)
        };
    }

    static_assert(sizeof(PackedLayeredInterfaceIdentity) == sizeof(uint32_t));

    enum class LayeredCaptureCandidateStatus : uint8_t {
        Accepted,
        InvalidDepth,
        WorkIndexExceeded,
        OpaqueOccluded,
        OrientationMismatch,
        MissingEntry,
        EntryWorkMismatch,
        NotBehindEntry,
    };

    struct LayeredCaptureCandidateResult {
        LayeredCaptureCandidateStatus status =
            LayeredCaptureCandidateStatus::InvalidDepth;
        PackedLayeredInterfaceIdentity identity;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == LayeredCaptureCandidateStatus::Accepted;
        }
    };

    // CPU reference for the entry/exit fragment acceptance implemented by
    // layered_interface_capture.frag. Depths use the production non-reversed
    // Vulkan convention: smaller is nearer and opaque depth clears to one.
    [[nodiscard]] inline LayeredCaptureCandidateResult
        evaluateLayeredCaptureCandidate(float candidateDepth,
            float opaqueDepth, uint32_t workTableIndex,
            bool rasterFrontFacing, bool mirroredTransform,
            bool captureExit, float entryDepth = 0.0f,
            PackedLayeredInterfaceIdentity entryIdentity = {}) noexcept {
        if (!std::isfinite(candidateDepth) ||
            !std::isfinite(opaqueDepth) || candidateDepth < 0.0f ||
            candidateDepth > 1.0f || opaqueDepth < 0.0f ||
            opaqueDepth > 1.0f) {
            return { LayeredCaptureCandidateStatus::InvalidDepth, {} };
        }
        const LayeredFaceOrientation orientation = layeredFaceOrientation(
            rasterFrontFacing, mirroredTransform);
        const LayeredFaceOrientation required = captureExit
            ? LayeredFaceOrientation::Exit
            : LayeredFaceOrientation::Entry;
        const PackedLayeredInterfaceIdentity packed =
            packLayeredInterfaceIdentity(workTableIndex, required);
        if (!packed.occupied()) {
            return { LayeredCaptureCandidateStatus::WorkIndexExceeded, {} };
        }
        if (candidateDepth > opaqueDepth) {
            return { LayeredCaptureCandidateStatus::OpaqueOccluded, {} };
        }
        if (orientation != required) {
            return { LayeredCaptureCandidateStatus::OrientationMismatch, {} };
        }
        if (captureExit) {
            if (!std::isfinite(entryDepth) || entryDepth < 0.0f ||
                entryDepth > 1.0f) {
                return { LayeredCaptureCandidateStatus::InvalidDepth, {} };
            }
            if (!entryIdentity.occupied() || entryIdentity.orientation() !=
                    LayeredFaceOrientation::Entry) {
                return { LayeredCaptureCandidateStatus::MissingEntry, {} };
            }
            if (entryIdentity.workTableIndex() != workTableIndex) {
                return { LayeredCaptureCandidateStatus::EntryWorkMismatch, {} };
            }
            if (candidateDepth <= entryDepth) {
                return { LayeredCaptureCandidateStatus::NotBehindEntry, {} };
            }
        }
        return { LayeredCaptureCandidateStatus::Accepted, packed };
    }

    enum class LayeredPeelCandidateStatus : uint8_t {
        Accepted,
        InvalidDepth,
        WorkIndexExceeded,
        OpaqueOccluded,
        MissingPreviousInterface,
        NotBehindPreviousInterface,
    };

    struct LayeredPeelCandidateResult {
        LayeredPeelCandidateStatus status =
            LayeredPeelCandidateStatus::InvalidDepth;
        PackedLayeredInterfaceIdentity identity;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == LayeredPeelCandidateStatus::Accepted;
        }
    };

    // CPU reference for the indexed 2/4/8 peel shader. Each pass captures the
    // nearest visible interface behind the preceding pass. Work and orientation
    // deliberately may differ from the previous sample: Entry(A), Entry(B),
    // Exit(B), Exit(A) is the canonical nested-shell sequence.
    [[nodiscard]] inline LayeredPeelCandidateResult
        evaluateLayeredPeelCandidate(float candidateDepth,
            float opaqueDepth, uint32_t workTableIndex,
            bool rasterFrontFacing, bool mirroredTransform,
            bool hasPreviousInterface, float previousDepth = 0.0f,
            PackedLayeredInterfaceIdentity previousIdentity = {}) noexcept {
        if (!std::isfinite(candidateDepth) ||
            !std::isfinite(opaqueDepth) || candidateDepth < 0.0f ||
            candidateDepth > 1.0f || opaqueDepth < 0.0f ||
            opaqueDepth > 1.0f) {
            return { LayeredPeelCandidateStatus::InvalidDepth, {} };
        }
        const PackedLayeredInterfaceIdentity packed =
            packLayeredInterfaceIdentity(workTableIndex,
                layeredFaceOrientation(rasterFrontFacing,
                    mirroredTransform));
        if (!packed.occupied()) {
            return { LayeredPeelCandidateStatus::WorkIndexExceeded, {} };
        }
        if (candidateDepth > opaqueDepth) {
            return { LayeredPeelCandidateStatus::OpaqueOccluded, {} };
        }
        if (hasPreviousInterface) {
            if (!std::isfinite(previousDepth) || previousDepth < 0.0f ||
                previousDepth > 1.0f) {
                return { LayeredPeelCandidateStatus::InvalidDepth, {} };
            }
            if (!previousIdentity.occupied()) {
                return {
                    LayeredPeelCandidateStatus::MissingPreviousInterface, {}
                };
            }
            if (candidateDepth <= previousDepth) {
                return { LayeredPeelCandidateStatus::
                    NotBehindPreviousInterface, {} };
            }
        }
        return { LayeredPeelCandidateStatus::Accepted, packed };
    }

    struct alignas(16) LayeredInterfaceCapturePushConstants {
        glm::mat4 renderMatrix{ 1.0f };
        uint32_t materialIndex = 0u;
        uint32_t workTableIndex = 0u;
        uint32_t flags = 0u;
        uint32_t packedViewportOffset = 0u;
    };

    [[nodiscard]] constexpr bool validLayeredViewportOffset(
        int64_t x, int64_t y) noexcept {
        return x >= (std::numeric_limits<int16_t>::min)() &&
            x <= (std::numeric_limits<int16_t>::max)() &&
            y >= (std::numeric_limits<int16_t>::min)() &&
            y <= (std::numeric_limits<int16_t>::max)();
    }

    [[nodiscard]] constexpr uint32_t packLayeredViewportOffset(
        int32_t x, int32_t y) noexcept {
        return (static_cast<uint32_t>(x) & 0xffffu) |
            ((static_cast<uint32_t>(y) & 0xffffu) << 16u);
    }

    [[nodiscard]] constexpr int32_t layeredViewportOffsetX(
        uint32_t packed) noexcept {
        return static_cast<int32_t>((packed & 0xffffu) ^ 0x8000u) -
            0x8000;
    }

    [[nodiscard]] constexpr int32_t layeredViewportOffsetY(
        uint32_t packed) noexcept {
        return static_cast<int32_t>((packed >> 16u) ^ 0x8000u) -
            0x8000;
    }

    static_assert(sizeof(LayeredInterfaceCapturePushConstants) == 80u);
    static_assert(std::is_trivially_copyable_v<
        LayeredInterfaceCapturePushConstants>);

    enum class LayeredWorkTableInsertStatus : uint8_t {
        Inserted,
        Existing,
        CapacityExceeded,
    };

    struct LayeredWorkTableInsertResult {
        LayeredWorkTableInsertStatus status =
            LayeredWorkTableInsertStatus::CapacityExceeded;
        uint32_t workTableIndex = 0u;
        uint32_t probeCount = 0u;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status != LayeredWorkTableInsertStatus::CapacityExceeded;
        }
    };

    struct LayeredWorkTableStats {
        uint32_t uniqueWorkCount = 0u;
        uint32_t duplicateWorkCount = 0u;
        uint32_t rejectedWorkCount = 0u;
        uint64_t hashProbeCount = 0u;
        uint32_t maximumProbeCount = 0u;
    };

    [[nodiscard]] constexpr uint64_t hashTransparentWorkIdentity(
        const TransparentWorkIdentity& identity) noexcept {
        uint64_t hash = 14695981039346656037ull;
        const auto append = [&hash](const auto& bytes) constexpr {
            for (const uint8_t byte : bytes) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
        };
        append(identity.owner.bytes());
        append(identity.sourcePrimitiveGuid.bytes());
        append(identity.primitiveGuid.bytes());
        append(identity.materialGuid.bytes());
        return hash;
    }

    // Fixed-capacity, allocation-free table used to turn stable GUID tuples into
    // the compact per-frame indices written to the R32 interface attachments.
    // Reset touches only slots populated by the previous frame, avoiding a full
    // table clear. Capacity overflow is explicit so work can take the ThinGlass
    // fallback instead of growing a container or stalling the frame allocator.
    template <size_t Capacity>
    class BoundedLayeredWorkTable final {
    public:
        static_assert(Capacity > 0u);
        static_assert(Capacity <=
            static_cast<size_t>(kMaximumLayeredWorkTableIndex) + 1u);
        static constexpr size_t SlotCount = std::bit_ceil(Capacity * 2u);

        BoundedLayeredWorkTable() = default;
        BoundedLayeredWorkTable(const BoundedLayeredWorkTable&) = delete;
        BoundedLayeredWorkTable& operator=(
            const BoundedLayeredWorkTable&) = delete;

        void reset() noexcept {
            for (size_t index = 0; index < size_; ++index)
                slots_[occupiedSlots_[index]] = 0u;
            size_ = 0u;
            stats_ = {};
        }

        [[nodiscard]] LayeredWorkTableInsertResult insert(
            const TransparentWorkIdentity& identity) noexcept {
            size_t slot = static_cast<size_t>(
                hashTransparentWorkIdentity(identity)) & (SlotCount - 1u);
            uint32_t probes = 1u;
            for (; probes <= SlotCount; ++probes) {
                const uint32_t oneBased = slots_[slot];
                if (oneBased == 0u) {
                    if (size_ == Capacity) {
                        ++stats_.rejectedWorkCount;
                        accountProbes(probes);
                        return {
                            LayeredWorkTableInsertStatus::CapacityExceeded,
                            0u, probes };
                    }
                    const uint32_t workIndex = static_cast<uint32_t>(size_);
                    identities_[size_] = identity;
                    occupiedSlots_[size_] = static_cast<uint32_t>(slot);
                    slots_[slot] = workIndex + 1u;
                    ++size_;
                    stats_.uniqueWorkCount = static_cast<uint32_t>(size_);
                    accountProbes(probes);
                    return { LayeredWorkTableInsertStatus::Inserted,
                        workIndex, probes };
                }
                const uint32_t workIndex = oneBased - 1u;
                if (identities_[workIndex] == identity) {
                    ++stats_.duplicateWorkCount;
                    accountProbes(probes);
                    return { LayeredWorkTableInsertStatus::Existing,
                        workIndex, probes };
                }
                slot = (slot + 1u) & (SlotCount - 1u);
            }
            ++stats_.rejectedWorkCount;
            accountProbes(probes);
            return { LayeredWorkTableInsertStatus::CapacityExceeded,
                0u, probes };
        }

        [[nodiscard]] std::span<const TransparentWorkIdentity> identities()
            const noexcept {
            return { identities_.data(), size_ };
        }

        [[nodiscard]] size_t size() const noexcept { return size_; }
        [[nodiscard]] static constexpr size_t capacity() noexcept {
            return Capacity;
        }
        [[nodiscard]] const LayeredWorkTableStats& stats() const noexcept {
            return stats_;
        }

    private:
        void accountProbes(uint32_t probes) noexcept {
            stats_.hashProbeCount += probes;
            stats_.maximumProbeCount = (std::max)(
                stats_.maximumProbeCount, probes);
        }

        std::array<TransparentWorkIdentity, Capacity> identities_{};
        std::array<uint32_t, Capacity> occupiedSlots_{};
        std::array<uint32_t, SlotCount> slots_{};
        size_t size_ = 0u;
        LayeredWorkTableStats stats_{};
    };

    inline constexpr size_t kOrdinary2MaximumWorkCount = 4096u;
    using Ordinary2WorkTable =
        BoundedLayeredWorkTable<kOrdinary2MaximumWorkCount>;

    struct LayeredQualityTierContract {
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        uint32_t maximumInterfaceCount = 0u;
        uint32_t atlasAreaNumerator = 0u;
        uint32_t atlasAreaDenominator = 1u;
        bool explicitOnly = false;
        bool nonRefractiveResidualTail = false;

        [[nodiscard]] constexpr bool valid() const noexcept {
            return maximumInterfaceCount != 0u &&
                atlasAreaNumerator != 0u && atlasAreaDenominator != 0u;
        }
    };

    // Accepted M6 policy: ordinary work stores one shell in at most one quarter
    // of the scene, hero work stores two shells in at most one half, and the
    // explicit cinematic tier can consume the full scene. Overflow never grows
    // storage; it becomes a bounded, non-refractive residual tail.
    [[nodiscard]] constexpr LayeredQualityTierContract
        layeredQualityTierContract(TransparencyQuality quality) noexcept {
        switch (quality) {
        case TransparencyQuality::Ordinary2:
            return { quality, 2u, 1u, 4u, false, true };
        case TransparencyQuality::Hero4:
            return { quality, 4u, 1u, 2u, true, true };
        case TransparencyQuality::Cinematic8:
            return { quality, 8u, 1u, 1u, true, true };
        }
        return { quality };
    }

    [[nodiscard]] constexpr uint64_t layeredAtlasAreaCapPixels(
        uint32_t sceneWidth, uint32_t sceneHeight,
        TransparencyQuality quality) noexcept {
        const LayeredQualityTierContract tier =
            layeredQualityTierContract(quality);
        if (!tier.valid() || sceneWidth == 0u || sceneHeight == 0u)
            return 0u;
        return static_cast<uint64_t>(sceneWidth) * sceneHeight *
            tier.atlasAreaNumerator / tier.atlasAreaDenominator;
    }

    inline constexpr float kLayeredEarlyTerminationTransmittance =
        1.0f / 1024.0f;

    [[nodiscard]] inline bool layeredEarlyTerminationReached(
        float remainingTransmittance,
        float threshold = kLayeredEarlyTerminationTransmittance) noexcept {
        return std::isfinite(remainingTransmittance) &&
            std::isfinite(threshold) && threshold >= 0.0f &&
            threshold <= 1.0f && remainingTransmittance >= 0.0f &&
            remainingTransmittance <= threshold;
    }

    struct LayeredInterfaceSample {
        TransparentWorkIdentity work;
        float rayDistanceMeters = 0.0f;
        LayeredFaceOrientation orientation = LayeredFaceOrientation::Entry;
    };

    enum class LayeredInterfaceStackStatus : uint8_t {
        Exact,
        OverflowResidual,
        InvalidQuality,
        InvalidDistance,
        RepeatedEntryWithoutExit,
        MissingEntry,
        IncompleteVolume,
    };

    struct LayeredInterfaceStackReduction {
        LayeredInterfaceStackStatus status =
            LayeredInterfaceStackStatus::InvalidQuality;
        TransparencyQuality quality = TransparencyQuality::Ordinary2;
        uint32_t interfaceCapacity = 0u;
        uint32_t exactInterfaceCount = 0u;
        uint32_t residualInterfaceCount = 0u;
        uint32_t pairedVolumeCount = 0u;
        float residualStartMeters = 0.0f;
        float residualEndMeters = 0.0f;
        bool exactPrefixEndsInsideVolume = false;
        bool residualIsNonRefractive = false;

        [[nodiscard]] constexpr bool accepted() const noexcept {
            return status == LayeredInterfaceStackStatus::Exact ||
                status == LayeredInterfaceStackStatus::OverflowResidual;
        }

        [[nodiscard]] constexpr bool requiresResidualTail() const noexcept {
            return status == LayeredInterfaceStackStatus::OverflowResidual;
        }

        [[nodiscard]] constexpr uint32_t accountedInterfaceCount() const
            noexcept {
            return exactInterfaceCount + residualInterfaceCount;
        }
    };

    // CPU reference for the M6.6 tier/overflow contract. Samples are ordered
    // front-to-back. Pairing is by stable work identity rather than LIFO order,
    // so both nested and crossing closed volumes are valid. The exact prefix is
    // bounded by the authored tier and every remaining interface is explicitly
    // accounted to a non-refractive residual tail; no interface is silently lost.
    [[nodiscard]] inline LayeredInterfaceStackReduction
        reduceLayeredInterfaceStack(
            std::span<const LayeredInterfaceSample> interfaces,
            TransparencyQuality quality) noexcept {
        LayeredInterfaceStackReduction result;
        result.quality = quality;
        const LayeredQualityTierContract tier =
            layeredQualityTierContract(quality);
        if (!tier.valid()) return result;
        result.interfaceCapacity = tier.maximumInterfaceCount;

        float previousDistance = -1.0f;
        uint32_t openVolumeCount = 0u;
        uint32_t prefixOpenVolumeCount = 0u;
        for (size_t index = 0; index < interfaces.size(); ++index) {
            const LayeredInterfaceSample& sample = interfaces[index];
            if (!std::isfinite(sample.rayDistanceMeters) ||
                sample.rayDistanceMeters < 0.0f ||
                (index != 0u &&
                    sample.rayDistanceMeters <= previousDistance)) {
                result.status = LayeredInterfaceStackStatus::InvalidDistance;
                return result;
            }
            previousDistance = sample.rayDistanceMeters;

            int32_t workBalance = 0;
            for (size_t priorIndex = 0; priorIndex < index; ++priorIndex) {
                const LayeredInterfaceSample& prior = interfaces[priorIndex];
                if (prior.work != sample.work) continue;
                workBalance += prior.orientation ==
                    LayeredFaceOrientation::Entry ? 1 : -1;
            }
            if (sample.orientation == LayeredFaceOrientation::Entry) {
                if (workBalance != 0) {
                    result.status = LayeredInterfaceStackStatus::
                        RepeatedEntryWithoutExit;
                    return result;
                }
                ++openVolumeCount;
            }
            else {
                if (workBalance != 1 || openVolumeCount == 0u) {
                    result.status = LayeredInterfaceStackStatus::MissingEntry;
                    return result;
                }
                --openVolumeCount;
                ++result.pairedVolumeCount;
            }
            if (index + 1u == tier.maximumInterfaceCount)
                prefixOpenVolumeCount = openVolumeCount;
        }
        if (openVolumeCount != 0u) {
            result.status = LayeredInterfaceStackStatus::IncompleteVolume;
            return result;
        }

        result.exactInterfaceCount = static_cast<uint32_t>((std::min)(
            interfaces.size(),
            static_cast<size_t>(tier.maximumInterfaceCount)));
        result.residualInterfaceCount = static_cast<uint32_t>(
            interfaces.size() - result.exactInterfaceCount);
        if (result.residualInterfaceCount == 0u) {
            result.status = LayeredInterfaceStackStatus::Exact;
            return result;
        }
        result.status = LayeredInterfaceStackStatus::OverflowResidual;
        result.residualStartMeters =
            interfaces[result.exactInterfaceCount].rayDistanceMeters;
        result.residualEndMeters = interfaces.back().rayDistanceMeters;
        result.exactPrefixEndsInsideVolume = prefixOpenVolumeCount != 0u;
        result.residualIsNonRefractive = tier.nonRefractiveResidualTail;
        return result;
    }

    enum class Ordinary2PairingStatus : uint8_t {
        Paired,
        Incomplete,
        CapacityExceeded,
        WorkMismatch,
        OrientationMismatch,
        InvalidDistance,
        InvalidThicknessCap,
    };

    struct Ordinary2InterfacePair {
        Ordinary2PairingStatus status = Ordinary2PairingStatus::Incomplete;
        LayeredInterfaceSample entry;
        LayeredInterfaceSample exit;
        float measuredChordMeters = 0.0f;
        float participatingPathMeters = 0.0f;
        uint32_t rejectedInterfaceCount = 0;

        [[nodiscard]] bool paired() const noexcept {
            return status == Ordinary2PairingStatus::Paired;
        }
    };

    // CPU reference for the Ordinary2 GPU contract. Peeling supplies samples in
    // increasing ray distance. M6.5 accepts exactly one same-work entry/exit pair;
    // larger stacks are explicitly deferred to the bounded M6.6 residual tail.
    [[nodiscard]] inline Ordinary2InterfacePair pairOrdinary2Interfaces(
        std::span<const LayeredInterfaceSample> interfaces,
        float authoredMaximumThicknessMeters) noexcept {
        Ordinary2InterfacePair result;
        if (!std::isfinite(authoredMaximumThicknessMeters) ||
            authoredMaximumThicknessMeters < 0.0f) {
            result.status = Ordinary2PairingStatus::InvalidThicknessCap;
            return result;
        }
        if (interfaces.size() < 2) {
            result.status = Ordinary2PairingStatus::Incomplete;
            return result;
        }
        result.entry = interfaces[0];
        result.exit = interfaces[1];
        if (interfaces.size() > 2) {
            result.status = Ordinary2PairingStatus::CapacityExceeded;
            result.rejectedInterfaceCount = static_cast<uint32_t>(
                interfaces.size() - 2);
            return result;
        }
        if (result.entry.work != result.exit.work) {
            result.status = Ordinary2PairingStatus::WorkMismatch;
            return result;
        }
        if (result.entry.orientation != LayeredFaceOrientation::Entry ||
            result.exit.orientation != LayeredFaceOrientation::Exit) {
            result.status = Ordinary2PairingStatus::OrientationMismatch;
            return result;
        }
        if (!std::isfinite(result.entry.rayDistanceMeters) ||
            !std::isfinite(result.exit.rayDistanceMeters) ||
            result.entry.rayDistanceMeters < 0.0f ||
            result.exit.rayDistanceMeters <= result.entry.rayDistanceMeters) {
            result.status = Ordinary2PairingStatus::InvalidDistance;
            return result;
        }
        result.measuredChordMeters = result.exit.rayDistanceMeters -
            result.entry.rayDistanceMeters;
        result.participatingPathMeters = authoredMaximumThicknessMeters > 0.0f
            ? (std::min)(result.measuredChordMeters,
                authoredMaximumThicknessMeters)
            : result.measuredChordMeters;
        result.status = Ordinary2PairingStatus::Paired;
        return result;
    }

} // namespace Iridium
