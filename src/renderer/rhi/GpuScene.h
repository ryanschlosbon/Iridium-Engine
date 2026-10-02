#pragma once

#include "RenderHandles.h"
#include "assets/AssetGuid.h"
#include "scene/SceneEntityUuid.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t GpuSceneAbiVersion = 2;
    inline constexpr uint32_t MaximumGpuSceneLodLevels = 16;
    inline constexpr uint32_t InvalidGpuSceneIndex = UINT32_MAX;

    template<typename Tag>
    struct GpuSceneHandle {
        uint32_t slot = InvalidGpuSceneIndex;
        uint32_t generation = 0;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return slot != InvalidGpuSceneIndex && generation != 0;
        }
        auto operator<=>(const GpuSceneHandle&) const = default;
    };

    struct GpuSceneInstanceTag {};
    struct GpuScenePrimitiveTag {};
    struct GpuSceneGeometryTag {};
    using GpuSceneInstanceHandle = GpuSceneHandle<GpuSceneInstanceTag>;
    using GpuScenePrimitiveHandle = GpuSceneHandle<GpuScenePrimitiveTag>;
    using GpuSceneGeometryHandle = GpuSceneHandle<GpuSceneGeometryTag>;

    enum class GpuSceneMobility : uint32_t { Static = 0, Movable = 1, Animated = 2 };

    enum GpuSceneInstanceFlag : uint32_t {
        GpuSceneInstanceEnabled = 1u << 0u,
        GpuSceneInstanceSelected = 1u << 1u,
        GpuSceneInstanceHistoryReset = 1u << 2u,
        GpuSceneInstanceInvalidBoundsFailVisible = 1u << 3u,
    };

    inline constexpr uint32_t GpuSceneInstanceMaximumLodShift = 8u;
    inline constexpr uint32_t GpuSceneInstanceMaximumLodMask =
        (MaximumGpuSceneLodLevels - 1u) << GpuSceneInstanceMaximumLodShift;

    [[nodiscard]] constexpr uint32_t packGpuSceneInstanceFlags(
        uint32_t flags, uint32_t maximumLod) noexcept {
        return (flags & ~GpuSceneInstanceMaximumLodMask) |
            (((std::min)(maximumLod, MaximumGpuSceneLodLevels - 1u) <<
                GpuSceneInstanceMaximumLodShift) &
                GpuSceneInstanceMaximumLodMask);
    }

    [[nodiscard]] constexpr uint32_t gpuSceneInstanceMaximumLod(
        uint32_t packedFlags) noexcept {
        return (packedFlags & GpuSceneInstanceMaximumLodMask) >>
            GpuSceneInstanceMaximumLodShift;
    }

    enum GpuSceneConsumerMask : uint32_t {
        GpuSceneConsumerMainOpaque = 1u << 0u,
        GpuSceneConsumerForwardOpaque = 1u << 1u,
        GpuSceneConsumerShadow = 1u << 2u,
        GpuSceneConsumerProbe = 1u << 3u,
        GpuSceneConsumerSelection = 1u << 4u,
        GpuSceneConsumerRayTracing = 1u << 5u,
    };

    enum GpuScenePrimitiveFlag : uint32_t {
        GpuScenePrimitiveOpaque = 1u << 0u,
        GpuScenePrimitiveAlphaMask = 1u << 1u,
        GpuScenePrimitiveTransparent = 1u << 2u,
        GpuScenePrimitiveTwoSided = 1u << 3u,
    };

    enum GpuSceneGeometryFlag : uint32_t {
        // M7.2 compatibility publication stores the current RHI GeometryHandle in
        // both logical arena fields. M7.3 replaces this with stable arena ranges.
        GpuSceneGeometryLegacyRhiHandle = 1u << 0u,
    };

    struct alignas(16) GpuSceneFloat4 {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    };

    struct alignas(16) GpuSceneUint4 {
        uint32_t x = 0, y = 0, z = 0, w = 0;
    };

    // Row-major affine convention: p'.x = dot(row0.xyz, p) + row0.w.
    struct alignas(16) GpuSceneAffineTransform {
        GpuSceneFloat4 row0, row1, row2;
    };

    struct alignas(16) GpuSceneInstanceRecord {
        GpuSceneFloat4 worldBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        GpuSceneFloat4 worldBoundsMin;
        GpuSceneFloat4 worldBoundsMax;
        GpuSceneUint4 references{ InvalidGpuSceneIndex, InvalidGpuSceneIndex, 0u, 0u };
        GpuSceneUint4 state; // generation, mobility, flags, consumer mask
    };

    // Per-instance binding; immutable geometry is shared separately.
    struct alignas(16) GpuScenePrimitiveRecord {
        GpuSceneUint4 binding{ InvalidGpuSceneIndex, InvalidGpuSceneIndex, 0u, 0u };
        GpuSceneUint4 state{ 0u, 0u, InvalidGpuSceneIndex, InvalidGpuSceneIndex };
        GpuSceneUint4 revisions; // layout, geometry, binding, material
    };

    struct alignas(16) GpuSceneGeometryRecord {
        GpuSceneFloat4 localBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        GpuSceneFloat4 localBoundsMin; // xyz bounds; w object-space LOD error
        GpuSceneFloat4 localBoundsMax; // xyz bounds; w finest-to-coarsest ordinal
        GpuSceneUint4 draw; // first index, count, signed vertex offset bits, index type
        GpuSceneUint4 storage{ InvalidGpuSceneIndex, InvalidGpuSceneIndex, 0u, 0u };
        // generation, cold identity, next coarser dense geometry (or invalid), revision
        GpuSceneUint4 state{ 0u, InvalidGpuSceneIndex, InvalidGpuSceneIndex, 0u };
    };

    static_assert(sizeof(GpuSceneInstanceHandle) == 8);
    static_assert(sizeof(GpuScenePrimitiveHandle) == 8);
    static_assert(sizeof(GpuSceneGeometryHandle) == 8);
    static_assert(sizeof(GpuSceneFloat4) == 16);
    static_assert(sizeof(GpuSceneUint4) == 16);
    static_assert(sizeof(GpuSceneAffineTransform) == 48);
    static_assert(sizeof(GpuSceneInstanceRecord) == 80);
    static_assert(sizeof(GpuScenePrimitiveRecord) == 48);
    static_assert(sizeof(GpuSceneGeometryRecord) == 96);
    static_assert(std::is_standard_layout_v<GpuSceneAffineTransform>);
    static_assert(std::is_standard_layout_v<GpuSceneInstanceRecord>);
    static_assert(std::is_standard_layout_v<GpuScenePrimitiveRecord>);
    static_assert(std::is_standard_layout_v<GpuSceneGeometryRecord>);
    static_assert(std::is_trivially_copyable_v<GpuSceneAffineTransform>);
    static_assert(std::is_trivially_copyable_v<GpuSceneInstanceRecord>);
    static_assert(std::is_trivially_copyable_v<GpuScenePrimitiveRecord>);
    static_assert(std::is_trivially_copyable_v<GpuSceneGeometryRecord>);

    struct GpuSceneInstanceIdentity {
        SceneEntityUuid owner;
        AssetGuid modelAssetGuid;
        uint64_t publishedRevision = 0;
        std::string artifactCookKey;
        auto operator<=>(const GpuSceneInstanceIdentity&) const = default;
    };

    struct GpuScenePrimitiveIdentity {
        SceneEntityUuid owner;
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        AssetGuid effectiveMaterialGuid;
        auto operator<=>(const GpuScenePrimitiveIdentity&) const = default;
    };

    struct GpuSceneGeometryIdentity {
        AssetGuid modelAssetGuid;
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        auto operator<=>(const GpuSceneGeometryIdentity&) const = default;
    };

    struct GpuSceneGeometrySource {
        GpuSceneGeometryHandle handle;
        glm::vec4 localBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        glm::vec4 localBoundsMin{ 0.0f };
        glm::vec4 localBoundsMax{ 0.0f };
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t indexType = 0;
        uint32_t vertexArena = InvalidGpuSceneIndex;
        uint32_t indexArena = InvalidGpuSceneIndex;
        uint32_t vertexLayout = 0, flags = 0, productRevision = 0;
        uint64_t recordRevision = 0;
        GpuSceneGeometryIdentity identity;
        GpuSceneGeometryHandle coarserLod;
        float geometricError = 0.0f;
        uint32_t lodLevel = 0;
    };

    struct GpuScenePrimitiveSource {
        GpuScenePrimitiveHandle handle;
        GpuSceneGeometryHandle geometry;
        MaterialHandle material;
        PipelineHandle pipeline;
        uint32_t flags = GpuScenePrimitiveOpaque;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque;
        uint32_t primitiveLayoutRevision = 0;
        uint32_t geometryProductRevision = 0;
        uint32_t instanceBindingRevision = 0;
        uint32_t materialRevision = 0;
        uint64_t recordRevision = 0;
        GpuScenePrimitiveIdentity identity;
    };

    struct GpuSceneInstanceSource {
        GpuSceneInstanceHandle handle;
        GpuSceneMobility mobility = GpuSceneMobility::Movable;
        uint32_t flags = GpuSceneInstanceEnabled;
        uint32_t maximumLod = MaximumGpuSceneLodLevels - 1u;
        uint32_t consumerMask = GpuSceneConsumerMainOpaque |
            GpuSceneConsumerShadow | GpuSceneConsumerProbe |
            GpuSceneConsumerSelection;
        uint64_t instanceRevision = 0;
        uint64_t currentTransformRevision = 0;
        uint64_t previousTransformRevision = 0;
        glm::mat4 currentWorld{ 1.0f };
        glm::mat4 previousWorld{ 1.0f };
        glm::vec4 worldBoundsSphere{ 0.0f, 0.0f, 0.0f, -1.0f };
        glm::vec4 worldBoundsMin{ 0.0f };
        glm::vec4 worldBoundsMax{ 0.0f };
        GpuSceneInstanceIdentity identity;
        std::vector<GpuScenePrimitiveSource> primitives;
    };

    struct GpuSceneCapacity {
        uint32_t maximumInstances = 1u << 20u;
        uint32_t maximumPrimitives = 1u << 20u;
        uint32_t maximumGeometries = 1u << 20u;
        uint32_t maximumTransforms = 1u << 21u;
    };

    struct GpuSceneCapacityRequirements {
        uint32_t transforms = 0;
        uint32_t instances = 0;
        uint32_t primitives = 0;
        uint32_t geometries = 0;
    };

    struct GpuSceneFrameSerials {
        uint64_t lastSubmitted = 0;
        uint64_t completed = 0;
    };

    struct GpuSceneUploadTelemetry {
        uint64_t bytes = 0;
        uint32_t ranges = 0;
        uint32_t transformRanges = 0;
        uint32_t instanceRanges = 0;
        uint32_t primitiveRanges = 0;
        uint32_t geometryRanges = 0;
    };

    struct GpuScenePublicationInput {
        uint64_t sceneEpoch = 0;
        uint64_t publicationRevision = 0;
        std::span<const GpuSceneGeometrySource> geometries;
        std::span<const GpuSceneInstanceSource> instances;
    };

    struct GpuScenePackedTables {
        uint32_t abiVersion = GpuSceneAbiVersion;
        uint64_t sceneEpoch = 0, publicationRevision = 0;
        std::vector<GpuSceneAffineTransform> transforms;
        std::vector<GpuSceneInstanceRecord> instances;
        std::vector<GpuScenePrimitiveRecord> primitives;
        std::vector<GpuSceneGeometryRecord> geometries;
        std::vector<uint64_t> transformRevisions;
        std::vector<uint64_t> instanceRevisions;
        std::vector<uint64_t> primitiveRevisions;
        std::vector<uint64_t> geometryRevisions;
        std::vector<GpuSceneInstanceIdentity> instanceIdentities;
        std::vector<GpuScenePrimitiveIdentity> primitiveIdentities;
        std::vector<GpuSceneGeometryIdentity> geometryIdentities;
        std::vector<GpuSceneInstanceHandle> denseInstanceHandles;
        std::vector<GpuScenePrimitiveHandle> densePrimitiveHandles;
        std::vector<GpuSceneGeometryHandle> denseGeometryHandles;
        std::vector<GpuSceneInstanceHandle> directFallbackInstances;
        // Camera-independent consumer membership is published once with the
        // packed scene. Consumers may cache derived work while the matching
        // revision remains unchanged; transforms and bounds intentionally do
        // not participate in these revisions.
        std::vector<uint32_t> shadowConsumerPrimitiveIndices;
        std::vector<uint32_t> probeConsumerPrimitiveIndices;
        uint64_t shadowConsumerMembershipRevision = 0;
        uint64_t probeConsumerMembershipRevision = 0;
        uint32_t invalidSourceCount = 0;
        uint32_t capacityOmittedInstanceCount = 0;
    };

    [[nodiscard]] GpuSceneAffineTransform packGpuSceneAffine(const glm::mat4& transform);
    [[nodiscard]] glm::mat4 unpackGpuSceneAffine(
        const GpuSceneAffineTransform& transform) noexcept;
    [[nodiscard]] glm::vec3 transformGpuScenePoint(
        const GpuSceneAffineTransform& transform, glm::vec3 point) noexcept;
    void collectGpuSceneConsumerPrimitiveIndices(
        const GpuScenePackedTables& scene, uint32_t consumerMask,
        std::vector<uint32_t>& destination);
    void publishGpuSceneConsumerMembership(GpuScenePackedTables& scene);
    [[nodiscard]] GpuScenePackedTables packGpuSceneReference(
        const GpuScenePublicationInput& input, const GpuSceneCapacity& capacity);

    // Retired slots remain unavailable until the caller's completion serial is
    // reached. Generation exhaustion quarantines a slot rather than wrapping.
    template<typename Handle>
    class GpuSceneSlotAllocator {
    public:
        explicit GpuSceneSlotAllocator(uint32_t capacity) {
            if (capacity == 0) throw std::out_of_range(
                "GPU-scene slot capacity must be nonzero");
            slots_.resize(capacity);
        }
        [[nodiscard]] std::optional<Handle> allocate() {
            for (uint32_t offset = 0; offset < slots_.size(); ++offset) {
                const uint32_t index = static_cast<uint32_t>(
                    (static_cast<uint64_t>(allocationCursor_) + offset) %
                    slots_.size());
                Slot& slot = slots_[index];
                if (slot.state != SlotState::Free) continue;
                slot.state = SlotState::Live;
                allocationCursor_ = index + 1u == slots_.size()
                    ? 0u : index + 1u;
                ++liveCount_;
                return Handle{ index, slot.generation };
            }
            return std::nullopt;
        }
        [[nodiscard]] bool retire(Handle handle, uint64_t serial) noexcept {
            if (!isLive(handle)) return false;
            Slot& slot = slots_[handle.slot];
            slot.state = SlotState::Retired;
            slot.retireAfterSerial = serial;
            slot.nextRetired = retiredHead_;
            retiredHead_ = handle.slot;
            --liveCount_; ++retiredCount_;
            return true;
        }
        uint32_t collect(uint64_t completedSerial) noexcept {
            if (retiredHead_ == InvalidGpuSceneIndex) return 0;
            uint32_t count = 0;
            uint32_t pendingHead = InvalidGpuSceneIndex;
            uint32_t slotIndex = retiredHead_;
            while (slotIndex != InvalidGpuSceneIndex) {
                Slot& slot = slots_[slotIndex];
                const uint32_t next = slot.nextRetired;
                if (slot.state != SlotState::Retired ||
                    slot.retireAfterSerial > completedSerial) {
                    slot.nextRetired = pendingHead;
                    pendingHead = slotIndex;
                    slotIndex = next;
                    continue;
                }
                --retiredCount_; ++count; slot.retireAfterSerial = 0;
                slot.nextRetired = InvalidGpuSceneIndex;
                if (slot.generation == UINT32_MAX) {
                    slot.state = SlotState::Exhausted;
                    ++exhaustedCount_;
                }
                else { ++slot.generation; slot.state = SlotState::Free; }
                slotIndex = next;
            }
            retiredHead_ = pendingHead;
            return count;
        }
        [[nodiscard]] bool isLive(Handle handle) const noexcept {
            return handle.isValid() && handle.slot < slots_.size() &&
                slots_[handle.slot].state == SlotState::Live &&
                slots_[handle.slot].generation == handle.generation;
        }
        [[nodiscard]] uint32_t capacity() const noexcept {
            return static_cast<uint32_t>(slots_.size());
        }
        [[nodiscard]] uint32_t liveCount() const noexcept { return liveCount_; }
        [[nodiscard]] uint32_t retiredCount() const noexcept { return retiredCount_; }
        [[nodiscard]] uint32_t availableCount() const noexcept {
            return capacity() - liveCount_ - retiredCount_ - exhaustedCount_;
        }
    private:
        enum class SlotState : uint8_t { Free, Live, Retired, Exhausted };
        struct Slot {
            uint32_t generation = 1;
            SlotState state = SlotState::Free;
            uint64_t retireAfterSerial = 0;
            uint32_t nextRetired = InvalidGpuSceneIndex;
        };
        std::vector<Slot> slots_;
        uint32_t retiredHead_ = InvalidGpuSceneIndex;
        uint32_t allocationCursor_ = 0;
        uint32_t liveCount_ = 0, retiredCount_ = 0, exhaustedCount_ = 0;
    };

} // namespace Iridium
