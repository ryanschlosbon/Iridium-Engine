#pragma once

// M7R R3c.11: one frame's render inputs, submitted once with
// IRenderBackend::submitFrame between beginFrame and endFrame.
//
// Every span and pointer is non-owning and only has to stay valid until
// submitFrame returns; callers build the frame from storage they reuse, so a
// steady frame allocates nothing. The backend records the frame in a fixed
// stage order (shadows, probe captures, main-view G-buffer, lighting,
// forward and transparency, output transform, UI) and reports the end of the
// stages a caller may observe through IRenderFrameStageObserver, inside
// submitFrame, so caller-side bookkeeping and profile counters keep their
// place relative to the backend's own work.
//
// The GPU-scene tables are not part of the frame: publishGpuScene runs right
// after beginFrame because shadow-caster revisions, queried while the frame
// is extracted, read the slot's published mirror.

#include "DrawPacket.h"
#include "GpuScene.h"
#include "LightingTypes.h"
#include "Mesh.h"
#include "ReflectionProbeCapture.h"
#include "ReflectionProbeTypes.h"
#include "RenderDebugView.h"
#include "ShadowTypes.h"
#include "ViewportGridOverlay.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Iridium {

    // Shadow consumers address persistent scene geometry through the same
    // frame-local dense primitive indices used by GPU-scene indirect work.
    // Packets remain only as an explicit compatibility path for producers
    // that could not enter the persistent publication.
    struct ShadowCasterSubmission {
        std::span<const uint32_t> gpuScenePrimitiveIndices;
        std::span<const DrawPacket> directPackets;
        uint64_t membershipRevision = 0;

        [[nodiscard]] constexpr size_t size() const noexcept {
            return gpuScenePrimitiveIndices.size() + directPackets.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0u;
        }
    };

    // Reflection captures are an independent visibility consumer. Dense
    // references come from GpuSceneConsumerProbe rather than either main-view
    // queue; packets remain only for publication fallback.
    struct ReflectionProbeCasterSubmission {
        std::span<const uint32_t> gpuScenePrimitiveIndices;
        std::span<const DrawPacket> directPackets;
        uint64_t membershipRevision = 0;

        [[nodiscard]] constexpr size_t size() const noexcept {
            return gpuScenePrimitiveIndices.size() + directPackets.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0u;
        }
    };

    // M7R R5c.4b: the main view's G-buffer work. It replaces the M7.2 parity
    // packets (one DrawPacket rebuilt per published primitive every frame):
    // GPU-scene work is the published main-opaque list, addressed by dense
    // primitive index; direct packets remain for producers outside the
    // publication.
    inline constexpr uint32_t OpaqueSubmissionDirectBit = 1u << 31u;

    struct OpaqueSubmission {
        // Draw order. One std::sort with the opaque comparator
        // (opaqueSortKey, geometry, firstIndex) over the direct packets in
        // append order followed by the main-opaque primitives in ascending
        // order, as the parity queue was sorted. An entry is a dense
        // GPU-scene primitive index, or OpaqueSubmissionDirectBit | the index
        // of a direct packet.
        std::span<const uint32_t> order;
        // In draw order (their entries in `order` count up from 0).
        std::span<const DrawPacket> directPackets;
        // M9 G4: last frame's world transform of each direct packet (parallel
        // to directPackets). GPU-scene entries read theirs from the tables.
        std::span<const glm::mat4> directPreviousTransforms;
        uint32_t gpuScenePrimitiveCount = 0;
        uint64_t membershipRevision = 0;
        // Per dense primitive: the CPU frustum classification of this view
        // (the former DrawPacketCpuVisibilityOracle bit). Empty when the
        // frame has no GPU-scene work.
        std::span<const uint8_t> cpuVisibility;

        [[nodiscard]] static constexpr bool isDirect(uint32_t entry) noexcept {
            return (entry & OpaqueSubmissionDirectBit) != 0u;
        }
        [[nodiscard]] static constexpr uint32_t indexOf(uint32_t entry) noexcept {
            return entry & ~OpaqueSubmissionDirectBit;
        }
        [[nodiscard]] bool cpuVisible(uint32_t primitiveIndex) const noexcept {
            return primitiveIndex < cpuVisibility.size() &&
                cpuVisibility[primitiveIndex] != 0u;
        }
        [[nodiscard]] constexpr size_t size() const noexcept {
            return order.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return order.empty();
        }
    };

    // One shadow kind's casters and its (possibly empty) frame packets.
    // Cached storage is updated before any opaque/forward consumer reads it;
    // an empty packet list disables sampling of that kind.
    template <typename Packet>
    struct RenderFrameShadows {
        ShadowCasterSubmission casters{};
        std::span<const Packet> shadows{};
    };

    // Live display-output settings. The backend applies them when they differ
    // from the active ones (and validates them then).
    struct RenderFrameOutputSettings {
        float manualExposureEv = 0.0f;
        float paperWhiteNits = 203.0f;
        float peakNits = 1000.0f;
    };

    // Stage boundaries reported during submitFrame, in this order.
    enum class RenderFrameStage : uint8_t {
        DirectionalShadows,
        SpotShadows,
        PointShadows,
        // Only when the frame submits probe captures.
        ReflectionProbeCaptures,
        // Deferred lighting recorded; lighting/cluster telemetry is current.
        Lighting,
        // Scene-linear color is complete (after transparency).
        SceneLinearComplete,
        // The display output is complete, before the UI pass.
        OutputComplete,
    };

    class IRenderFrameStageObserver {
    public:
        virtual void onRenderFrameStage(RenderFrameStage stage) = 0;

    protected:
        ~IRenderFrameStageObserver() = default;
    };

    struct RenderFrame {
        // The view: camera uniforms, history identity, and the camera
        // position, matrices and near/far planes the lighting, clusters and
        // probes use (the transport record's view, projection,
        // cameraPosition and depthRange).
        ViewTransportRecord view{};
        ViewHistoryContext history{};
        RenderDebugView debugView = RenderDebugView::Final;
        RenderFrameOutputSettings output{};
        // Not visible unless the editor supplies one.
        ViewportGridOverlay gridOverlay{};

        // Shadows (always submitted; empty kinds record nothing).
        RenderFrameShadows<DirectionalShadowFramePacket> directionalShadows{};
        RenderFrameShadows<SpotShadowFramePacket> spotShadows{};
        RenderFrameShadows<PointShadowFramePacket> pointShadows{};

        // Runtime reflection-probe captures (scene views only; an asset
        // preview submits none).
        bool submitReflectionProbeCaptures = false;
        ReflectionProbeCasterSubmission probeCasters{};
        std::span<const ReflectionProbeCaptureScheduleEntry> probeCaptureSchedule{};

        // Main-view work. Both depth-writing inputs freeze the depth-pyramid
        // history identity before main-view compaction.
        OpaqueSubmission opaque{};
        std::span<const DrawPacket> selectionQueue{};
        bool wireframe = false;
        std::span<const DrawPacket> forwardOpaqueQueue{};
        // M9 G4: last frame's world transform per forward-opaque packet
        // (parallel to forwardOpaqueQueue). Transparent queues carry none:
        // transparency writes no velocity (it feeds the reactive mask).
        std::span<const glm::mat4> forwardOpaquePreviousTransforms{};
        std::span<const DrawPacket> sortedSurfaceQueue{};
        std::span<const DrawPacket> compatibilityTransparentQueue{};
        // Frame-local transforms referenced by DrawPacket instance ranges.
        std::span<const glm::mat4> instanceTransforms{};

        // Lights (lighting and probe captures) and the published reflection
        // probes. Required.
        const LightingFramePacket* lights = nullptr;
        const ReflectionProbeGpuFramePacket* reflectionProbes = nullptr;

        // Optional; called on the submitting thread inside submitFrame.
        IRenderFrameStageObserver* stageObserver = nullptr;
    };

    // Backend telemetry of the frame being recorded: GPU-scene uploads after
    // publishGpuScene, probe captures after the ReflectionProbeCaptures stage,
    // light uploads and clusters after the Lighting stage.
    struct RenderFrameTelemetry {
        GpuSceneUploadTelemetry gpuSceneUpload{};
        ReflectionProbeCaptureTelemetry probeCaptures{};
        LightingUploadTelemetry lightUploads{};
        ClusteredLightingTelemetry clusters{};
    };

} // namespace Iridium
