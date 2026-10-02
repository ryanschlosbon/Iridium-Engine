#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class FrameCapturePixelFormat : uint8_t {
        Rgba8Srgb,
        Bgra8Srgb,
        Rgba8Unorm,
        Bgra8Unorm,
        Rgba32Float,
    };

    enum class FrameCaptureColorDomain : uint8_t {
        LegacyDisplayReferred,
        SceneLinearAcesCg,
        DisplayEncodedSdr,
        DisplayLinearHdr,
    };

    enum class FrameCapturePoint : uint8_t {
        SceneLinear,
        FinalSdr,
        FinalOutput,
    };

    struct FrameCapture {
        uint64_t captureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitchBytes = 0;
        FrameCapturePixelFormat pixelFormat = FrameCapturePixelFormat::Rgba8Srgb;
        FrameCaptureColorDomain colorDomain =
            FrameCaptureColorDomain::LegacyDisplayReferred;
        std::vector<std::byte> pixels;
    };

    [[nodiscard]] constexpr std::string_view frameCapturePixelFormatName(
        FrameCapturePixelFormat format) noexcept {
        switch (format) {
        case FrameCapturePixelFormat::Rgba8Srgb: return "rgba8_srgb";
        case FrameCapturePixelFormat::Bgra8Srgb: return "bgra8_srgb";
        case FrameCapturePixelFormat::Rgba8Unorm: return "rgba8_unorm";
        case FrameCapturePixelFormat::Bgra8Unorm: return "bgra8_unorm";
        case FrameCapturePixelFormat::Rgba32Float: return "rgba32_float";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view frameCaptureColorDomainName(
        FrameCaptureColorDomain domain) noexcept {
        switch (domain) {
        case FrameCaptureColorDomain::LegacyDisplayReferred:
            return "legacy_display_referred_srgb_target";
        case FrameCaptureColorDomain::SceneLinearAcesCg:
            return "scene_linear_acescg_ap1";
        case FrameCaptureColorDomain::DisplayEncodedSdr:
            return "display_encoded_sdr_rec709";
        case FrameCaptureColorDomain::DisplayLinearHdr:
            return "display_linear_hdr_relative_paper_white";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view frameCapturePointName(
        FrameCapturePoint point) noexcept {
        switch (point) {
        case FrameCapturePoint::SceneLinear: return "scene";
        case FrameCapturePoint::FinalSdr: return "final-sdr";
        case FrameCapturePoint::FinalOutput: return "final-output";
        }
        return "unknown";
    }

} // namespace Iridium
