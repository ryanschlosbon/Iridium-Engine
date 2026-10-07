#pragma once

#include "renderer/rhi/VisibilityContracts.h"
#include "core/types/SceneEntityUuid.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <compare>
#include <cstddef>
#include <array>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Iridium {

    constexpr uint32_t VirtualShadowMapAbiVersion = 2;
    constexpr uint32_t InvalidVirtualShadowPhysicalPage = ~0u;

    // Initial Vulkan storage qualification contract. The flat page table and
    // frame-owned compute working sets are resource/lifetime checkpoints; the
    // sampled page-table representation remains subject to runtime-path
    // qualification before lighting consumes it.
    struct VirtualShadowResourceConfig {
        uint32_t pageSizeTexels = 128;
        uint32_t borderTexels = 4;
        uint32_t physicalPageCapacity = 1'024;
        uint32_t pageTableEntryCapacity = 65'536;
        uint32_t receiverMarkCapacity = 65'536;
        uint32_t compactedRequestCapacity = 4'096;
    };

    struct VirtualShadowGpuBufferRange {
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    struct VirtualShadowGpuWorkingSetLayout {
        VirtualShadowGpuBufferRange clipLevels;
        VirtualShadowGpuBufferRange receivers;
        VirtualShadowGpuBufferRange rawMarks;
        VirtualShadowGpuBufferRange alternateRequests;
        VirtualShadowGpuBufferRange denseRequests;
        VirtualShadowGpuBufferRange outputRequests;
        VirtualShadowGpuBufferRange telemetry;
        uint64_t totalBytes = 0;
        uint32_t scratchCapacity = 0;
    };

    enum class VirtualShadowProjection : uint8_t {
        DirectionalClip = 0,
        Spot = 1,
        PointFace = 2,
    };

    enum VirtualShadowPageLayer : uint8_t {
        VirtualShadowPageLayerNone = 0,
        VirtualShadowPageLayerStatic = 1u << 0u,
        VirtualShadowPageLayerDynamic = 1u << 1u,
        VirtualShadowPageLayerAll = VirtualShadowPageLayerStatic |
            VirtualShadowPageLayerDynamic,
    };

    enum class VirtualShadowPageState : uint8_t {
        Missing = 0,
        CachedSampleable,
        PendingRaster,
    };

    enum class VirtualShadowMissingPageAction : uint8_t {
        FullyLitFailVisible = 0,
        ConventionalShadowFallback,
    };

    // Stable virtual identity. Directional clip levels use levelOrFace for the
    // clip level; point lights use it for the cube face; spot lights keep zero.
    struct VirtualShadowPageAddress {
        SceneEntityUuid lightOwner;
        uint64_t projectionRevision = 0;
        // Signed world-page coordinates remain stable when a directional
        // clipmap scrolls its camera-relative window.
        int64_t pageX = 0;
        int64_t pageY = 0;
        uint16_t mip = 0;
        uint8_t levelOrFace = 0;
        VirtualShadowProjection projection =
            VirtualShadowProjection::DirectionalClip;

        auto operator<=>(const VirtualShadowPageAddress&) const = default;
    };

    struct VirtualShadowPageRequest {
        VirtualShadowPageAddress address;
        uint64_t staticCasterRevision = 0;
        uint64_t dynamicCasterRevision = 0;
        uint32_t receiverSamples = 0;
        int32_t priority = 0;
        uint8_t requiredLayers = VirtualShadowPageLayerAll;
    };

    struct DirectionalVirtualShadowClipLevel {
        SceneEntityUuid lightOwner;
        uint64_t projectionRevision = 0;
        uint64_t staticCasterRevision = 0;
        uint64_t dynamicCasterRevision = 0;
        glm::mat4 worldToShadowClip{ 1.0f };
        uint32_t virtualResolutionTexels = 16'384;
        int64_t worldPageOriginX = 0;
        int64_t worldPageOriginY = 0;
        int32_t priority = 0;
        uint16_t mip = 0;
        uint8_t level = 0;
        uint8_t requiredLayers = VirtualShadowPageLayerAll;
    };

    struct DirectionalVirtualShadowReceiverSample {
        glm::vec3 worldPosition{ 0.0f };
        uint32_t receiverSamples = 1;
    };

    // Camera motion scrolls XY windows in whole world pages. Depth bounds and
    // light basis are projection policy, not camera-following values: changing
    // either requires a new projectionRevision before cached pages are reused.
    struct DirectionalVirtualShadowClipConfig {
        SceneEntityUuid lightOwner;
        uint64_t projectionRevision = 0;
        uint64_t staticCasterRevision = 0;
        uint64_t dynamicCasterRevision = 0;
        glm::vec3 focusWorld{0.0f};
        glm::vec3 lightForward{0.0f, -1.0f, 0.0f};
        double finestWorldSpan = 32.0;
        double lightDepthMinimum = -1'024.0;
        double lightDepthMaximum = 1'024.0;
        uint32_t levelCount = 4;
        uint32_t virtualResolutionTexels = 16'384;
        uint32_t pageSizeTexels = 128;
        uint8_t requiredLayers = VirtualShadowPageLayerAll;
    };

    struct DirectionalVirtualShadowClipPlan {
        std::array<DirectionalVirtualShadowClipLevel, 16> clips{};
        std::array<double, 16> worldUnitsPerTexel{};
        std::array<double, 16> worldUnitsPerPage{};
        uint32_t levelCount = 0;
        [[nodiscard]] std::span<const DirectionalVirtualShadowClipLevel> levels() const {
            return {clips.data(), levelCount};
        }
    };

    [[nodiscard]] DirectionalVirtualShadowClipPlan buildDirectionalVirtualShadowClips(
        const DirectionalVirtualShadowClipConfig& config);

    struct VirtualShadowCasterBounds {
        glm::vec3 centerWorld{};
        float radiusWorld = -1.0f;
    };

    // Monotonic per-publisher revisions; camera XY scroll is not invalidation.
    // Unknown bounds suppress publication, preserving conventional fallback.
    class DirectionalVirtualShadowClipPublisher final {
    public:
        [[nodiscard]] std::optional<DirectionalVirtualShadowClipPlan> publish(
            DirectionalVirtualShadowClipConfig config,
            std::span<const VirtualShadowCasterBounds> casters,
            double depthQuantum = 256.0, double padding = 16.0);
        void reset() noexcept { initialized_ = false; }
    private:
        DirectionalVirtualShadowClipConfig previous_{};
        uint64_t revision_ = 0;
        bool initialized_ = false;
    };

    // One packed receiver per pixel in an explicit region. Empty/invalid depth
    // emits zero samples; no spatial subsampling is implied by the capacity cap.
    struct VirtualShadowDepthReceiverRegion {
        uint32_t sourceWidth = 0, sourceHeight = 0;
        uint32_t originX = 0, originY = 0;
        uint32_t width = 0, height = 0;
        bool reverseDepth = false;
    };

    struct DirectionalVirtualShadowMarkConfig {
        uint32_t pageSizeTexels = 128;
        uint32_t finerLevelGuardBandPages = 1;
        uint32_t maximumUniquePageRequests = 65'536;
    };

    // Per-light direct marking uses one raw-mark cell per page in the clip stack.
    // Every depth pixel contributes to its finest containing cell before a single
    // compaction. The combined clip grid must fit the configured workspace.
    struct VirtualShadowFullViewPageGrid {
        std::vector<uint32_t> levelOffsets;
        uint32_t cellCount = 0;
    };

    struct DirectionalVirtualShadowMarkPlan {
        std::vector<VirtualShadowPageRequest> requests;
        uint64_t inputReceiverSamples = 0;
        uint64_t markedReceiverSamples = 0;
        uint64_t unmappedReceiverSamples = 0;
        uint64_t requestCapacityDroppedSamples = 0;
        uint32_t invalidReceivers = 0;
        uint32_t uniquePagesBeforeCapacity = 0;
        uint32_t requestCapacityOverflow = 0;
    };

    enum class DirectionalVirtualShadowGpuMarkStatus : int32_t {
        Unmapped = 0,
        Mapped = 1,
        InvalidReceiver = 2,
        AddressOverflow = 3,
    };

    // std430 ABI for the M7.8 raw GPU marking pass. Deduplication and bounded
    // request compaction consume these records in the following pass.
    struct alignas(16) PackedDirectionalVirtualShadowClipLevel {
        glm::mat4 worldToShadowClip{ 1.0f };
        int32_t worldPageOriginX = 0;
        int32_t worldPageOriginY = 0;
        uint32_t virtualResolutionTexels = 0;
        uint32_t levelAndMip = 0;
        int32_t priority = 0;
        uint32_t requiredLayers = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
    };

    struct alignas(16) PackedDirectionalVirtualShadowReceiver {
        glm::vec4 worldPosition{ 0.0f, 0.0f, 0.0f, 1.0f };
        uint32_t receiverSamples = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
        uint32_t reserved2 = 0;
    };

    struct alignas(16) PackedDirectionalVirtualShadowGpuMark {
        int32_t pageX = 0;
        int32_t pageY = 0;
        int32_t selectedLevelIndex = -1;
        DirectionalVirtualShadowGpuMarkStatus status =
            DirectionalVirtualShadowGpuMarkStatus::Unmapped;
        uint32_t receiverSamples = 0;
        uint32_t abiVersion = VirtualShadowMapAbiVersion;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
    };

    struct alignas(16) PackedDirectionalVirtualShadowGpuRequest {
        int32_t pageX = 0;
        int32_t pageY = 0;
        int32_t level = 0;
        int32_t mip = 0;
        uint32_t receiverSamples = 0;
        int32_t priority = 0;
        uint32_t requiredLayers = 0;
        uint32_t selectedLevelIndex = 0;
    };

    struct alignas(16) PackedDirectionalVirtualShadowGpuCompactionTelemetry {
        uint32_t uniquePagesBeforeCapacity = 0;
        uint32_t outputRequestCount = 0;
        uint32_t requestCapacityOverflow = 0;
        uint32_t requestCapacityDroppedSamples = 0;
        uint32_t abiMismatchMarks = 0;
        uint32_t invalidLevelMarks = 0;
        uint32_t reserved0 = 0;
        uint32_t reserved1 = 0;
    };

    static_assert(sizeof(PackedDirectionalVirtualShadowClipLevel) == 96);
    [[nodiscard]] PackedDirectionalVirtualShadowClipLevel packDirectionalVirtualShadowClipLevel(
        const DirectionalVirtualShadowClipLevel& source);
    static_assert(sizeof(PackedDirectionalVirtualShadowReceiver) == 32);
    static_assert(sizeof(PackedDirectionalVirtualShadowGpuMark) == 32);
    static_assert(sizeof(PackedDirectionalVirtualShadowGpuRequest) == 32);
    static_assert(sizeof(
        PackedDirectionalVirtualShadowGpuCompactionTelemetry) == 32);
    static_assert(std::is_standard_layout_v<
        PackedDirectionalVirtualShadowClipLevel>);
    static_assert(std::is_trivially_copyable_v<
        PackedDirectionalVirtualShadowClipLevel>);

    // CPU mirror of physical residency. Static and dynamic layers retain
    // independent validity so a moving caster does not invalidate cached static
    // depth. Sampling combines only layers declared valid for the request.
    struct VirtualShadowResidentPage {
        VirtualShadowPageAddress address;
        uint64_t staticCasterRevision = 0;
        uint64_t dynamicCasterRevision = 0;
        uint64_t lastUsedFrame = 0;
        uint64_t lastRenderedFrame = 0;
        uint32_t physicalPage = InvalidVirtualShadowPhysicalPage;
        int32_t lastPriority = 0;
        uint8_t validLayers = VirtualShadowPageLayerNone;
    };

    // Separate from marking ABI v2. All 64-bit values use low/high uint words;
    // UUID words preserve bytes in increasing order, little-endian within a word.
    constexpr uint32_t VirtualShadowResidencyAbiVersion = 1;
    struct alignas(16) PackedVirtualShadowPageKey {
        std::array<uint32_t, 4> ownerWords{};
        std::array<uint32_t, 4> signedPageWords{}; // X low/high, Y low/high.
        std::array<uint32_t, 2> projectionRevisionWords{};
        uint32_t projectionDescriptor = 0; // mip:16, level/face:8, projection:8.
        uint32_t abiVersion = 0; // Zero identifies an unused record, never a key.
    };
    struct alignas(16) PackedVirtualShadowResidentPage {
        PackedVirtualShadowPageKey key;
        std::array<uint32_t, 2> staticRevisionWords{}, dynamicRevisionWords{};
        std::array<uint32_t, 2> lastUsedFrameWords{}, lastRenderedFrameWords{};
        uint32_t physicalPage = InvalidVirtualShadowPhysicalPage;
        int32_t lastPriority = 0;
        uint32_t validLayers = VirtualShadowPageLayerNone;
        uint32_t reserved = 0;
    };
    static_assert(sizeof(PackedVirtualShadowPageKey) == 48);
    static_assert(offsetof(PackedVirtualShadowPageKey, signedPageWords) == 16);
    static_assert(offsetof(PackedVirtualShadowPageKey, projectionDescriptor) == 40);
    static_assert(offsetof(PackedVirtualShadowPageKey, abiVersion) == 44);
    static_assert(sizeof(PackedVirtualShadowResidentPage) == 96);
    static_assert(offsetof(PackedVirtualShadowResidentPage, staticRevisionWords) == 48);
    static_assert(offsetof(PackedVirtualShadowResidentPage, physicalPage) == 80);
    static_assert(std::is_trivially_copyable_v<PackedVirtualShadowResidentPage>);
    static_assert(std::is_standard_layout_v<PackedVirtualShadowResidentPage>);
    [[nodiscard]] PackedVirtualShadowPageKey packVirtualShadowPageKey(const VirtualShadowPageAddress& address);
    [[nodiscard]] VirtualShadowPageAddress unpackVirtualShadowPageKey(const PackedVirtualShadowPageKey& key);
    [[nodiscard]] PackedVirtualShadowResidentPage packVirtualShadowResidentPage(
        const VirtualShadowResidentPage& page, uint32_t physicalPageCapacity);
    [[nodiscard]] VirtualShadowResidentPage unpackVirtualShadowResidentPage(
        const PackedVirtualShadowResidentPage& page, uint32_t physicalPageCapacity);
    // Qualification bridge only: residency must ultimately consume requests on
    // the GPU, not use retired CPU readback as a frame-critical allocator input.
    [[nodiscard]] VirtualShadowPageRequest unpackDirectionalVirtualShadowGpuRequest(
        const PackedDirectionalVirtualShadowGpuRequest& request,
        std::span<const DirectionalVirtualShadowClipLevel> clips, uint32_t pageSizeTexels);

    struct VirtualShadowPageMapping {
        VirtualShadowPageAddress address;
        uint64_t staticCasterRevision = 0;
        uint64_t dynamicCasterRevision = 0;
        uint32_t physicalPage = InvalidVirtualShadowPhysicalPage;
        uint32_t receiverSamples = 0;
        VirtualShadowPageState state = VirtualShadowPageState::Missing;
        VirtualShadowMissingPageAction missingAction =
            VirtualShadowMissingPageAction::FullyLitFailVisible;
        uint8_t requiredLayers = VirtualShadowPageLayerNone;
        uint8_t updateLayers = VirtualShadowPageLayerNone;
    };

    struct VirtualShadowPagePlan {
        uint32_t abiVersion = VirtualShadowMapAbiVersion;
        std::vector<VirtualShadowPageMapping> mappings;
        std::vector<VirtualShadowResidentPage> nextResidency;
        uint32_t uniqueRequests = 0;
        uint32_t cacheHits = 0;
        uint32_t pagesAllocated = 0;
        uint32_t pagesEvicted = 0;
        uint32_t pagesToRender = 0;
        uint32_t missingPages = 0;
        uint32_t updateBudgetOverflow = 0;
        uint32_t physicalPoolOverflow = 0;
    };

    // Qualification allocator uses sorted unique requests; live marking order
    // is NOT this priority/coverage/full-key order. Consumers must inspect
    // invalidInput before reading mappings. A successful plan remains pending
    // until matching atlas raster completion is published separately.
    inline constexpr uint32_t VirtualShadowResidencyReferenceCapacity = 256;
    struct alignas(16) PackedVirtualShadowResidencyRequest {
        PackedVirtualShadowPageKey key;
        std::array<uint32_t, 2> staticRevisionWords{}, dynamicRevisionWords{};
        uint32_t receiverSamples = 0;
        int32_t priority = 0;
        uint32_t requiredLayers = 0, reserved = 0;
    };
    struct alignas(16) PackedVirtualShadowPageMapping {
        PackedVirtualShadowPageKey key;
        std::array<uint32_t, 2> staticRevisionWords{}, dynamicRevisionWords{};
        uint32_t physicalPage = InvalidVirtualShadowPhysicalPage, receiverSamples = 0;
        uint32_t state = 0, missingAction = 0, requiredLayers = 0, updateLayers = 0;
        uint32_t reserved0 = 0, reserved1 = 0;
    };
    struct alignas(16) PackedVirtualShadowResidencyTelemetry {
        uint32_t uniqueRequests = 0, cacheHits = 0, pagesAllocated = 0, pagesEvicted = 0;
        uint32_t pagesToRender = 0, missingPages = 0, updateBudgetOverflow = 0, physicalPoolOverflow = 0;
        uint32_t invalidInput = 0, reserved0 = 0, reserved1 = 0, reserved2 = 0;
    };
    static_assert(sizeof(PackedVirtualShadowResidencyRequest) == 80);
    static_assert(sizeof(PackedVirtualShadowPageMapping) == 96);
    static_assert(sizeof(PackedVirtualShadowResidencyTelemetry) == 48);
    static_assert(offsetof(PackedVirtualShadowResidencyRequest, receiverSamples) == 64);
    static_assert(offsetof(PackedVirtualShadowPageMapping, physicalPage) == 64);
    static_assert(offsetof(PackedVirtualShadowPageMapping, state) == 72);
    static_assert(offsetof(PackedVirtualShadowResidencyTelemetry, invalidInput) == 32);
    [[nodiscard]] PackedVirtualShadowResidencyRequest packVirtualShadowResidencyRequest(const VirtualShadowPageRequest& request);
    [[nodiscard]] VirtualShadowPageMapping unpackVirtualShadowPageMapping(
        const PackedVirtualShadowPageMapping& mapping, uint32_t physicalPageCapacity);

    struct VirtualShadowAtlasLayout {
        uint32_t widthTexels = 0, heightTexels = 0;
        uint32_t tilesPerRow = 0, tileFootprintTexels = 0;
        uint32_t physicalPageCapacity = 0;
        uint32_t pageSizeTexels = 0, borderTexels = 0;
    };

    // Viewport/scissor cover the entire tile, including rasterized borders.
    // pageUvToAtlasUv maps interior page UV [0,1] to normalized atlas UV.
    // Static/dynamic storage and GPU draw generation are separate consumers.
    struct VirtualShadowPageRasterRegion {
        glm::mat4 worldToPageClip{1.0f};
        glm::vec4 pageUvToAtlasUv{}; // xy scale, zw bias.
        uint32_t originXTexels = 0, originYTexels = 0;
        uint32_t extentTexels = 0;
        uint32_t physicalPage = InvalidVirtualShadowPhysicalPage;
        uint8_t updateLayers = VirtualShadowPageLayerNone;
    };

    [[nodiscard]] VirtualShadowAtlasLayout buildVirtualShadowAtlasLayout(
        uint32_t pageSizeTexels, uint32_t borderTexels,
        uint32_t physicalPageCapacity, uint32_t maximumDimensionTexels);
    [[nodiscard]] VirtualShadowPageRasterRegion buildDirectionalVirtualShadowPageRasterRegion(
        const VirtualShadowAtlasLayout& atlas,
        const DirectionalVirtualShadowClipLevel& clip,
        const VirtualShadowPageMapping& mapping);
    // Unknown/singular bounds fail visible; forward-zero-to-one clip convention.
    [[nodiscard]] bool virtualShadowPageMayContainCaster(
        const VirtualShadowPageRasterRegion& region,
        const VirtualShadowCasterBounds& caster) noexcept;

    [[nodiscard]] std::string validateVirtualShadowPagePolicy(
        const VirtualShadowPagePolicy& policy);
    [[nodiscard]] std::string validateVirtualShadowResourceConfig(
        const VirtualShadowResourceConfig& config);
    [[nodiscard]] uint32_t validateVirtualShadowDepthReceiverRegion(
        const VirtualShadowDepthReceiverRegion& region, uint32_t capacity);
    [[nodiscard]] std::vector<PackedDirectionalVirtualShadowReceiver>
        buildVirtualShadowDepthReceivers(const VirtualShadowDepthReceiverRegion& region,
            const glm::mat4& inverseViewProjection, std::span<const float> depth,
            uint32_t capacity = 65'536);
    [[nodiscard]] VirtualShadowFullViewPageGrid buildVirtualShadowFullViewPageGrid(
        const DirectionalVirtualShadowMarkConfig& config,
        std::span<const DirectionalVirtualShadowClipLevel> levels,
        uint32_t cellCapacity);
    [[nodiscard]] VirtualShadowGpuWorkingSetLayout
        buildVirtualShadowGpuWorkingSetLayout(
            const VirtualShadowResourceConfig& config,
            uint64_t storageBufferOffsetAlignment);

    // Deterministic CPU oracle for the directional receiver-marking compute
    // pass. The finest containing clip level wins; a page-sized guard band on
    // finer levels moves edge receivers to a coarser level without gaps.
    [[nodiscard]] DirectionalVirtualShadowMarkPlan
        buildDirectionalVirtualShadowReceiverMarks(
            const DirectionalVirtualShadowMarkConfig& config,
            std::span<const DirectionalVirtualShadowClipLevel> levels,
            std::span<const DirectionalVirtualShadowReceiverSample> receivers);

    // Deterministic CPU reference for receiver-marked page resolution. It never
    // exposes stale revisions as sampleable. Pages awaiting raster remain pending
    // until commitVirtualShadowPageUpdates observes completion.
    [[nodiscard]] VirtualShadowPagePlan buildVirtualShadowPagePlan(
        const VirtualShadowPagePolicy& policy,
        std::span<const VirtualShadowPageRequest> requests,
        std::span<const VirtualShadowResidentPage> currentResidency,
        uint64_t frameSerial);

    void commitVirtualShadowPageUpdates(VirtualShadowPagePlan& plan,
        std::span<const uint32_t> completedPhysicalPages,
        uint64_t frameSerial);

} // namespace Iridium
