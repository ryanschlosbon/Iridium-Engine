#pragma once

// Temporal image-quality metrics (M9 G6d). Standard C++ only; shared by the
// IridiumTemporalMetrics tool and its unit tests. Nothing here is linked into the
// engine.
//
// Conventions:
// - Images are held in canonical top-left row order, interleaved RGB.
// - Scene-domain images are scene-linear ACEScg (AP1) RGB float, as written by the
//   engine's PFM capture (src/capture/PfmImage.cpp: bottom row stored first,
//   little-endian, scale -1).
// - SDR-domain images are the engine's final-SDR TGA (8-bit, display-encoded).
// - "Tone-mapped luma" is the common [0,1) signal for temporal metrics:
//   scene: Y = AP1 luminance of (exposure * rgb), clamped at 0, then Y / (1 + Y);
//   SDR:   Rec.709 weights applied to the display-encoded code values / 255.
// - Reductions are evaluated per fixed row chunk and combined in chunk order, so
//   results do not depend on the worker count.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium::TemporalMetrics {

    inline constexpr double Ap1LuminanceR = 0.2722287;
    inline constexpr double Ap1LuminanceG = 0.6740818;
    inline constexpr double Ap1LuminanceB = 0.0536895;

    inline constexpr double Rec709LumaR = 0.2126;
    inline constexpr double Rec709LumaG = 0.7152;
    inline constexpr double Rec709LumaB = 0.0722;

    // ---------------------------------------------------------------- images
    struct FloatImage {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> rgb; // top-left rows, RGB interleaved
    };

    struct Rgb8Image {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgb; // top-left rows, RGB interleaved
    };

    // A single-channel image (luma, error or standard-deviation map).
    struct Plane {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> values;
        // Pixels whose source was NaN (stored as 0). +Inf luminance maps to 1.
        uint64_t nonFiniteCount = 0;
    };

    // Statistics include pixel i only when include[i] != 0.
    struct Mask {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> include;
        [[nodiscard]] uint64_t area() const;
    };

    enum class Domain { Scene, Sdr };
    [[nodiscard]] const char* domainName(Domain domain);
    // .pfm -> Scene, .tga -> Sdr; throws for other extensions.
    [[nodiscard]] Domain domainForPath(const std::filesystem::path& path);

    // ------------------------------------------------------------------- I/O
    // Accepts "PF" (RGB) and "Pf" (grayscale, replicated), either byte order, rows
    // stored bottom-up per the PFM specification.
    [[nodiscard]] FloatImage readPfm(const std::filesystem::path& path);
    // Writes the engine's layout: "PF\n<w> <h>\n-1.0\n", little-endian, bottom row
    // first.
    void writePfm(const std::filesystem::path& path, const FloatImage& image);
    // Uncompressed true-colour TGA, 24 or 32 bits, either vertical origin. Alpha is
    // ignored.
    [[nodiscard]] Rgb8Image readTga(const std::filesystem::path& path);
    // Canonical engine TGA layout: 32-bit BGRA, top-left origin, alpha 255.
    void writeTga(const std::filesystem::path& path, const Rgb8Image& image);

    // TGA: include where max(R,G,B) > 127. PFM: include where R > 0.5.
    [[nodiscard]] Mask readMask(const std::filesystem::path& path);
    [[nodiscard]] Mask regionMask(uint32_t width, uint32_t height,
        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);

    // Grayscale heatmaps: value * scale, written to all channels (TGA clamps to
    // [0,1] before quantization).
    void writeHeatmap(const std::filesystem::path& path, const Plane& plane,
        double scale);

    // ------------------------------------------------------------- execution
    // 0 = std::thread::hardware_concurrency().
    void setWorkerCount(unsigned count);
    [[nodiscard]] unsigned workerCount();
    inline constexpr uint32_t RowsPerChunk = 16;
    [[nodiscard]] uint32_t chunkCount(uint32_t rows);
    // Calls body(chunk, rowBegin, rowEnd, worker) for every RowsPerChunk-row chunk.
    // worker < workerCount() identifies the calling thread.
    void parallelForRows(uint32_t rows,
        const std::function<void(uint32_t chunk, uint32_t rowBegin,
            uint32_t rowEnd, unsigned worker)>& body);

    // ------------------------------------------------------------------ luma
    [[nodiscard]] Plane toneMappedLuma(const FloatImage& image,
        double exposureScale = 1.0);
    [[nodiscard]] Plane encodedLuma(const Rgb8Image& image);

    // ------------------------------------------------------- reference error
    struct SceneErrorOptions {
        double exposureScale = 1.0;
        double log2Epsilon = 1.0e-4;
        double toneMappedThreshold = 1.0 / 64.0;
    };

    struct SceneErrorMetrics {
        uint64_t pixelCount = 0;          // included and finite in both images
        uint64_t nonFinitePixelCount = 0; // included but excluded as non-finite
        double luminanceRmse = 0.0;
        double log2LuminanceMae = 0.0;
        double log2LuminanceRmse = 0.0;
        double toneMappedRmse = 0.0;      // per channel c / (1 + c), all channels
        double maxAbsLinear = 0.0;        // max over channels and pixels
        double maxAbsToneMapped = 0.0;
        uint64_t aboveThresholdPixelCount = 0; // max channel tone-mapped error > threshold
        double aboveThresholdFraction = 0.0;
    };

    [[nodiscard]] SceneErrorMetrics computeSceneError(const FloatImage& reference,
        const FloatImage& test, const Mask* mask = nullptr,
        const SceneErrorOptions& options = {});

    struct SsimResult {
        double mean = 1.0;
        uint64_t windowCount = 0;
    };

    // Gaussian 11x11 (sigma 1.5) SSIM, K1 = 0.01, K2 = 0.03, over the valid region
    // (no padding; output centres at least 5 pixels from every edge). With a mask,
    // only windows whose centre pixel is included are averaged.
    inline constexpr uint32_t SsimWindowSize = 11;
    [[nodiscard]] SsimResult computeSsim(const Plane& reference, const Plane& test,
        const Mask* mask = nullptr, double dynamicRange = 1.0);

    struct SdrErrorOptions {
        // A pixel changed when any RGB code differs by more than this value.
        uint8_t changedPixelCodeThreshold = 0;
    };

    struct SdrErrorMetrics {
        uint64_t pixelCount = 0;
        double mse = 0.0;      // over RGB codes
        double psnrDb = 0.0;   // +infinity for identical images
        uint32_t maxAbsCode = 0;
        uint64_t changedPixelCount = 0;
        double changedPixelFraction = 0.0;
        SsimResult lumaSsim{};
    };

    [[nodiscard]] SdrErrorMetrics computeSdrError(const Rgb8Image& reference,
        const Rgb8Image& test, const Mask* mask = nullptr,
        const SdrErrorOptions& options = {});

    // -------------------------------------------------------------- stability
    inline constexpr double DefaultFlickerThreshold = 1.0 / 255.0;

    struct StabilityMetrics {
        uint32_t frameCount = 0;
        uint64_t pixelCount = 0;
        // Pooled over every included pixel of every consecutive frame pair.
        double meanFrameDelta = 0.0;
        double p99FrameDelta = 0.0;   // histogram resolution 1/65535
        double maxFrameDelta = 0.0;
        std::vector<double> pairMeanDelta;
        // Population standard deviation over the frames, per pixel.
        double meanTemporalStdDev = 0.0;
        double p99TemporalStdDev = 0.0;
        double maxTemporalStdDev = 0.0;
        double flickerThreshold = DefaultFlickerThreshold;
        uint64_t flickerPixelCount = 0;
        double flickerPixelFraction = 0.0;
        // Mean temporal std over pixels whose std exceeds flickerThreshold.
        double flickerEnergy = 0.0;
    };

    class StabilityAccumulator {
    public:
        StabilityAccumulator(uint32_t width, uint32_t height,
            const Mask* mask = nullptr,
            double flickerThreshold = DefaultFlickerThreshold);
        void addFrame(const Plane& luma);
        [[nodiscard]] StabilityMetrics metrics() const;
        [[nodiscard]] Plane temporalStdDev() const;

    private:
        static constexpr uint32_t HistogramBins = 65536;
        uint32_t m_width;
        uint32_t m_height;
        std::vector<uint8_t> m_mask; // empty = all pixels
        double m_flickerThreshold;
        uint32_t m_frameCount = 0;
        std::vector<float> m_first;
        std::vector<float> m_previous;
        std::vector<double> m_sum;   // sum of (x - first)
        std::vector<double> m_sumSq; // sum of (x - first)^2
        std::vector<uint64_t> m_deltaHistogram;
        double m_deltaSum = 0.0;
        uint64_t m_deltaCount = 0;
        double m_deltaMax = 0.0;
        std::vector<double> m_pairMeanDelta;
        [[nodiscard]] bool included(size_t pixel) const {
            return m_mask.empty() || m_mask[pixel] != 0;
        }
    };

    // --------------------------------------------------- ghosting / recovery
    struct LumaErrorStats {
        uint64_t pixelCount = 0;
        double sumAbs = 0.0;
        double meanAbs = 0.0; // = trail energy when mask is a trail mask
        double rmse = 0.0;
        double maxAbs = 0.0;
    };

    [[nodiscard]] LumaErrorStats lumaError(const Plane& reference, const Plane& test,
        const Mask* mask = nullptr);

    // Include where |now - past| > threshold (and limit, when given, includes).
    [[nodiscard]] Mask trailMask(const Plane& referenceNow, const Plane& referencePast,
        double threshold, const Mask* limit = nullptr);

    struct RecoveryResult {
        double threshold = 0.0;
        bool thresholdFromSteadyState = false;
        double steadyStateMedian = 0.0;
        uint32_t steadyStateFrameCount = 0;
        // First frame index whose error is <= threshold.
        std::optional<uint32_t> firstBelowFrame;
        // First frame index from which every later error stays <= threshold.
        std::optional<uint32_t> settledFrame;
    };

    // Default threshold: multiplier * median error of the last quarter of frames
    // (at least one frame).
    [[nodiscard]] RecoveryResult evaluateRecovery(std::span<const double> errors,
        std::optional<double> absoluteThreshold = std::nullopt,
        double steadyStateMultiplier = 2.0);

    // ------------------------------------------------------------ accumulate
    class MeanAccumulator {
    public:
        void add(const FloatImage& image);
        [[nodiscard]] uint32_t count() const { return m_count; }
        [[nodiscard]] uint64_t nonFiniteSampleCount() const { return m_nonFinite; }
        [[nodiscard]] FloatImage mean() const;

    private:
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_count = 0;
        uint64_t m_nonFinite = 0;
        std::vector<double> m_sum;
    };

    // ----------------------------------------------------------------- utils
    // Nearest-rank percentile (p in [0,1]); values are reordered.
    [[nodiscard]] double nearestRankPercentile(std::vector<double>& values, double p);
    [[nodiscard]] double median(std::vector<double> values);

} // namespace Iridium::TemporalMetrics
