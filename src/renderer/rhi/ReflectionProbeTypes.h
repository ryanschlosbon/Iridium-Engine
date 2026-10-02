#pragma once

#include "scene/SceneEntityUuid.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Iridium {

    inline constexpr uint32_t kInitialGpuReflectionProbeCapacity = 64;
    inline constexpr uint32_t kMaximumGpuReflectionProbeCapacity = 4'096;
    inline constexpr uint32_t kMaximumGpuReflectionProbeEnvironments = 64;
    inline constexpr uint32_t kInvalidEnvironmentTableSlot = 0xffff'ffffu;

    enum PackedGpuReflectionProbeFlag : uint32_t {
        PackedGpuReflectionProbeBoxShape = 1u << 0u,
        PackedGpuReflectionProbeBoxProjection = 1u << 1u,
    };

    // std430-compatible local-specular-probe ABI. Transform scale is removed by
    // extraction. The environment slot is an abstract table index, never a Vulkan
    // descriptor or persistent asset identity.
    struct alignas(16) PackedGpuReflectionProbe {
        glm::mat4 worldToProbe{ 1.0f };
        glm::vec4 influence{}; // sphere radius or box extents xyz; blend in w
        glm::vec4 positionIntensity{}; // world position xyz; intensity in w
        glm::uvec4 metadata{}; // flags, environment slot, signed priority bits, selection rank
    };

    struct alignas(16) PackedGpuReflectionProbeClusterParameters {
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };
        glm::mat4 inverseView{ 1.0f };
        glm::uvec4 grid{};   // width, height, tiles-x, tiles-y
        glm::vec4 depth{};   // near, far, slices/log(far/near), reserved
        glm::uvec4 limits{}; // slices, per-cluster, references, active probes
        glm::uvec4 tiles{};  // tile width, tile height, reserved, reserved
    };

    struct ReflectionProbeRecordRange {
        uint32_t firstRecord = 0;
        uint32_t recordCount = 0;
    };

    struct ReflectionProbeSelectionMetadata {
        SceneEntityUuid owner;
        int32_t priority = 0;
        float influenceVolume = 0.0f;
    };

    struct ReflectionProbePublicationStats {
        uint32_t extractedCandidateCount = 0;
        uint32_t activeProbeCount = 0;
        uint32_t nonresidentProbeCount = 0;
        uint32_t unresolvedEnvironmentCount = 0;
        uint32_t capacityOmittedCount = 0;
        uint32_t changedRecordCount = 0;
        uint32_t changedRangeCount = 0;
        uint32_t capacity = 0;
        uint64_t changedRecordBytes = 0;
    };

    // Spans remain valid until the owning publisher runs again. The backend may
    // patch abstract environment slots to its indexed cubemap table during upload.
    struct ReflectionProbeGpuFramePacket {
        std::span<const PackedGpuReflectionProbe> records;
        std::span<const uint64_t> recordRevisions;
        std::span<const uint32_t> activeSlots;
        std::span<const ReflectionProbeSelectionMetadata> selectionMetadata;
        std::span<const ReflectionProbeRecordRange> changedRanges;
        uint64_t activeListRevision = 0;
        uint32_t requiredCapacity = kInitialGpuReflectionProbeCapacity;
        ReflectionProbePublicationStats stats;
    };

    static_assert(sizeof(PackedGpuReflectionProbe) == 112);
    static_assert(alignof(PackedGpuReflectionProbe) == 16);
    static_assert(offsetof(PackedGpuReflectionProbe, worldToProbe) == 0);
    static_assert(offsetof(PackedGpuReflectionProbe, influence) == 64);
    static_assert(offsetof(PackedGpuReflectionProbe, positionIntensity) == 80);
    static_assert(offsetof(PackedGpuReflectionProbe, metadata) == 96);
    static_assert(std::is_standard_layout_v<PackedGpuReflectionProbe>);
    static_assert(std::is_trivially_copyable_v<PackedGpuReflectionProbe>);
    static_assert(sizeof(PackedGpuReflectionProbeClusterParameters) == 256);
    static_assert(alignof(PackedGpuReflectionProbeClusterParameters) == 16);
    static_assert(std::is_standard_layout_v<
        PackedGpuReflectionProbeClusterParameters>);
    static_assert(std::is_trivially_copyable_v<
        PackedGpuReflectionProbeClusterParameters>);

} // namespace Iridium
