#include "capture/ImageComparison.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Iridium {

    namespace {

        constexpr std::array<size_t, 4> RgbaOffsets{ 2, 1, 0, 3 };
        constexpr double SsimC1 = 0.01 * 0.01;
        constexpr double SsimC2 = 0.03 * 0.03;

        [[nodiscard]] size_t checkedPixelStorage(const TgaImage& image) {
            if (image.width == 0 || image.height == 0) {
                throw std::invalid_argument(
                    "Image comparison requires nonzero image dimensions.");
            }
            const uint64_t byteCount = static_cast<uint64_t>(image.width) *
                image.height * 4;
            if (byteCount > std::numeric_limits<size_t>::max() ||
                image.bgra8.size() != static_cast<size_t>(byteCount)) {
                throw std::invalid_argument(
                    "Image comparison requires tightly packed BGRA8 pixels.");
            }
            return static_cast<size_t>(byteCount);
        }

        void validateCompatibleImages(const TgaImage& reference,
            const TgaImage& candidate) {
            (void)checkedPixelStorage(reference);
            (void)checkedPixelStorage(candidate);
            if (reference.width != candidate.width ||
                reference.height != candidate.height) {
                throw std::invalid_argument(
                    "Image comparison dimensions do not match.");
            }
        }

        void validateOptions(const ImageComparisonOptions& options,
            const ImageComparisonThresholds& thresholds) {
            if (options.ssimWindowSize == 0) {
                throw std::invalid_argument("SSIM window size must be nonzero.");
            }
            if (!std::isfinite(thresholds.maximumChangedPixelFraction) ||
                thresholds.maximumChangedPixelFraction < 0.0 ||
                thresholds.maximumChangedPixelFraction > 1.0) {
                throw std::invalid_argument(
                    "Maximum changed-pixel fraction must be in [0, 1].");
            }
            if (!std::isfinite(thresholds.minimumMeanLumaSsim) ||
                thresholds.minimumMeanLumaSsim < -1.0 ||
                thresholds.minimumMeanLumaSsim > 1.0) {
                throw std::invalid_argument(
                    "Minimum mean luma SSIM must be in [-1, 1].");
            }
        }

        [[nodiscard]] uint8_t codeValue(const std::byte value) noexcept {
            return std::to_integer<uint8_t>(value);
        }

        [[nodiscard]] uint8_t absoluteDifference(uint8_t first,
            uint8_t second) noexcept {
            return first >= second ? static_cast<uint8_t>(first - second) :
                static_cast<uint8_t>(second - first);
        }

        [[nodiscard]] const std::array<double, 256>& srgbLinearTable() {
            static const std::array<double, 256> table = [] {
                std::array<double, 256> values{};
                for (size_t code = 0; code < values.size(); ++code) {
                    const double encoded = static_cast<double>(code) / 255.0;
                    values[code] = encoded <= 0.04045 ? encoded / 12.92 :
                        std::pow((encoded + 0.055) / 1.055, 2.4);
                }
                return values;
            }();
            return table;
        }

        [[nodiscard]] double srgbToLinear(uint8_t code) {
            return srgbLinearTable()[code];
        }

        [[nodiscard]] double linearRec709Luma(const std::byte* bgra) {
            return 0.2126 * srgbToLinear(codeValue(bgra[2])) +
                0.7152 * srgbToLinear(codeValue(bgra[1])) +
                0.0722 * srgbToLinear(codeValue(bgra[0]));
        }

        template <typename Value>
        [[nodiscard]] Value nearestRankPercentile(
            std::vector<Value> values, double percentile) {
            if (values.empty()) {
                throw std::invalid_argument(
                    "A percentile requires at least one sample.");
            }
            std::sort(values.begin(), values.end());
            const size_t rank = static_cast<size_t>(std::ceil(
                percentile * static_cast<double>(values.size())));
            return values[std::max<size_t>(1, rank) - 1];
        }

        [[nodiscard]] uint8_t histogramNearestRankPercentile(
            const std::array<uint64_t, 256>& histogram, uint64_t sampleCount,
            double percentile) {
            const uint64_t rank = std::max<uint64_t>(1,
                static_cast<uint64_t>(std::ceil(
                    percentile * static_cast<double>(sampleCount))));
            uint64_t cumulative = 0;
            for (size_t code = 0; code < histogram.size(); ++code) {
                cumulative += histogram[code];
                if (cumulative >= rank) return static_cast<uint8_t>(code);
            }
            throw std::logic_error("Pixel-difference histogram is incomplete.");
        }

        [[nodiscard]] double computeWindowSsim(const TgaImage& reference,
            const TgaImage& candidate, uint32_t startX, uint32_t startY,
            uint32_t endX, uint32_t endY) {
            const uint64_t sampleCount = static_cast<uint64_t>(endX - startX) *
                (endY - startY);
            double referenceMean = 0.0;
            double candidateMean = 0.0;
            for (uint32_t y = startY; y < endY; ++y) {
                for (uint32_t x = startX; x < endX; ++x) {
                    const size_t offset =
                        (static_cast<size_t>(y) * reference.width + x) * 4;
                    referenceMean += linearRec709Luma(reference.bgra8.data() + offset);
                    candidateMean += linearRec709Luma(candidate.bgra8.data() + offset);
                }
            }
            referenceMean /= static_cast<double>(sampleCount);
            candidateMean /= static_cast<double>(sampleCount);

            double referenceVariance = 0.0;
            double candidateVariance = 0.0;
            double covariance = 0.0;
            for (uint32_t y = startY; y < endY; ++y) {
                for (uint32_t x = startX; x < endX; ++x) {
                    const size_t offset =
                        (static_cast<size_t>(y) * reference.width + x) * 4;
                    const double referenceDelta =
                        linearRec709Luma(reference.bgra8.data() + offset) -
                        referenceMean;
                    const double candidateDelta =
                        linearRec709Luma(candidate.bgra8.data() + offset) -
                        candidateMean;
                    referenceVariance += referenceDelta * referenceDelta;
                    candidateVariance += candidateDelta * candidateDelta;
                    covariance += referenceDelta * candidateDelta;
                }
            }
            referenceVariance /= static_cast<double>(sampleCount);
            candidateVariance /= static_cast<double>(sampleCount);
            covariance /= static_cast<double>(sampleCount);

            const double luminance = 2.0 * referenceMean * candidateMean + SsimC1;
            const double contrastStructure = 2.0 * covariance + SsimC2;
            const double denominator =
                (referenceMean * referenceMean + candidateMean * candidateMean +
                    SsimC1) *
                (referenceVariance + candidateVariance + SsimC2);
            return std::clamp((luminance * contrastStructure) / denominator,
                -1.0, 1.0);
        }

        [[nodiscard]] LumaSsimMetrics computeLumaSsim(
            const TgaImage& reference, const TgaImage& candidate,
            uint32_t windowSize) {
            std::vector<double> windowScores;
            const uint64_t windowColumns =
                (static_cast<uint64_t>(reference.width) + windowSize - 1) /
                windowSize;
            const uint64_t windowRows =
                (static_cast<uint64_t>(reference.height) + windowSize - 1) /
                windowSize;
            windowScores.reserve(static_cast<size_t>(windowColumns * windowRows));

            for (uint64_t startY = 0; startY < reference.height;
                startY += windowSize) {
                for (uint64_t startX = 0; startX < reference.width;
                    startX += windowSize) {
                    windowScores.push_back(computeWindowSsim(reference, candidate,
                        static_cast<uint32_t>(startX),
                        static_cast<uint32_t>(startY),
                        static_cast<uint32_t>(std::min<uint64_t>(
                            startX + windowSize, reference.width)),
                        static_cast<uint32_t>(std::min<uint64_t>(
                            startY + windowSize, reference.height))));
                }
            }

            LumaSsimMetrics result{};
            result.windowSize = windowSize;
            result.windowCount = windowScores.size();
            double scoreSum = 0.0;
            for (const double score : windowScores) scoreSum += score;
            result.mean = scoreSum / static_cast<double>(windowScores.size());
            result.minimum = *std::min_element(windowScores.begin(),
                windowScores.end());
            result.percentile5 = nearestRankPercentile(windowScores, 0.05);
            return result;
        }

    } // namespace

    ImageComparisonResult compareImages(const TgaImage& reference,
        const TgaImage& candidate, const ImageComparisonOptions& options,
        const ImageComparisonThresholds& thresholds) {
        validateCompatibleImages(reference, candidate);
        validateOptions(options, thresholds);

        ImageComparisonResult result{};
        result.thresholds = thresholds;
        result.metrics.width = reference.width;
        result.metrics.height = reference.height;
        result.metrics.pixelCount =
            static_cast<uint64_t>(reference.width) * reference.height;
        result.metrics.changedPixelCodeThreshold =
            options.changedPixelCodeThreshold;

        std::array<double, 4> absoluteSums{};
        std::array<double, 4> squaredSums{};
        std::array<uint64_t, 256> maximumRgbDifferenceHistogram{};

        for (uint64_t pixel = 0; pixel < result.metrics.pixelCount; ++pixel) {
            const size_t offset = static_cast<size_t>(pixel) * 4;
            uint8_t maximumRgbDifference = 0;
            for (size_t channel = 0; channel < RgbaOffsets.size(); ++channel) {
                const size_t byteOffset = offset + RgbaOffsets[channel];
                const uint8_t difference = absoluteDifference(
                    codeValue(reference.bgra8[byteOffset]),
                    codeValue(candidate.bgra8[byteOffset]));
                absoluteSums[channel] += difference;
                squaredSums[channel] +=
                    static_cast<double>(difference) * difference;
                result.metrics.rgba[channel].maximumAbsoluteErrorCode =
                    std::max(result.metrics.rgba[channel].maximumAbsoluteErrorCode,
                        difference);
                result.metrics.maximumAbsoluteErrorCode =
                    std::max(result.metrics.maximumAbsoluteErrorCode, difference);
                if (channel < 3) {
                    maximumRgbDifference =
                        std::max(maximumRgbDifference, difference);
                }
            }
            ++maximumRgbDifferenceHistogram[maximumRgbDifference];
            if (maximumRgbDifference > options.changedPixelCodeThreshold) {
                ++result.metrics.changedPixelCount;
            }
        }

        const double pixelCount = static_cast<double>(result.metrics.pixelCount);
        for (size_t channel = 0; channel < result.metrics.rgba.size(); ++channel) {
            result.metrics.rgba[channel].meanAbsoluteErrorCode =
                absoluteSums[channel] / pixelCount;
            result.metrics.rgba[channel].rootMeanSquareErrorCode =
                std::sqrt(squaredSums[channel] / pixelCount);
        }
        result.metrics.maximumRgbPixelDifferencePercentile95Code =
            histogramNearestRankPercentile(maximumRgbDifferenceHistogram,
                result.metrics.pixelCount, 0.95);
        result.metrics.maximumRgbPixelDifferencePercentile99Code =
            histogramNearestRankPercentile(maximumRgbDifferenceHistogram,
                result.metrics.pixelCount, 0.99);
        result.metrics.changedPixelFraction =
            static_cast<double>(result.metrics.changedPixelCount) / pixelCount;
        result.metrics.lumaSsim = computeLumaSsim(reference, candidate,
            options.ssimWindowSize);

        // Equality passes deliberately, enabling a strict bit-exact policy with
        // thresholds 0, 0, and 1.
        result.maximumAbsoluteErrorPassed =
            result.metrics.maximumAbsoluteErrorCode <=
            thresholds.maximumAbsoluteErrorCode;
        result.changedPixelFractionPassed =
            result.metrics.changedPixelFraction <=
            thresholds.maximumChangedPixelFraction;
        result.meanLumaSsimPassed =
            result.metrics.lumaSsim.mean >= thresholds.minimumMeanLumaSsim;
        return result;
    }

    TgaImage makeImageDifferenceHeatmap(const TgaImage& reference,
        const TgaImage& candidate, double scale) {
        validateCompatibleImages(reference, candidate);
        if (!std::isfinite(scale) || scale < 0.0) {
            throw std::invalid_argument(
                "Image-difference heatmap scale must be finite and nonnegative.");
        }

        TgaImage heatmap{};
        heatmap.width = reference.width;
        heatmap.height = reference.height;
        heatmap.bgra8.resize(reference.bgra8.size());
        const uint64_t pixelCount =
            static_cast<uint64_t>(reference.width) * reference.height;
        for (uint64_t pixel = 0; pixel < pixelCount; ++pixel) {
            const size_t offset = static_cast<size_t>(pixel) * 4;
            uint8_t maximumRgbDifference = 0;
            for (const size_t channelOffset : { size_t{ 0 }, size_t{ 1 },
                    size_t{ 2 } }) {
                maximumRgbDifference = std::max(maximumRgbDifference,
                    absoluteDifference(
                        codeValue(reference.bgra8[offset + channelOffset]),
                        codeValue(candidate.bgra8[offset + channelOffset])));
            }
            const uint8_t intensity = static_cast<uint8_t>(std::lround(
                std::min(255.0,
                    static_cast<double>(maximumRgbDifference) * scale)));
            heatmap.bgra8[offset] = static_cast<std::byte>(intensity);
            heatmap.bgra8[offset + 1] = static_cast<std::byte>(intensity);
            heatmap.bgra8[offset + 2] = static_cast<std::byte>(intensity);
            heatmap.bgra8[offset + 3] = std::byte{ 255 };
        }
        return heatmap;
    }

} // namespace Iridium
