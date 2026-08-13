#include "capture/ImageComparison.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " \
                    << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    [[nodiscard]] bool near(double actual, double expected,
        double tolerance = 1.0e-12) {
        return std::abs(actual - expected) <= tolerance;
    }

    [[nodiscard]] TgaImage image(uint32_t width, uint32_t height,
        uint8_t blue = 0, uint8_t green = 0, uint8_t red = 0,
        uint8_t alpha = 255) {
        TgaImage result{};
        result.width = width;
        result.height = height;
        result.bgra8.resize(static_cast<size_t>(width) * height * 4);
        for (uint64_t pixel = 0;
            pixel < static_cast<uint64_t>(width) * height; ++pixel) {
            const size_t offset = static_cast<size_t>(pixel) * 4;
            result.bgra8[offset] = static_cast<std::byte>(blue);
            result.bgra8[offset + 1] = static_cast<std::byte>(green);
            result.bgra8[offset + 2] = static_cast<std::byte>(red);
            result.bgra8[offset + 3] = static_cast<std::byte>(alpha);
        }
        return result;
    }

    void setPixel(TgaImage& target, uint32_t x, uint32_t y,
        uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha) {
        const size_t offset =
            (static_cast<size_t>(y) * target.width + x) * 4;
        target.bgra8[offset] = static_cast<std::byte>(blue);
        target.bgra8[offset + 1] = static_cast<std::byte>(green);
        target.bgra8[offset + 2] = static_cast<std::byte>(red);
        target.bgra8[offset + 3] = static_cast<std::byte>(alpha);
    }

    [[nodiscard]] uint8_t byteValue(std::byte value) {
        return std::to_integer<uint8_t>(value);
    }

    bool testExactEqualityAndStrictThresholds() {
        const TgaImage reference = image(9, 9, 17, 83, 201, 44);
        const ImageComparisonResult result = compareImages(reference, reference);
        CHECK(result.passed());
        CHECK(result.metrics.width == 9);
        CHECK(result.metrics.height == 9);
        CHECK(result.metrics.pixelCount == 81);
        for (const ImageChannelMetrics& channel : result.metrics.rgba) {
            CHECK(channel.meanAbsoluteErrorCode == 0.0);
            CHECK(channel.rootMeanSquareErrorCode == 0.0);
            CHECK(channel.maximumAbsoluteErrorCode == 0);
        }
        CHECK(result.metrics.maximumAbsoluteErrorCode == 0);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile95Code == 0.0);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile99Code == 0.0);
        CHECK(result.metrics.changedPixelCount == 0);
        CHECK(result.metrics.changedPixelFraction == 0.0);
        CHECK(result.metrics.lumaSsim.mean == 1.0);
        CHECK(result.metrics.lumaSsim.minimum == 1.0);
        CHECK(result.metrics.lumaSsim.percentile5 == 1.0);
        CHECK(result.metrics.lumaSsim.windowSize == 8);
        CHECK(result.metrics.lumaSsim.windowCount == 4);
        return true;
    }

    bool testPerChannelMetricsAndChangedThreshold() {
        TgaImage reference = image(2, 1, 0, 0, 0, 0);
        TgaImage candidate = reference;
        // Differences in canonical RGBA order are (1,2,3,4) and (5,6,7,8).
        setPixel(candidate, 0, 0, 3, 2, 1, 4);
        setPixel(candidate, 1, 0, 7, 6, 5, 8);

        ImageComparisonOptions options{};
        options.changedPixelCodeThreshold = 3;
        ImageComparisonThresholds thresholds{};
        thresholds.maximumAbsoluteErrorCode = 8;
        thresholds.maximumChangedPixelFraction = 0.5;
        thresholds.minimumMeanLumaSsim = -1.0;
        const ImageComparisonResult result = compareImages(reference, candidate,
            options, thresholds);

        CHECK(near(result.metrics.rgba[0].meanAbsoluteErrorCode, 3.0));
        CHECK(near(result.metrics.rgba[0].rootMeanSquareErrorCode,
            std::sqrt(13.0)));
        CHECK(result.metrics.rgba[0].maximumAbsoluteErrorCode == 5);
        CHECK(near(result.metrics.rgba[1].meanAbsoluteErrorCode, 4.0));
        CHECK(near(result.metrics.rgba[1].rootMeanSquareErrorCode,
            std::sqrt(20.0)));
        CHECK(result.metrics.rgba[1].maximumAbsoluteErrorCode == 6);
        CHECK(near(result.metrics.rgba[2].meanAbsoluteErrorCode, 5.0));
        CHECK(near(result.metrics.rgba[2].rootMeanSquareErrorCode,
            std::sqrt(29.0)));
        CHECK(result.metrics.rgba[2].maximumAbsoluteErrorCode == 7);
        CHECK(near(result.metrics.rgba[3].meanAbsoluteErrorCode, 6.0));
        CHECK(near(result.metrics.rgba[3].rootMeanSquareErrorCode,
            std::sqrt(40.0)));
        CHECK(result.metrics.rgba[3].maximumAbsoluteErrorCode == 8);
        CHECK(result.metrics.maximumAbsoluteErrorCode == 8);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile95Code == 7.0);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile99Code == 7.0);
        CHECK(result.metrics.changedPixelCount == 1);
        CHECK(result.metrics.changedPixelFraction == 0.5);
        CHECK(result.passed());
        return true;
    }

    bool testNearestRankPixelPercentiles() {
        const TgaImage reference = image(100, 1);
        TgaImage candidate = reference;
        for (uint32_t x = 0; x < 100; ++x) {
            setPixel(candidate, x, 0, static_cast<uint8_t>(x), 0, 0, 255);
        }
        ImageComparisonOptions options{};
        options.changedPixelCodeThreshold = 255;
        ImageComparisonThresholds thresholds{};
        thresholds.maximumAbsoluteErrorCode = 99;
        thresholds.minimumMeanLumaSsim = -1.0;
        const ImageComparisonResult result = compareImages(reference, candidate,
            options, thresholds);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile95Code == 94.0);
        CHECK(result.metrics.maximumRgbPixelDifferencePercentile99Code == 98.0);
        CHECK(result.metrics.changedPixelFraction == 0.0);
        CHECK(result.passed());
        return true;
    }

    bool testFixedWindowLinearLumaSsim() {
        const TgaImage reference = image(16, 8, 128, 128, 128);
        TgaImage candidate = reference;
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 8; x < 16; ++x) {
                setPixel(candidate, x, y, 255, 255, 255, 255);
            }
        }
        ImageComparisonThresholds thresholds{};
        thresholds.maximumAbsoluteErrorCode = 127;
        thresholds.maximumChangedPixelFraction = 0.5;
        thresholds.minimumMeanLumaSsim = -1.0;
        const ImageComparisonResult result = compareImages(reference, candidate,
            {}, thresholds);
        CHECK(result.metrics.lumaSsim.windowCount == 2);
        CHECK(result.metrics.lumaSsim.minimum < 1.0);
        CHECK(result.metrics.lumaSsim.minimum > 0.0);
        const double grayLinear = std::pow(
            (128.0 / 255.0 + 0.055) / 1.055, 2.4);
        const double expectedChangedWindowSsim =
            (2.0 * grayLinear + 0.0001) /
            (grayLinear * grayLinear + 1.0 + 0.0001);
        CHECK(near(result.metrics.lumaSsim.minimum,
            expectedChangedWindowSsim));
        CHECK(result.metrics.lumaSsim.percentile5 ==
            result.metrics.lumaSsim.minimum);
        CHECK(near(result.metrics.lumaSsim.mean,
            (1.0 + result.metrics.lumaSsim.minimum) / 2.0));
        return true;
    }

    bool testThresholdEqualityIsInclusive() {
        const TgaImage reference = image(1, 1);
        TgaImage candidate = reference;
        setPixel(candidate, 0, 0, 0, 0, 1, 255);

        ImageComparisonThresholds permissive{};
        permissive.maximumAbsoluteErrorCode = 1;
        permissive.maximumChangedPixelFraction = 1.0;
        permissive.minimumMeanLumaSsim = -1.0;
        const ImageComparisonResult measured = compareImages(reference, candidate,
            {}, permissive);

        ImageComparisonThresholds equalToMetrics{};
        equalToMetrics.maximumAbsoluteErrorCode =
            measured.metrics.maximumAbsoluteErrorCode;
        equalToMetrics.maximumChangedPixelFraction =
            measured.metrics.changedPixelFraction;
        equalToMetrics.minimumMeanLumaSsim = measured.metrics.lumaSsim.mean;
        CHECK(compareImages(reference, candidate, {}, equalToMetrics).passed());

        equalToMetrics.maximumAbsoluteErrorCode = 0;
        CHECK(!compareImages(reference, candidate, {}, equalToMetrics).passed());

        ImageComparisonOptions ignoredDifference{};
        ignoredDifference.changedPixelCodeThreshold = 1;
        equalToMetrics.maximumAbsoluteErrorCode = 1;
        equalToMetrics.maximumChangedPixelFraction = 0.0;
        CHECK(compareImages(reference, candidate, ignoredDifference,
            equalToMetrics).passed());
        return true;
    }

    bool testHeatmapIsDeterministicAndBlackAtZero() {
        const TgaImage reference = image(3, 1, 10, 20, 30, 40);
        TgaImage candidate = reference;
        setPixel(candidate, 1, 0, 12, 20, 30, 40);
        setPixel(candidate, 2, 0, 10, 20, 30, 255); // Alpha-only difference.
        const TgaImage heatmap = makeImageDifferenceHeatmap(reference, candidate,
            10.0);
        CHECK(heatmap.width == 3 && heatmap.height == 1);
        for (const size_t offset : { size_t{ 0 }, size_t{ 8 } }) {
            CHECK(byteValue(heatmap.bgra8[offset]) == 0);
            CHECK(byteValue(heatmap.bgra8[offset + 1]) == 0);
            CHECK(byteValue(heatmap.bgra8[offset + 2]) == 0);
            CHECK(byteValue(heatmap.bgra8[offset + 3]) == 255);
        }
        CHECK(byteValue(heatmap.bgra8[4]) == 20);
        CHECK(byteValue(heatmap.bgra8[5]) == 20);
        CHECK(byteValue(heatmap.bgra8[6]) == 20);
        CHECK(byteValue(heatmap.bgra8[7]) == 255);
        return true;
    }

    bool testDimensionAndStorageMismatchAreRejected() {
        try {
            (void)compareImages(image(2, 2), image(2, 3));
            return false;
        }
        catch (const std::invalid_argument&) {
        }

        TgaImage malformed = image(2, 2);
        malformed.bgra8.pop_back();
        try {
            (void)compareImages(image(2, 2), malformed);
            return false;
        }
        catch (const std::invalid_argument&) {
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[] = {
        { "exact equality and strict thresholds",
            testExactEqualityAndStrictThresholds },
        { "per-channel metrics and changed threshold",
            testPerChannelMetricsAndChangedThreshold },
        { "nearest-rank pixel percentiles", testNearestRankPixelPercentiles },
        { "fixed-window linear-luma SSIM", testFixedWindowLinearLumaSsim },
        { "inclusive threshold equality", testThresholdEqualityIsInclusive },
        { "deterministic black-at-zero heatmap",
            testHeatmapIsDeterministicAndBlackAtZero },
        { "dimension and storage rejection",
            testDimensionAndStorageMismatchAreRejected },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
