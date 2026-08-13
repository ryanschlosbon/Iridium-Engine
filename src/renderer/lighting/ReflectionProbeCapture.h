#pragma once

#include "scene/SceneEntityUuid.h"
#include "scene/components/ReflectionProbeComponent.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kReflectionProbeCaptureFaceCount = 6;
    inline constexpr uint8_t kReflectionProbeCaptureCompleteMask = 0x3fu;

    struct ReflectionProbeCaptureStorageFootprint {
        uint64_t rawRadianceBytes = 0;
        uint64_t depthBytes = 0;
        uint64_t prefilteredRadianceBytes = 0;
        uint64_t totalStagingBytes = 0;
    };

    [[nodiscard]] uint32_t reflectionProbeCaptureMipCount(
        uint32_t resolution);
    [[nodiscard]] ReflectionProbeCaptureStorageFootprint
        reflectionProbeCaptureStorageFootprint(uint32_t resolution);

    struct ReflectionProbeCaptureFace {
        uint32_t faceIndex = 0;
        glm::vec3 direction{};
        glm::vec3 up{};
        glm::mat4 worldToClip{ 1.0f };
    };

    // Matches the cooked +X,-X,+Y,-Y,+Z,-Z environment convention. The
    // projection deliberately does not invert Y: a positive-height Vulkan
    // viewport and the selected cube up vectors produce the cooked face layout.
    [[nodiscard]] std::array<ReflectionProbeCaptureFace,
        kReflectionProbeCaptureFaceCount> buildReflectionProbeCaptureFaces(
            glm::vec3 position, float nearPlane, float farPlane);

    struct ReflectionProbeCaptureRequest {
        SceneEntityUuid owner;
        ReflectionProbeUpdateMode updateMode =
            ReflectionProbeUpdateMode::OnDemand;
        glm::vec3 position{};
        uint32_t resolution = 512;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
        int32_t priority = 0;
        bool captureSky = true;

        // The caller owns revision production. Settings includes transform and
        // capture settings. Explicit request changes for a Capture Now action.
        // Scene/light/environment changes automatically dirty Realtime probes;
        // OnDemand and Baked probes retain their last product until requested.
        uint64_t settingsRevision = 1;
        uint64_t explicitRequestRevision = 0;
        uint64_t sceneRevision = 0;
        uint64_t lightingRevision = 0;
        uint64_t environmentRevision = 0;
        uint64_t pipelineRevision = 1;
        uint64_t frameIndex = 0;
    };

    enum class ReflectionProbeCaptureDirtyReason : uint8_t {
        None,
        NewProbe,
        ExplicitRequest,
        SettingsChanged,
        SceneChanged,
        LightingChanged,
        EnvironmentChanged,
        PipelineChanged,
    };

    struct ReflectionProbeCaptureScheduleEntry {
        SceneEntityUuid owner;
        uint64_t captureTicket = 0;
        ReflectionProbeCaptureDirtyReason dirtyReason =
            ReflectionProbeCaptureDirtyReason::None;
        std::array<ReflectionProbeCaptureFace,
            kReflectionProbeCaptureFaceCount> faces{};
        uint8_t capturedFaceMask = 0;
        uint8_t scheduledFaceMask = 0;
        uint32_t resolution = 0;
        glm::vec3 position{};
        float nearPlane = 0.1f;
        bool captureSky = true;
        ReflectionProbeUpdateMode updateMode =
            ReflectionProbeUpdateMode::OnDemand;
        bool sampleable = false;
        bool captureInFlight = false;
        bool awaitingPublication = false;
    };

    struct ReflectionProbeCaptureScheduleStats {
        uint32_t requests = 0;
        uint32_t cacheHits = 0;
        uint32_t dirty = 0;
        uint32_t capturesStarted = 0;
        uint32_t capturesInvalidated = 0;
        uint32_t capturesInFlight = 0;
        uint32_t awaitingPublication = 0;
        uint32_t facesScheduled = 0;
        uint32_t budgetDeferred = 0;
        uint32_t capacityDeferred = 0;
        uint32_t cadenceDeferred = 0;
        uint64_t renderedTexels = 0;
    };

    struct ReflectionProbeCaptureSchedule {
        std::vector<ReflectionProbeCaptureScheduleEntry> entries;
        ReflectionProbeCaptureScheduleStats stats;
    };

    struct ReflectionProbeCapturePublication {
        SceneEntityUuid owner;
        uint64_t captureTicket = 0;
        ReflectionProbeCaptureRequest capturedRequest;
    };

    struct ReflectionProbeCaptureCompletion {
        struct Product {
            uint32_t resolution = 0;
            uint32_t mipLevels = 0;
            std::vector<std::byte> radiance;
            std::vector<std::byte> prefilteredSpecular;
        };

        SceneEntityUuid owner;
        uint64_t captureTicket = 0;
        uint32_t environmentSlot = 0;
        std::optional<Product> bakedProduct;
    };

    struct ReflectionProbeCaptureTelemetry {
        uint32_t facesRendered = 0;
        uint32_t capturesFiltered = 0;
        uint32_t capturesPublished = 0;
        uint32_t capturesInFlight = 0;
        uint64_t renderedTexels = 0;
        uint64_t stagingLogicalBytes = 0;
        uint64_t publishedLogicalBytes = 0;
    };

    struct ReflectionProbeCaptureSchedulerConfig {
        uint64_t maximumRenderedTexels =
            6ull * 512ull * 512ull;
        uint32_t maximumFacesPerProbePerFrame = 6;
        uint32_t maximumCapturesInFlight = 4;
        uint32_t minimumRealtimeFramesBetweenCaptures = 6;
    };

    class ReflectionProbeCaptureScheduler final {
    public:
        explicit ReflectionProbeCaptureScheduler(
            ReflectionProbeCaptureSchedulerConfig config = {});

        void configure(ReflectionProbeCaptureSchedulerConfig config);
        [[nodiscard]] const ReflectionProbeCaptureSchedule& schedule(
            std::span<const ReflectionProbeCaptureRequest> requests);

        // Marks every face emitted by the current schedule as rendered into
        // private staging storage. A completed cube remains unpublished until
        // markPublished confirms filtering/upload and fence-safe visibility.
        void markScheduledFacesRendered();
        [[nodiscard]] std::span<const ReflectionProbeCapturePublication>
            publicationsReady();
        void markPublished(SceneEntityUuid owner, uint64_t captureTicket);
        void abandon(SceneEntityUuid owner, uint64_t captureTicket);
        void reset() noexcept;

        [[nodiscard]] bool hasPublishedCapture(
            SceneEntityUuid owner) const noexcept;
        [[nodiscard]] std::optional<uint64_t> publishedTicket(
            SceneEntityUuid owner) const noexcept;

    private:
        struct CaptureState {
            SceneEntityUuid owner;
            ReflectionProbeCaptureRequest published;
            ReflectionProbeCaptureRequest pending;
            uint64_t publishedTicket = 0;
            uint64_t pendingTicket = 0;
            uint8_t capturedFaceMask = 0;
            ReflectionProbeCaptureDirtyReason pendingReason =
                ReflectionProbeCaptureDirtyReason::None;
            bool hasPublished = false;
            bool hasPending = false;
            bool awaitingPublication = false;
        };

        [[nodiscard]] CaptureState* findState(
            SceneEntityUuid owner) noexcept;
        [[nodiscard]] const CaptureState* findState(
            SceneEntityUuid owner) const noexcept;
        [[nodiscard]] ReflectionProbeCaptureDirtyReason dirtyReason(
            const ReflectionProbeCaptureRequest& request,
            const CaptureState& state) const noexcept;
        [[nodiscard]] bool pendingIsCompatible(
            const ReflectionProbeCaptureRequest& request,
            const CaptureState& state) const noexcept;
        [[nodiscard]] uint64_t nextTicket();
        static void validateConfig(
            const ReflectionProbeCaptureSchedulerConfig& config);
        static void validateRequest(
            const ReflectionProbeCaptureRequest& request);

        ReflectionProbeCaptureSchedulerConfig config_;
        uint64_t nextTicket_ = 0;
        std::vector<CaptureState> states_;
        std::optional<ReflectionProbeCaptureSchedule> currentSchedule_;
        std::vector<ReflectionProbeCapturePublication> readyPublications_;
    };

} // namespace Iridium
