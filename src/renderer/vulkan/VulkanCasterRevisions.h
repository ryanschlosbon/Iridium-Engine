#pragma once

// M7R R5c.1 / R5c.2: change-driven caster and depth-content revisions.
//
// Until R5c the shadow-caster revision (local shadows, reflection-probe scene
// revision, the VSM clip key), the per-cascade directional revisions and the
// depth-history content revision were FNV-1a hashes computed one byte at a time
// over every caster, every frame. Here each is a monotonic revision that
// advances exactly when the caster content it summarizes changes:
//
//   - Change triggers: the published shadow list's membership revision and
//     content watermark (GpuScene.cpp), the GPU-scene publication revision
//     (depth history), the material registry's revision, a field-by-field
//     check of the frame's direct (non-GPU-scene) packets, and for the depth
//     history a scan of the queues' GPU-scene primitive indices. On an
//     untriggered frame no caster content is read.
//   - On a triggered frame the caster sequence is rebuilt from the same
//     resolution as before (visitIndirectCasters, or the queue packets) and
//     compared field by field with the previous sequence, over exactly the
//     bytes the hash consumed. The revision advances only when they differ, so
//     its change frames are the hash's change frames.
//   - A directional cascade's revision compares the ordered casters whose
//     bounds touch the cascade, recomputing membership only when the cascade's
//     clip matrix or the sequence changed.
//
// Deliberate differences, both cache-safe (a cache re-renders identical
// content): content that changes A -> B -> A gets three distinct revisions
// where the hash returned to A's value, so a cache that last rendered A
// re-renders; and a directional light that was not evaluated in a frame where
// the casters changed treats all of its cascades as changed.

#include "VulkanIndirectCullerShared.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/RenderFrame.h"
#include "renderer/rhi/ShadowTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Iridium {

    class VulkanResourceRegistry;

    // The backend-owned material state a caster's content includes.
    struct VulkanCasterMaterialSource {
        const void* owner = nullptr;
        // False when `handle` has no live payload.
        bool (*lookup)(const void* owner, MaterialHandle handle,
            uint64_t& packedRevision, uint32_t& alphaMode,
            uint32_t& doubleSided) = nullptr;
        // Advances whenever any material payload is allocated, updated or freed.
        uint64_t revision = 0;
    };
    [[nodiscard]] VulkanCasterMaterialSource vulkanCasterMaterials(
        const VulkanResourceRegistry& resources) noexcept;

    // One caster's content: exactly the fields the retired FNV-1a revision
    // appended (world transform, handles, index range and, when the material
    // payload is live, its packed revision, alpha mode and double-sidedness).
    struct VulkanCasterContent {
        glm::mat4 worldTransform{ 1.0f };
        uint32_t geometry = 0;
        uint32_t material = 0;
        uint32_t pipeline = 0;
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        uint32_t materialPresent = 0;
        uint64_t materialPackedRevision = 0;
        uint32_t materialAlphaMode = 0;
        uint32_t materialDoubleSided = 0;
    };
    static_assert(sizeof(VulkanCasterContent) == 104,
        "VulkanCasterContent is compared bytewise and must have no padding");
    static_assert(std::is_trivially_copyable_v<VulkanCasterContent>);

    void makeCasterContent(VulkanCasterContent& content,
        const VulkanResolvedCaster& caster,
        const VulkanCasterMaterialSource& materials) noexcept;
    void makeCasterContent(VulkanCasterContent& content,
        const DrawPacket& packet,
        const VulkanCasterMaterialSource& materials) noexcept;
    [[nodiscard]] bool sameCasterContent(const VulkanCasterContent& lhs,
        const VulkanCasterContent& rhs) noexcept;

    // The shadow-caster submission's revisions: one for the whole sequence
    // (local shadows, the probe scene revision, the VSM clip key) and one per
    // cascade of each directional light.
    class VulkanShadowCasterRevisions {
    public:
        // Every frame the GPU scene is published: the shadow list identity
        // (membership revision and size) and its content watermark.
        void publishScene(const GpuScenePackedTables& scene) noexcept;
        // Directional evaluations are keyed by their order within a frame,
        // which is the application's directional shadow index.
        void beginFrame() noexcept { directionalOrdinal_ = 0; }

        // Advances exactly when the resolved caster sequence changes.
        [[nodiscard]] uint64_t casterRevision(const VulkanIndirectScene& scene,
            const ShadowCasterSubmission& casters,
            const VulkanCasterMaterialSource& materials);
        // Per cascade: advances exactly when the sequence of casters whose
        // bounds touch the cascade, or one of their contents, changes.
        [[nodiscard]] std::array<uint64_t, kDirectionalShadowCascadeCount>
            directionalRevisions(const VulkanIndirectScene& scene,
                const ShadowCasterSubmission& casters,
                const VulkanCasterMaterialSource& materials,
                const DirectionalShadowCascadePlan& plan);
        // The ordinal of the latest directionalRevisions call in this frame.
        [[nodiscard]] uint32_t lastDirectionalOrdinal() const noexcept {
            return lastDirectionalOrdinal_;
        }

        struct Stats {
            uint64_t evaluations = 0;
            uint64_t rebuilds = 0;          // triggered sequence rebuilds
            uint64_t sequenceChanges = 0;   // rebuilds that changed content or bounds
            uint64_t directionalMaskPasses = 0;
        };
        [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    private:
        struct Published {
            bool valid = false;
            uint64_t membershipRevision = 0;
            uint64_t contentWatermark = 0;
            size_t size = 0;
        };
        struct Trigger {
            bool valid = false;
            bool identified = false;
            uint64_t membershipRevision = 0;
            uint64_t contentWatermark = 0;
            size_t gpuCount = 0;
            uint64_t materialRevision = 0;
            bool operator==(const Trigger&) const = default;
        };
        struct Directional {
            bool valid = false;
            uint64_t sequenceRevision = 0;
            std::array<glm::mat4, kDirectionalShadowCascadeCount> matrices{};
            std::array<uint64_t, kDirectionalShadowCascadeCount> revisions{};
            std::vector<uint8_t> masks;
        };

        void refresh(const VulkanIndirectScene& scene,
            const ShadowCasterSubmission& casters,
            const VulkanCasterMaterialSource& materials);
        [[nodiscard]] bool directPacketsChanged(
            std::span<const DrawPacket> packets) const noexcept;
        // Whether the casters of `previous` in cascade `bit` (by
        // `previousMasks`) equal those of `current` (by `currentMasks`), in
        // order and content: the retired per-cascade hash input.
        [[nodiscard]] static bool sameCascadeSequence(
            std::span<const VulkanCasterContent> previous,
            std::span<const uint8_t> previousMasks,
            std::span<const VulkanCasterContent> current,
            std::span<const uint8_t> currentMasks, uint32_t bit) noexcept;

        Published published_{};
        Trigger trigger_{};
        // The current resolved sequence: GPU-scene casters, then directs.
        std::vector<VulkanCasterContent> contents_;
        std::vector<glm::vec4> spheres_;
        size_t directStart_ = 0;
        // Rebuild targets. After a change they hold the previous sequence
        // (previousAvailable_) until the next rebuild reuses them.
        std::vector<VulkanCasterContent> scratchContents_;
        std::vector<glm::vec4> scratchSpheres_;
        bool previousAvailable_ = false;
        // Advances when contents or bounds change (directional streams).
        uint64_t sequenceRevision_ = 0;
        uint64_t nextRevision_ = 1;
        uint64_t revision_ = nextRevision_++;
        std::vector<Directional> directional_;
        std::vector<uint8_t> maskScratch_;
        uint32_t directionalOrdinal_ = 0;
        uint32_t lastDirectionalOrdinal_ = 0;
        Stats stats_{};
    };

} // namespace Iridium
