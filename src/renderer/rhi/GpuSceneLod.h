#pragma once

#include "GpuScene.h"
#include "Mesh.h"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace Iridium {

    inline constexpr uint32_t GpuSceneLodAbiVersion = 1;

    enum class GpuSceneLodReason : uint32_t {
        ProjectedError = 0,
        HysteresisRetained = 1,
        CoarserResidentFallback = 2,
        FinerResidentFallback = 3,
        InvalidInputFailVisible = 4,
        NoResidentLevel = 5,
    };

    struct GpuSceneLodLevel {
        uint32_t geometryIndex = InvalidGpuSceneIndex;
        float geometricError = 0.0f;
        uint32_t triangleCount = 0;
        bool resident = false;
    };

    struct GpuSceneLodRequest {
        std::span<const GpuSceneLodLevel> levels;
        float distanceToBounds = 0.0f;
        float verticalFieldOfViewRadians = 0.0f;
        float viewportHeightPixels = 0.0f;
        float targetErrorPixels = 2.0f;
        float hysteresisFraction = 0.15f;
        uint32_t previousLod = InvalidGpuSceneIndex;
        // A hero-quality override sets this to zero. UINT32_MAX permits every
        // level supplied by the cooked product.
        uint32_t maximumLod = UINT32_MAX;
    };

    struct GpuSceneLodSelection {
        uint32_t abiVersion = GpuSceneLodAbiVersion;
        uint32_t selectedLod = InvalidGpuSceneIndex;
        uint32_t geometryIndex = InvalidGpuSceneIndex;
        uint32_t triangleCount = 0;
        float projectedErrorPixels = 0.0f;
        GpuSceneLodReason reason = GpuSceneLodReason::NoResidentLevel;
        bool drawable = false;
        bool changed = false;
    };

    // Backend-neutral deterministic oracle for the GPU implementation. Levels
    // are finest-to-coarsest with monotonically nondecreasing geometric error.
    [[nodiscard]] GpuSceneLodSelection selectGpuSceneLod(
        const GpuSceneLodRequest& request) noexcept;

    // Conservative pixel displacement bound for a local-space error ball around
    // any point in the base AABB. Handles shear, reflection, nonuniform scale,
    // off-axis perspective and orthographic projections. Invalid/eye-crossing
    // projections return infinity, retaining the base geometry.
    [[nodiscard]] float boundGpuSceneLodPixelError(
        const GpuSceneGeometryRecord& base, const glm::mat4& clipFromLocal,
        glm::vec2 viewportPixels, float geometricError) noexcept;

    // Experimental device oracle. The host limits the chain to a
    // resident prefix whose buffers/index type/layout are compatible with the
    // bound indirect bin. Valid previous ordinals delay coarsening only; the
    // requested error ceiling is never relaxed. Invalid history is stateless.
    [[nodiscard]] uint32_t selectGpuSceneLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const glm::mat4& clipFromLocal, glm::vec2 viewportPixels,
        float targetErrorPixels, uint32_t maximumLod,
        uint32_t previousLod = InvalidGpuSceneIndex,
        float hysteresisFraction = 0.0f) noexcept;

    // Stateless, face-invariant selector for a 90-degree cubemap consumer.
    // World-space error is conservatively derived from the packed local/world
    // bounds-sphere radius ratio, so every face of one probe makes the same
    // decision and cubemap seams cannot select different geometry.
    [[nodiscard]] uint32_t selectGpuSceneRadialLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const GpuSceneInstanceRecord& instance, glm::vec3 consumerPosition,
        float faceResolutionPixels, float targetErrorPixels,
        uint32_t maximumLod) noexcept;

    // Directional-shadow selector expressed directly in world units per shadow
    // texel. The packed sphere ratio conservatively converts object-space LOD
    // error to world space without coupling the shadow decision to a camera.
    [[nodiscard]] uint32_t selectGpuSceneDensityLodGeometry(
        std::span<const GpuSceneGeometryRecord> geometries, uint32_t baseIndex,
        const GpuSceneInstanceRecord& instance, float worldUnitsPerTexel,
        float targetErrorTexels, uint32_t maximumLod) noexcept;

    // One ordered consumer/view owns one table, NOT one table per frame context.
    // Only ordinal history is persistent; dense geometry indices are transient.
    struct GpuSceneLodHistoryRecord {
        uint32_t tokenLow = 0, tokenHigh = 0;
        uint32_t selectedLod = 0, lastFrame = 0;
    };
    struct GpuSceneLodHistoryBinding {
        uint32_t slot = InvalidGpuSceneIndex;
        uint32_t tokenLow = 0, tokenHigh = 0;
    };
    static_assert(sizeof(GpuSceneLodHistoryRecord) == 16);

    // Bounded stable-handle -> history-slot transport and independent CPU oracle
    // state. Tokens never wrap/repeat during the lifetime of this owner. GPU
    // selection is not uploaded: candidates carry only identity tokens.
    class GpuSceneLodHistory {
    public:
        void resize(uint32_t capacity);
        void publish(const GpuScenePackedTables& scene);
        void updateView(const ViewTransportRecord& view, ViewHistoryContext context);
        // Suspended/recreated views can resume at the identical extent/projection.
        void resetView() noexcept { viewValid_ = false; }
        [[nodiscard]] GpuSceneLodHistoryBinding binding(uint32_t densePrimitive) const noexcept;
        [[nodiscard]] uint32_t previous(GpuSceneLodHistoryBinding binding) const noexcept;
        void record(GpuSceneLodHistoryBinding binding, uint32_t lod) noexcept;
        [[nodiscard]] uint32_t frameSerial() const noexcept { return frameSerial_; }
        [[nodiscard]] uint32_t capacity() const noexcept { return static_cast<uint32_t>(entries_.size()); }
    private:
        struct Entry {
            GpuScenePrimitiveHandle primitive;
            GpuSceneGeometryHandle geometry;
            uint32_t layoutRevision = 0, productRevision = 0;
            uint64_t token = 0;
            GpuSceneLodHistoryRecord oracle;
        };
        [[nodiscard]] uint64_t newToken();
        void invalidate();
        std::vector<Entry> entries_;
        std::vector<uint32_t> denseSlots_;
        std::vector<uint64_t> geometryRevisions_;
        std::unordered_map<uint64_t, uint32_t> slots_;
        std::vector<uint8_t> live_;
        std::vector<uint32_t> freeSlots_;
        uint64_t nextToken_ = 0, sceneEpoch_ = 0, publicationRevision_ = 0;
        uint32_t frameSerial_ = 0;
        bool published_ = false, viewValid_ = false;
        ViewTransportRecord view_;
        ViewHistoryContext context_;
    };

} // namespace Iridium
