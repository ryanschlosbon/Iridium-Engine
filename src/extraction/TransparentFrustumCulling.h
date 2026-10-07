#pragma once

// M7.10.1: conservative main-view frustum culling of transparent work.
//
// RenderExtractor builds one GpuSceneFrustum per extract from the unjittered
// projection * view (scene and asset-preview views alike) and rejects a
// transparent packet whose world AABB lies outside a frustum plane, or whose
// view-space depth interval misses [near, far]. A GPU-scene owner that visits
// its cached transparent submesh list is first tested whole, against the
// world AABB of the list's local bound union; a rejected owner skips the
// per-submesh work. Invalid bounds are never rejected (fail visible).
//
// Culled work keeps two side effects of the work it replaces:
//   - its PreviousTransformCache keys are touched with this frame's
//     transform, so a packet that re-enters view next frame resolves the
//     previous transform it would have resolved without culling;
//   - its TransparentResidencyDemand travels with the frame
//     (RenderFrame::culledTransparentDemand), so residency hysteresis follows
//     the requested work.
// Each extraction chunk records its culled work here; the record is replayed
// into the cache after the queues' previous transforms are resolved.

#include "extraction/PreviousTransformCache.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/GpuSceneVisibility.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

struct RenderInstanceBatchComponent;

namespace Iridium {

    struct ModelAsset;

    struct TransparentCullRecord {
        // One culled packet: its cache key and this frame's world transform.
        struct Packet {
            PreviousTransformCache::Key key;
            glm::mat4 world{ 1.0f };
        };
        // One owner whose transparent submesh list was rejected whole. The
        // span views the cached list (valid for the frame).
        struct Model {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            std::span<const uint32_t> submeshes;
            glm::mat4 world{ 1.0f };
        };

        std::vector<Packet> packets;
        std::vector<Model> models;
        // TransparentResidencyDemand of the individually culled packets
        // (rejected models add theirs when replayed).
        uint32_t packetDemand = 0;

        void clear() noexcept {
            packets.clear();
            models.clear();
            packetDemand = 0;
        }

        // Records a packet culled after its interval was prepared.
        void cull(const DrawPacket& packet) {
            packets.push_back({ { packet.owner, packet.primitiveGuid },
                packet.worldTransform });
            packetDemand |= transparentPacketDemand(packet);
        }

        [[nodiscard]] static uint32_t transparentPacketDemand(
            const DrawPacket& packet) noexcept;
    };

    // True when the packet's prepared world AABB (prepareTransparentWorkInterval)
    // lies outside a plane of the frustum. Invalid-bounds packets are kept.
    [[nodiscard]] inline bool transparentPacketFrustumRejected(
        const GpuSceneFrustum& frustum, const DrawPacket& packet) noexcept {
        return (packet.transparentWorkFlags & TransparentWorkIntervalValid) != 0u &&
            gpuSceneFrustumRejectsAabb(frustum, packet.boundsMinWorld,
                packet.boundsMaxWorld);
    }

    // A local-space AABB union; invalid when empty or when any member bound
    // is non-finite or inverted.
    struct TransparentLocalBounds {
        glm::vec3 minimum{ 0.0f };
        glm::vec3 maximum{ 0.0f };
        bool valid = false;
    };

    // The union of the local bounds of `submeshes` of `model`.
    [[nodiscard]] TransparentLocalBounds transparentSubmeshBounds(
        const ModelAsset& model, std::span<const uint32_t> submeshes) noexcept;

    // The union of an instance batch's per-submesh bounds (each covers every
    // instance) over `submeshes`. Invalid when a bound is missing or invalid,
    // or when a submesh is not classified WeightedOIT (the per-submesh path
    // then reports the batch error exactly as before).
    [[nodiscard]] TransparentLocalBounds instanceBatchTransparentBounds(
        const RenderInstanceBatchComponent& batch, const ModelAsset& model,
        std::span<const uint32_t> submeshes) noexcept;

    // True when the conservative world AABB of a valid local bound under
    // `world` (eight transformed corners, as prepareTransparentWorkInterval
    // computes a packet's) lies outside a plane of the frustum. Invalid
    // bounds or a degenerate transform never reject.
    [[nodiscard]] bool transparentLocalBoundsFrustumRejected(
        const GpuSceneFrustum& frustum, const TransparentLocalBounds& local,
        const glm::mat4& world) noexcept;

    // The whole-owner test of a GPU-scene owner visiting its cached
    // transparent submesh list: the list's union, or for an instance batch
    // the batch's union over the listed submeshes. Every packet the owner
    // would emit lies inside the tested bound, so a rejection removes only
    // work the per-packet test would also reject.
    [[nodiscard]] bool transparentModelFrustumRejected(
        const GpuSceneFrustum& frustum, const ModelAsset& model,
        const RenderInstanceBatchComponent* batch,
        std::span<const uint32_t> submeshes,
        const TransparentLocalBounds& listBounds,
        const glm::mat4& world) noexcept;

    // Touches every culled key of `record` into `cache` (between its
    // beginFrame and endFrame) and returns the culled work's residency
    // demand. A rejected model's packets carry the effective policy
    // emitSubmesh would give them (the editor's layered-interface override
    // unless deterministic).
    [[nodiscard]] uint32_t replayCulledTransparentWork(
        const TransparentCullRecord& record, PreviousTransformCache& cache,
        bool deterministicContent, unsigned layeredInterfaceOverride);

} // namespace Iridium
