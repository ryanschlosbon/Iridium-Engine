#pragma once

// Qualification-only caster-revision equivalence oracle (M7R R5c.1/R5c.2,
// --qualification-caster-revision-oracle).
//
// R5c replaced three per-frame FNV-1a hashes over every caster with
// change-driven monotonic revisions (renderer/vulkan/VulkanCasterRevisions.h).
// For every evaluation the backend reports, this oracle recomputes the retired
// hash from the same inputs, exactly as the pre-R5c code did, and compares the
// change relation per stream (the shadow submission, each cascade of each
// directional light, the depth history and, since R5c.5, the published shadow
// and probe membership revisions) against the previous evaluation:
//
//   - unchanged / both changed: equivalent;
//   - revision only: the revision advanced while the hash did not. This is the
//     documented cache-safe direction (a directional light that was not
//     evaluated in a frame where the casters changed treats every cascade as
//     changed); it is counted and listed but does not fail the run. Content
//     returning to an earlier state (A -> B -> A) changes both, frame by frame;
//   - hash only: the revision missed a content change. This fails the oracle.
//
// It prints IRIDIUM_CASTER_REVISION_DIVERGENCE lines (the first 32) and, from
// finish(), one IRIDIUM_CASTER_REVISION_ORACLE summary with "passed".

#include "renderer/vulkan/VulkanBackendExtension.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace Iridium {

    struct VulkanIndirectScene;
    class VulkanResourceRegistry;

    // The retired per-frame revisions, bit for bit (pre-R5c VulkanShadowCasters
    // and VulkanOpaqueFeature::prepareDepthHistory).
    [[nodiscard]] uint64_t legacyShadowCasterHash(const VulkanIndirectScene& scene,
        const VulkanResourceRegistry& resources,
        const ShadowCasterSubmission& casters) noexcept;
    [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
        legacyDirectionalShadowCasterHashes(const VulkanIndirectScene& scene,
            const VulkanResourceRegistry& resources,
            const ShadowCasterSubmission& casters,
            const DirectionalShadowCascadePlan& plan) noexcept;
    [[nodiscard]] uint64_t legacyDepthContentHash(const VulkanIndirectScene& scene,
        const VulkanResourceRegistry& resources,
        std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> forwardQueue) noexcept;

    class VulkanCasterRevisionOracle final : public IVulkanCasterRevisionObserver {
    public:
        enum class Relation : uint8_t { Unchanged, BothChanged, RevisionOnly, HashOnly };
        static constexpr uint32_t StreamCount = 4;

        struct Totals {
            uint64_t samples = 0;     // compared evaluations (after the first)
            uint64_t unchanged = 0;
            uint64_t bothChanged = 0;
            uint64_t revisionOnly = 0;
            uint64_t hashOnly = 0;
        };

        explicit VulkanCasterRevisionOracle(std::ostream* output = nullptr) noexcept
            : output_(output) {}
        void setOutput(std::ostream* output) noexcept { output_ = output; }

        void observeCasterRevision(const VulkanCasterRevisionSample& sample) override;
        // The comparison core: one stream's new (hash, revision) pair against
        // its previous one. Public for tests.
        [[nodiscard]] Relation compare(VulkanCasterRevisionStream stream,
            uint32_t ordinal, uint32_t cascade, uint64_t frameSerial,
            uint64_t hash, uint64_t revision);

        // Prints the summary (once).
        void finish();

        [[nodiscard]] const Totals& totals(
            VulkanCasterRevisionStream stream) const noexcept {
            return totals_[static_cast<size_t>(stream)];
        }
        [[nodiscard]] bool passed() const noexcept;

    private:
        struct State {
            bool valid = false;
            uint64_t hash = 0;
            uint64_t revision = 0;
        };
        [[nodiscard]] State& state(VulkanCasterRevisionStream stream,
            uint32_t ordinal, uint32_t cascade);

        std::ostream* output_ = nullptr;
        State shadow_{};
        State depth_{};
        std::array<State, 2> membership_{};   // shadow, probe
        std::vector<State> directional_;   // ordinal * cascade count + cascade
        std::array<Totals, StreamCount> totals_{};
        uint32_t printedDivergences_ = 0;
        bool finished_ = false;
    };

} // namespace Iridium
