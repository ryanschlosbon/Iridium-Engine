#pragma once

#include "capture/TgaImage.h"

#include <array>
#include <cstdint>

namespace Iridium {

    struct ImageChannelMetrics {
        // All values are expressed in 8-bit code values, not normalized units.
        double meanAbsoluteErrorCode = 0.0;
        double rootMeanSquareErrorCode = 0.0;
        uint8_t maximumAbsoluteErrorCode = 0;
    };

    struct LumaSsimMetrics {
        // SSIM is evaluated over sRGB-decoded, linear Rec.709 luma.
        double mean = 1.0;
        double minimum = 1.0;
        double percentile5 = 1.0;
        uint32_t windowSize = 8;
        uint64_t windowCount = 0;
    };

    struct ImageComparisonOptions {
        // A pixel is changed when max(abs(R), abs(G), abs(B)) is strictly greater
        // than this threshold. Alpha is intentionally excluded from this count.
        uint8_t changedPixelCodeThreshold = 0;
        uint32_t ssimWindowSize = 8;
    };

    struct ImageComparisonThresholds {
        // The maximum is evaluated across all RGBA channels.
        uint8_t maximumAbsoluteErrorCode = 0;
        double maximumChangedPixelFraction = 0.0;
        // This gate is applied to the mean fixed-window luma SSIM.
        double minimumMeanLumaSsim = 1.0;
    };

    struct ImageComparisonMetrics {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t pixelCount = 0;
        // Canonical channel order is R, G, B, A, regardless of TGA byte order.
        std::array<ImageChannelMetrics, 4> rgba{};
        uint8_t maximumAbsoluteErrorCode = 0;
        double maximumRgbPixelDifferencePercentile95Code = 0.0;
        double maximumRgbPixelDifferencePercentile99Code = 0.0;
        uint8_t changedPixelCodeThreshold = 0;
        uint64_t changedPixelCount = 0;
        double changedPixelFraction = 0.0;
        LumaSsimMetrics lumaSsim{};
    };

    struct ImageComparisonResult {
        ImageComparisonMetrics metrics{};
        ImageComparisonThresholds thresholds{};
        bool maximumAbsoluteErrorPassed = false;
        bool changedPixelFractionPassed = false;
        bool meanLumaSsimPassed = false;

        [[nodiscard]] bool passed() const noexcept {
            return maximumAbsoluteErrorPassed && changedPixelFractionPassed &&
                meanLumaSsimPassed;
        }
    };

    [[nodiscard]] ImageComparisonResult compareImages(
        const TgaImage& reference, const TgaImage& candidate,
        const ImageComparisonOptions& options = {},
        const ImageComparisonThresholds& thresholds = {});

    // The heatmap uses max RGB code difference. Zero difference is exactly black;
    // nonzero values are grayscale round(diff * scale), saturated to 255.
    [[nodiscard]] TgaImage makeImageDifferenceHeatmap(
        const TgaImage& reference, const TgaImage& candidate,
        double scale = 4.0);

} // namespace Iridium
