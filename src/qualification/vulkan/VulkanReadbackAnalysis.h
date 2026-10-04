#pragma once

// CPU analysis of the qualification readbacks recorded by
// VulkanQualificationExtension (M7R R2.7). Pure functions over the mapped
// bytes, so they are unit-testable without a device. The byte layouts are the
// ones the extension's copies produce; each function documents its own.

#include "core/types/FrameCapture.h"
#include "renderer/rhi/DepthPyramid.h"
#include "qualification/QualificationResults.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Iridium {

    struct FrameCaptureReadbackInfo {
        uint64_t captureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        FrameCapturePixelFormat pixelFormat = FrameCapturePixelFormat::Rgba8Srgb;
        FrameCapturePoint point = FrameCapturePoint::SceneLinear;
        // RGBA16F source widened to RGBA32F; otherwise 4-byte pixels copied.
        bool halfFloatSource = false;
    };
    [[nodiscard]] FrameCapture convertFrameCaptureReadback(
        const FrameCaptureReadbackInfo& info, std::span<const std::byte> bytes);

    // Layout: entry identity (R32), entry depth (D32), exit identity, exit
    // depth, then RGBA16F local color; each image tightly packed.
    struct Ordinary2ReadbackInfo {
        uint64_t validationId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t expectedDrawCount = 0;
        uint32_t workItemCount = 0;
    };
    [[nodiscard]] Ordinary2CaptureValidationResult
        analyzeOrdinary2CaptureReadback(const Ordinary2ReadbackInfo& info,
            std::span<const std::byte> bytes);

    // Layout: per interface identity (R32) then depth (D32); RGBA16F local
    // color; then one R32 16x16-tile termination mask per interface slot
    // (only termination interfaces are written).
    struct DeepLayeredReadbackInfo {
        uint64_t validationId = 0;
        TransparencyQuality quality = TransparencyQuality::Hero4;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t interfaceCount = 0;
        uint32_t expectedDrawCount = 0;
        uint32_t sceneResolveDrawCount = 0;
        uint32_t compatibilityForwardDrawCount = 0;
        uint32_t workItemCount = 0;
    };
    [[nodiscard]] DeepLayeredCaptureValidationResult
        analyzeDeepLayeredCaptureReadback(const DeepLayeredReadbackInfo& info,
            std::span<const std::byte> bytes);

    // Layout: the D32 source depth, then every pyramid mip in order (R32F).
    struct DepthPyramidReadbackInfo {
        uint64_t validationId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipCount = 0;
    };
    [[nodiscard]] DepthPyramidCaptureValidationResult
        analyzeDepthPyramidCaptureReadback(const DepthPyramidReadbackInfo& info,
            std::span<const std::byte> bytes);

} // namespace Iridium
