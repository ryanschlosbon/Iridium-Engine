#pragma once

#include "DrawPacket.h"
#include "GpuScene.h"

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t GpuSceneIndirectAbiVersion = 3;

    // Backend-neutral mirror of the indexed-indirect command ABI. Vulkan and
    // other explicit backends may upload this array without repacking it.
    struct GpuSceneIndexedIndirectCommand {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t firstInstance = 0;
    };

    struct GpuSceneIndirectCandidate {
        uint32_t primitiveIndex = 0;
        uint32_t binIndex = 0;
        uint32_t commandBase = 0;
        uint32_t commandCapacity = 0;
        uint32_t maximumLod = 0; // Host-validated resident/buffer-compatible prefix.
        uint32_t historySlot = InvalidGpuSceneIndex;
        uint32_t historyTokenLow = 0, historyTokenHigh = 0;
    };

    inline constexpr uint32_t GpuSceneShadowIndirectAbiVersion = 1;

    // One immutable candidate table is reused for every directional cascade.
    // Dispatch push constants relocate its per-bin command and count ranges.
    struct GpuSceneShadowIndirectCandidate {
        uint32_t primitiveIndex = 0;
        uint32_t binIndex = 0;
        uint32_t commandBase = 0;
        uint32_t commandCapacity = 0;
    };

    struct GpuSceneIndirectPolicy {
        bool multiDrawIndirect = false;
        bool drawIndirectFirstInstance = false;
        bool drawIndirectCount = false;
        uint32_t maxDrawIndirectCount = 0;
        uint32_t minimumCommandCount = 8;
        bool forceDirectReference = false;
    };

    enum class GpuSceneIndirectFallbackReason : uint32_t {
        None = 0,
        MissingCapability,
        TinyWorkload,
        CapacityExceeded,
        InvalidPacket,
        UnsupportedPass,
        DirectReference,
    };

    struct GpuSceneIndirectPlan {
        uint32_t abiVersion = GpuSceneIndirectAbiVersion;
        GpuSceneIndirectFallbackReason fallbackReason =
            GpuSceneIndirectFallbackReason::None;
        std::vector<GpuSceneIndexedIndirectCommand> commands;
        std::vector<uint32_t> packetIndices;

        [[nodiscard]] bool usesIndirect() const noexcept {
            return fallbackReason == GpuSceneIndirectFallbackReason::None &&
                !commands.empty();
        }
    };

    // With packed tables, commands address a vertex arena bound at byte zero.
    // The table-free compatibility oracle assumes a per-primitive vertex binding.
    // Mixed or malformed work fails the entire plan to direct submission.
    void buildGpuSceneIndirectPlan(std::span<const DrawPacket> packets,
        const GpuSceneIndirectPolicy& policy, GpuSceneIndirectPlan& result,
        std::span<const GpuScenePrimitiveRecord> primitives = {},
        std::span<const GpuSceneGeometryRecord> geometries = {});

    // M7R R5c.4b: the same plan for published primitives in draw order (the
    // main-opaque submission), read from their records: index count, first
    // index and signed vertex offset from the LOD0 geometry record, first
    // instance = the dense primitive index. `directPacketCount` direct
    // packets drawn in the same pass count toward the workload thresholds and
    // make the plan fall back as InvalidPacket, as a mixed packet queue did.
    void buildGpuSceneIndirectPlan(std::span<const uint32_t> primitiveIndices,
        size_t directPacketCount, const GpuSceneIndirectPolicy& policy,
        GpuSceneIndirectPlan& result,
        std::span<const GpuScenePrimitiveRecord> primitives,
        std::span<const GpuSceneGeometryRecord> geometries);

    static_assert(sizeof(GpuSceneIndexedIndirectCommand) == 20);
    static_assert(sizeof(GpuSceneIndirectCandidate) == 32);
    static_assert(sizeof(GpuSceneShadowIndirectCandidate) == 16);
    static_assert(std::is_trivially_copyable_v<
        GpuSceneIndexedIndirectCommand>);
    static_assert(std::is_trivially_copyable_v<GpuSceneIndirectCandidate>);
    static_assert(std::is_trivially_copyable_v<
        GpuSceneShadowIndirectCandidate>);

} // namespace Iridium
