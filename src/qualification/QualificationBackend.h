#pragma once

// The qualification harness's view of its backend extension (M7R R2.9).
//
// The harness never calls capture or readback methods on IRenderBackend (they
// no longer exist there). It owns one IQualificationBackend, hands its
// extension to the Application through IFrameObserver::backendExtensions(),
// configures it before the backend is initialized, arms requests inside the
// open frame and collects results between frames. VulkanQualificationExtension
// is the only implementation; the interface keeps the harness free of Vulkan
// headers and lets its tests run against a device-free fake.

#include "core/types/FrameCapture.h"
#include "material/TransparencyPolicy.h"
#include "qualification/QualificationResults.h"
#include "renderer/rhi/RenderBackendExtension.h"

#include <cstdint>
#include <vector>

namespace Iridium {

    // Oracle and startup-validator selection. Fixed before backend init: the
    // extension's graph-hook declarations depend on it.
    struct QualificationBackendConfig {
        bool shadowIndirectOracle = false;
        bool gpuLodOracle = false;
        bool probeLodOracle = false;
        bool depthOcclusionOracle = false;
        bool virtualShadowDepthOracle = false;
        // Exercise reflection-probe capture-target acquire/promote/retire at
        // backend initialization.
        bool validateProbeCaptureTargets = false;
    };

    class IQualificationBackend {
    public:
        virtual ~IQualificationBackend() = default;

        // The extension to attach through RenderBackendCreateInfo. Owned by
        // this object, which must outlive the backend.
        [[nodiscard]] virtual IRenderBackendExtension& backendExtension() noexcept = 0;
        // Before the backend is created (StartupPhase::Configure).
        virtual void configureQualification(
            const QualificationBackendConfig& config) = 0;

        // Requests, armed inside the open frame (FrameBeginPhase::
        // BackendFrameOpened). Each is consumed by the next matching hook in
        // the frame's recording; one armed request of each kind at a time.
        virtual void armFrameCapture(uint64_t captureId,
            FrameCapturePoint point) = 0;
        virtual void armOrdinary2CaptureValidation(uint64_t validationId) = 0;
        virtual void armDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality) = 0;
        virtual void armDepthPyramidCaptureValidation(uint64_t validationId) = 0;

        // Between frames only. `waitForPending` waits for every frame in
        // flight before draining.
        [[nodiscard]] virtual std::vector<FrameCapture> collectFrameCaptures(
            bool waitForPending) = 0;
        [[nodiscard]] virtual std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool waitForPending) = 0;
        [[nodiscard]] virtual std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool waitForPending) = 0;
        [[nodiscard]] virtual std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool waitForPending) = 0;
    };

} // namespace Iridium
