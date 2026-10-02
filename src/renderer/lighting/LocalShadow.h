#pragma once

#include "renderer/rhi/LightingTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace Iridium {

    enum class LocalShadowKind : uint8_t { Spot, Point };

    struct LocalShadowRequest {
        SceneEntityUuid owner;
        LocalShadowKind kind = LocalShadowKind::Spot;
        uint32_t lightSlot = 0;
        uint32_t quality = 0;
        int32_t priority = 0;
        float conservativeContribution = 0.0f;
    };

    [[nodiscard]] uint32_t localShadowResolution(
        LocalShadowKind kind, uint32_t quality);
    [[nodiscard]] bool localShadowRequestPrecedes(
        const LocalShadowRequest& left,
        const LocalShadowRequest& right) noexcept;
    [[nodiscard]] std::vector<LocalShadowRequest> buildLocalShadowRequests(
        const LightingFramePacket& lighting, glm::vec3 cameraPosition);

    struct LocalShadowAllocationStats {
        uint32_t requested = 0;
        uint32_t allocated = 0;
        uint32_t reused = 0;
        uint32_t relocated = 0;
        uint32_t evicted = 0;
        uint32_t omitted = 0;
    };

    struct SpotShadowTile {
        SceneEntityUuid owner;
        uint32_t lightSlot = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t size = 0;
        uint32_t guardTexels = 4;

        friend bool operator==(const SpotShadowTile&,
            const SpotShadowTile&) = default;
    };

    struct SpotShadowAtlasConfig {
        uint32_t atlasResolution = 4096;
        uint32_t minimumTileResolution = 512;
        uint32_t guardTexels = 4;
    };

    class StableSpotShadowAtlas final {
    public:
        explicit StableSpotShadowAtlas(SpotShadowAtlasConfig config = {});
        [[nodiscard]] LocalShadowAllocationStats reconcile(
            std::span<const LocalShadowRequest> requests);
        [[nodiscard]] std::span<const SpotShadowTile> allocations() const noexcept {
            return allocations_;
        }
        void reset() noexcept { allocations_.clear(); }

    private:
        SpotShadowAtlasConfig config_;
        std::vector<SpotShadowTile> allocations_;
    };

    struct PointShadowSlot {
        SceneEntityUuid owner;
        uint32_t lightSlot = 0;
        uint32_t resolution = 0;
        uint32_t cubeIndex = 0;

        friend bool operator==(const PointShadowSlot&,
            const PointShadowSlot&) = default;
    };

    struct PointShadowPoolConfig {
        // Capacities for the 256, 512, and 1024 cube pools.
        std::array<uint32_t, 3> cubeCapacity{ 32, 16, 8 };
    };

    class StablePointShadowPools final {
    public:
        explicit StablePointShadowPools(PointShadowPoolConfig config = {});
        [[nodiscard]] LocalShadowAllocationStats reconcile(
            std::span<const LocalShadowRequest> requests);
        [[nodiscard]] std::span<const PointShadowSlot> allocations() const noexcept {
            return allocations_;
        }
        void reset() noexcept { allocations_.clear(); }

    private:
        PointShadowPoolConfig config_;
        std::vector<PointShadowSlot> allocations_;
    };

    struct PointShadowFace {
        glm::vec3 direction{};
        glm::vec3 up{};
        glm::mat4 worldToShadowClip{ 1.0f };
    };

    [[nodiscard]] std::array<PointShadowFace, 6> buildPointShadowFaces(
        glm::vec3 lightPosition, float nearPlane, float farPlane);

    struct SpotShadowProjection {
        glm::vec3 lightForward{};
        glm::vec3 lightRight{};
        float outerConeRadians = 0.0f;
        glm::mat4 worldToShadowClip{ 1.0f };
    };

    [[nodiscard]] SpotShadowProjection buildSpotShadowProjection(
        glm::vec3 lightPosition, glm::vec3 emissionDirection,
        float outerConeCosine, float nearPlane, float farPlane);

    struct LocalShadowCacheInput {
        LocalShadowRequest request;
        uint32_t resolution = 0;
        uint64_t allocationRevision = 0;
        uint64_t lightRevision = 0;
        uint64_t casterRevision = 0;
        uint64_t projectionRevision = 0;
        uint64_t pipelineRevision = 1;
    };

    enum class LocalShadowDirtyReason : uint8_t {
        None,
        NewAllocation,
        AllocationChanged,
        LightChanged,
        ProjectionChanged,
        PipelineChanged,
        CasterChanged,
    };

    struct LocalShadowScheduleEntry {
        SceneEntityUuid owner;
        LocalShadowKind kind = LocalShadowKind::Spot;
        LocalShadowDirtyReason dirtyReason = LocalShadowDirtyReason::None;
        uint64_t renderTexels = 0;
        uint32_t faceCount = 0;
        uint32_t staleAgeFrames = 0;
        bool update = false;
        bool sampleable = false;
        bool stale = false;
    };

    struct LocalShadowScheduleStats {
        uint32_t requests = 0;
        uint32_t cacheHits = 0;
        uint32_t dirty = 0;
        uint32_t updates = 0;
        uint32_t staleSampled = 0;
        uint32_t unshadowed = 0;
        uint64_t renderedTexels = 0;
    };

    struct LocalShadowSchedule {
        std::vector<LocalShadowScheduleEntry> entries;
        LocalShadowScheduleStats stats;
    };

    struct LocalShadowScheduleConfig {
        uint64_t maximumRenderedTexels = 4ull * 1024ull * 1024ull;
        uint32_t maximumCompatibleStaleFrames = 2;
    };

    class LocalShadowCacheScheduler final {
    public:
        explicit LocalShadowCacheScheduler(
            LocalShadowScheduleConfig config = {});
        void configure(LocalShadowScheduleConfig config);
        [[nodiscard]] const LocalShadowSchedule& schedule(
            std::span<const LocalShadowCacheInput> inputs);
        void markScheduledRendered();
        void reset() noexcept;

    private:
        struct State {
            LocalShadowCacheInput published;
            uint32_t staleAgeFrames = 0;
            bool valid = false;
        };
        struct PendingUpdate {
            SceneEntityUuid owner;
            LocalShadowCacheInput input;
        };
        LocalShadowScheduleConfig config_;
        std::vector<State> states_;
        std::vector<SceneEntityUuid> stateOwners_;
        std::vector<PendingUpdate> pendingUpdates_;
        std::optional<LocalShadowSchedule> currentSchedule_;
    };

} // namespace Iridium
