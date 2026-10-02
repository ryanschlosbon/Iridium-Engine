#pragma once

#include "scene/SceneEntityUuid.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace Iridium {

    inline constexpr uint32_t kInitialGpuLightCapacity = 256;
    inline constexpr uint32_t kMaximumGpuLightCapacity = 65'536;
    inline constexpr uint32_t kMaximumGlobalDirectionalLights = 8;
    inline constexpr uint32_t kInvalidShadowDataSlot = 0xffff'ffffu;

    enum class PackedGpuLightType : uint32_t {
        Directional = 0,
        Point = 1,
        Spot = 2,
    };

    enum PackedGpuLightFlag : uint32_t {
        PackedGpuLightCastsShadows = 1u << 2u,
        PackedGpuLightShadowQualityShift = 3u,
        PackedGpuLightShadowQualityMask = 3u <<
            PackedGpuLightShadowQualityShift,
    };

    // std430-compatible ABI shared by deferred, forward, and future RT consumers.
    // The record stores no persistent identity or Vulkan object.
    struct alignas(16) PackedGpuLight {
        glm::vec4 positionRange{};
        // World-space local +Z emission direction; w is spot outer-cone cosine.
        // Shading negates xyz when it needs the surface-to-light direction.
        glm::vec4 directionOuterCos{};
        glm::vec4 colorIntensity{};
        glm::vec4 shapeMetadata{};
    };

    struct LightRecordRange {
        uint32_t firstRecord = 0;
        uint32_t recordCount = 0;
    };

    struct LightSelectionMetadata {
        SceneEntityUuid owner;
        int32_t priority = 0;
        bool castsShadows = false;
    };

    enum class LightExtractionDiagnosticCode : uint8_t {
        MissingIdentity,
        MissingTransform,
        InvalidTransform,
        InvalidPhysicalValue,
        ZeroLuminanceColor,
        UnsupportedArea,
        CapacityExceeded,
    };

    struct LightExtractionDiagnostic {
        LightExtractionDiagnosticCode code =
            LightExtractionDiagnosticCode::InvalidPhysicalValue;
        SceneEntityUuid owner;
        std::string propertyPath;
        std::string message;
    };

    struct LightExtractionStats {
        uint32_t sceneLightCount = 0;
        uint32_t activeLightCount = 0;
        uint32_t directionalLightCount = 0;
        uint32_t localLightCount = 0;
        uint32_t changedRecordCount = 0;
        uint32_t changedRangeCount = 0;
        uint32_t omittedLightCount = 0;
        uint32_t capacity = 0;
        uint64_t changedRecordBytes = 0;
    };

    // Spans remain valid until the owning extractor runs again. Backends consume
    // this packet synchronously and track per-frame uploaded revisions.
    struct LightingFramePacket {
        std::span<const PackedGpuLight> records;
        std::span<const uint64_t> recordRevisions;
        std::span<const uint32_t> activeSlots;
        std::span<const LightSelectionMetadata> selectionMetadata;
        std::span<const LightRecordRange> changedRanges;
        uint64_t activeListRevision = 0;
        uint32_t requiredCapacity = kInitialGpuLightCapacity;
        LightExtractionStats stats;
    };

    struct LightingUploadTelemetry {
        uint64_t bytes = 0;
        uint32_t ranges = 0;
        uint32_t activeLights = 0;
        uint32_t capacity = 0;
    };

    struct ClusteredLightingTelemetry {
        uint64_t bufferBytesPerFrame = 0;
        uint32_t clusterCount = 0;
        uint32_t activeLights = 0;
        uint32_t directionalLights = 0;
        uint32_t localLights = 0;
        uint32_t clustersUsed = 0;
        uint32_t maximumOccupancy = 0;
        uint32_t requestedReferences = 0;
        uint32_t publishedReferences = 0;
        uint32_t fallbackLights = 0;
        uint32_t droppedLights = 0;
        uint32_t overflowCode = 0;
        bool available = false;
    };

    static_assert(sizeof(PackedGpuLight) == 64);
    static_assert(alignof(PackedGpuLight) == 16);
    static_assert(offsetof(PackedGpuLight, positionRange) == 0);
    static_assert(offsetof(PackedGpuLight, directionOuterCos) == 16);
    static_assert(offsetof(PackedGpuLight, colorIntensity) == 32);
    static_assert(offsetof(PackedGpuLight, shapeMetadata) == 48);
    static_assert(std::is_standard_layout_v<PackedGpuLight>);
    static_assert(std::is_trivially_copyable_v<PackedGpuLight>);

} // namespace Iridium
