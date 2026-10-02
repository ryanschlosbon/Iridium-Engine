#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t DepthPyramidAbiVersion = 2;

    struct DepthPyramidExtent {
        uint32_t width = 0;
        uint32_t height = 0;

        [[nodiscard]] bool valid() const noexcept {
            return width != 0 && height != 0;
        }
        [[nodiscard]] bool operator==(const DepthPyramidExtent&) const = default;
    };

    enum class DeviceDepthConvention : uint32_t {
        ForwardZeroToOne = 0,
        ReverseZeroToOne = 1,
    };

    [[nodiscard]] uint32_t depthPyramidMipCount(
        DepthPyramidExtent extent) noexcept;
    [[nodiscard]] DepthPyramidExtent depthPyramidMipExtent(
        DepthPyramidExtent extent, uint32_t mipLevel) noexcept;

    enum class DepthPyramidHistoryRejection : uint32_t {
        None = 0,
        Disabled = 1,
        HistoryUnavailable = 2,
        InvalidExtent = 3,
        ExtentMismatch = 4,
        StaleHistory = 5,
        InvalidProjection = 6,
        CameraCut = 7,
        RapidMotion = 8,
        NearPlaneIntersection = 9,
        NewlyResident = 10,
        LargeTransformChange = 11,
        InvalidOwnership = 12,
        ViewMismatch = 13,
        SceneMismatch = 14,
        DepthContentChanged = 15,
        ProjectionChanged = 16,
        DepthConventionMismatch = 17,
    };

    // CPU-side producer identity, not a Vulkan frame-slot index. A reopened view
    // or replaced scene must get a new identity/epoch. Depth revision covers ALL
    // occluder changes (geometry, transforms, coverage, residency and visibility),
    // not only changes to the object being tested. Zero ownership is unavailable.
    struct DepthPyramidHistoryOwner {
        uint64_t viewIdentity = 0;
        uint64_t sceneEpoch = 0;
        uint64_t depthContentRevision = 0;
        // Revision of the complete world-to-clip camera transform. The legacy
        // field name is retained in ABI v2, but view pose is part of the value.
        uint64_t projectionRevision = 0;
        uint64_t resetRevision = 0;
    };

    struct DepthPyramidHistoryInput {
        DepthPyramidExtent currentExtent{};
        DepthPyramidExtent historyExtent{};
        DepthPyramidHistoryOwner currentOwner{}, historyOwner{};
        // Global render-submission serials, not per-view frame counters. A
        // throttled/skipped view cannot relabel older depth as one frame old.
        uint64_t currentFrameSerial = 0;
        uint64_t historyFrameSerial = 0;
        DeviceDepthConvention currentConvention = DeviceDepthConvention::ForwardZeroToOne;
        DeviceDepthConvention historyConvention = DeviceDepthConvention::ForwardZeroToOne;
        bool enabled = false;
        bool historyAvailable = false;
        bool projectionValid = false;
        bool cameraCut = false;
        bool rapidMotion = false;
        bool nearPlaneIntersection = false;
        bool newlyResident = false;
        bool largeTransformChange = false;
    };

    struct DepthPyramidHistoryDecision {
        uint32_t abiVersion = DepthPyramidAbiVersion;
        DepthPyramidHistoryRejection rejection =
            DepthPyramidHistoryRejection::HistoryUnavailable;
        bool eligible = false;
    };

    // Previous-frame Hi-Z is allowed only when every conservative-history
    // precondition is explicit. Any uncertainty returns a stable fail-visible reason.
    [[nodiscard]] DepthPyramidHistoryDecision evaluateDepthPyramidHistory(
        const DepthPyramidHistoryInput& input) noexcept;

    struct DepthPyramidOcclusionQuery {
        // Base-level pixel bounds, with exclusive maxima. Projection owns clipping
        // to the current extent before constructing this query.
        uint32_t minimumX = 0;
        uint32_t minimumY = 0;
        uint32_t maximumX = 0;
        uint32_t maximumY = 0;
        // Nearest device depth of the tested bounds under the configured convention.
        float nearestDepth = 0.0f;
        float depthBias = 0.0f;
    };

    struct DepthPyramidOcclusionResult {
        uint32_t abiVersion = DepthPyramidAbiVersion;
        uint32_t mipLevel = 0;
        uint32_t sampledTexels = 0;
        float farthestOccluderDepth = 0.0f;
        bool tested = false;
        bool occluded = false;
    };

    // Shader-hot query ABI used by bounded device qualification and the later
    // indirect visibility consumer. Explicit padding keeps std430 array stride
    // independent of compiler bool/layout rules.
    struct DepthPyramidDeviceQuery {
        uint32_t minimumX = 0, minimumY = 0;
        uint32_t maximumX = 0, maximumY = 0;
        float nearestDepth = 0.0f;
        float depthBias = 0.0f;
        uint32_t reserved0 = 0, reserved1 = 0;
    };

    struct DepthPyramidDeviceResult {
        uint32_t abiVersion = DepthPyramidAbiVersion;
        uint32_t mipLevel = 0;
        uint32_t sampledTexels = 0;
        uint32_t tested = 0;
        float farthestOccluderDepth = 0.0f;
        uint32_t occluded = 0;
        uint32_t reserved0 = 0, reserved1 = 0;
    };

    [[nodiscard]] constexpr DepthPyramidDeviceQuery
        packDepthPyramidDeviceQuery(
            const DepthPyramidOcclusionQuery& query) noexcept {
        return {query.minimumX, query.minimumY, query.maximumX,
            query.maximumY, query.nearestDepth, query.depthBias, 0u, 0u};
    }

    static_assert(sizeof(DepthPyramidDeviceQuery) == 32);
    static_assert(sizeof(DepthPyramidDeviceResult) == 32);

    struct DepthPyramidHistoryPublication {
        DepthPyramidHistoryOwner owner{};
        DepthPyramidExtent extent{};
        DeviceDepthConvention convention =
            DeviceDepthConvention::ForwardZeroToOne;
        uint64_t submissionSerial = 0;
        uint64_t imageGeneration = 0;
        bool available = false;
    };

    // Fixed-capacity completion tracker shared by backend implementations. An
    // older frame completion cannot replace a newer scheduled image for the
    // same view, and reset invalidates every publication after reallocation.
    class DepthPyramidHistoryPublicationTracker final {
    public:
        static constexpr uint32_t MaximumViews = 8;
        static constexpr uint32_t MaximumFrameSlots = 4;

        void reset(uint32_t viewCount, uint32_t frameSlotCount,
            uint64_t imageGeneration);
        void clear() noexcept;
        void schedule(uint32_t frameSlot, uint32_t view,
            const DepthPyramidHistoryOwner& owner,
            DepthPyramidExtent extent, DeviceDepthConvention convention,
            uint64_t submissionSerial);
        void complete(uint32_t frameSlot,
            uint64_t completedSubmissionSerial) noexcept;
        // Latest metadata scheduled for the view. It is not CPU-complete, but
        // may be consumed by a later submission on the same ordered queue when
        // its serial is exactly adjacent. CPU readback must use published().
        [[nodiscard]] const DepthPyramidHistoryPublication& queued(
            uint32_t view) const;
        [[nodiscard]] const DepthPyramidHistoryPublication& published(
            uint32_t view) const;

    private:
        struct Pending {
            DepthPyramidHistoryPublication publication{};
            uint32_t view = 0;
            bool valid = false;
        };
        std::array<DepthPyramidHistoryPublication, MaximumViews> published_{};
        std::array<DepthPyramidHistoryPublication, MaximumViews> queued_{};
        std::array<Pending, MaximumFrameSlots> pending_{};
        std::array<uint64_t, MaximumViews> latestScheduledSerial_{};
        uint32_t viewCount_ = 0;
        uint32_t frameSlotCount_ = 0;
        uint64_t imageGeneration_ = 0;
    };

    enum class DepthPyramidProjectionRejection : uint32_t {
        None, InvalidExtent, InvalidBounds, InvalidProjection, InvalidSettings,
        ClipPlaneIntersection, OutsideView, SmallBounds,
    };

    struct DepthPyramidProjectionInput {
        DepthPyramidExtent extent{};
        DeviceDepthConvention convention = DeviceDepthConvention::ForwardZeroToOne;
        // Explicit row-major world-to-history-clip matrix; includes the actual
        // history projection's Y orientation and jitter, not the current camera.
        std::array<float, 16> worldToClip{};
        std::array<float, 3> minimumWorld{}, maximumWorld{};
        float guardPixels = 1.0f;
        float minimumFootprintPixels = 1.0f;
        float depthBias = 0.00001f;
    };

    struct DepthPyramidProjectionResult {
        DepthPyramidOcclusionQuery query{};
        DepthPyramidProjectionRejection rejection = DepthPyramidProjectionRejection::InvalidProjection;
        bool eligible = false;
    };

    // Conservative query construction only, never a visibility verdict. The
    // caller must separately prove history eligibility before consuming it.
    [[nodiscard]] DepthPyramidProjectionResult projectDepthPyramidBounds(
        const DepthPyramidProjectionInput& input) noexcept;

    // Deterministic CPU oracle for the Vulkan pyramid and sampling path. Each mip
    // stores the farthest depth over its source footprint (max for forward Z, min
    // for reverse Z), which is required for conservative whole-bounds rejection.
    // Odd extents use proportional source intervals so edge texels are never lost.
    // Queries invert those integer intervals recursively at each mip; normalized
    // base-to-mip scaling is not equivalent for odd dimensions.
    class DepthPyramidReference final {
    public:
        void build(DepthPyramidExtent extent, DeviceDepthConvention convention,
            std::span<const float> baseDeviceDepth);
        void clear() noexcept;

        [[nodiscard]] DepthPyramidExtent extent() const noexcept { return extent_; }
        [[nodiscard]] DeviceDepthConvention convention() const noexcept {
            return convention_;
        }
        [[nodiscard]] uint32_t mipCount() const noexcept {
            return static_cast<uint32_t>(mipOffsets_.size());
        }
        [[nodiscard]] std::span<const float> mip(uint32_t mipLevel) const noexcept;
        [[nodiscard]] DepthPyramidOcclusionResult test(
            const DepthPyramidOcclusionQuery& query) const noexcept;

    private:
        DepthPyramidExtent extent_{};
        DeviceDepthConvention convention_ =
            DeviceDepthConvention::ForwardZeroToOne;
        std::vector<uint32_t> mipOffsets_;
        std::vector<float> depth_;
    };

} // namespace Iridium
