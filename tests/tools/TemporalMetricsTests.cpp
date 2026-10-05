// Unit tests for the M9 G6d temporal metrics library (tools/temporal). The PFM and
// TGA round trips run against the engine's own capture writers and readers
// (src/capture) so the tool reads exactly what the engine writes.

#include "temporal/TemporalMetrics.h"

#include "capture/PfmImage.h"
#include "capture/TgaImage.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    namespace tmx = Iridium::TemporalMetrics;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " \
                    << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    [[nodiscard]] bool near(double actual, double expected, double tolerance = 1.0e-9) {
        return std::abs(actual - expected) <= tolerance;
    }

    std::filesystem::path g_scratch;

    [[nodiscard]] std::filesystem::path scratch(const std::string& name) {
        return g_scratch / name;
    }

    [[nodiscard]] tmx::FloatImage constantImage(uint32_t w, uint32_t h, float r, float g,
        float b) {
        tmx::FloatImage image{ w, h, {} };
        image.rgb.resize(size_t(w) * h * 3);
        for (size_t p = 0; p < size_t(w) * h; ++p) {
            image.rgb[p * 3] = r;
            image.rgb[p * 3 + 1] = g;
            image.rgb[p * 3 + 2] = b;
        }
        return image;
    }

    [[nodiscard]] tmx::FloatImage randomImage(uint32_t w, uint32_t h, uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(0.0f, 4.0f);
        tmx::FloatImage image{ w, h, {} };
        image.rgb.resize(size_t(w) * h * 3);
        for (float& v : image.rgb) v = dist(rng);
        return image;
    }

    [[nodiscard]] tmx::Rgb8Image constantRgb8(uint32_t w, uint32_t h, uint8_t value) {
        tmx::Rgb8Image image{ w, h, {} };
        image.rgb.assign(size_t(w) * h * 3, value);
        return image;
    }

    [[nodiscard]] tmx::Plane constantPlane(uint32_t w, uint32_t h, float value) {
        tmx::Plane plane{};
        plane.width = w;
        plane.height = h;
        plane.values.assign(size_t(w) * h, value);
        return plane;
    }

    // ------------------------------------------------------- reference error

    bool testSceneIdenticalIsZero() {
        const tmx::FloatImage image = randomImage(64, 48, 1);
        const tmx::SceneErrorMetrics m = tmx::computeSceneError(image, image);
        CHECK(m.pixelCount == 64u * 48u);
        CHECK(m.nonFinitePixelCount == 0);
        CHECK(m.luminanceRmse == 0.0);
        CHECK(m.log2LuminanceMae == 0.0);
        CHECK(m.log2LuminanceRmse == 0.0);
        CHECK(m.toneMappedRmse == 0.0);
        CHECK(m.maxAbsLinear == 0.0);
        CHECK(m.maxAbsToneMapped == 0.0);
        CHECK(m.aboveThresholdPixelCount == 0);
        return true;
    }

    bool testSceneKnownScale() {
        // Grey 1 vs grey 2: AP1 weights sum to 1, so Y = 1 and 2.
        const tmx::FloatImage reference = constantImage(16, 16, 1.0f, 1.0f, 1.0f);
        const tmx::FloatImage test = constantImage(16, 16, 2.0f, 2.0f, 2.0f);
        const tmx::SceneErrorMetrics m = tmx::computeSceneError(reference, test);
        CHECK(near(m.luminanceRmse, 1.0, 1e-6));
        CHECK(near(m.log2LuminanceMae, 1.0, 1e-6));
        CHECK(near(m.log2LuminanceRmse, 1.0, 1e-6));
        // 2/3 - 1/2 = 1/6 per channel.
        CHECK(near(m.toneMappedRmse, 1.0 / 6.0, 1e-12));
        CHECK(near(m.maxAbsToneMapped, 1.0 / 6.0, 1e-12));
        CHECK(near(m.maxAbsLinear, 1.0, 1e-12));
        CHECK(m.aboveThresholdFraction == 1.0);

        // Exposure scales both images; log2 differences are unchanged.
        tmx::SceneErrorOptions options{};
        options.exposureScale = 0.5;
        const tmx::SceneErrorMetrics exposed = tmx::computeSceneError(reference, test,
            nullptr, options);
        CHECK(near(exposed.log2LuminanceMae, 1.0, 1e-6));
        CHECK(near(exposed.toneMappedRmse, 0.5 - 1.0 / 3.0, 1e-12));

        // Small error below the 1/64 threshold.
        const tmx::FloatImage close = constantImage(16, 16, 1.01f, 1.01f, 1.01f);
        const tmx::SceneErrorMetrics small = tmx::computeSceneError(reference, close);
        CHECK(small.aboveThresholdPixelCount == 0);
        CHECK(small.toneMappedRmse > 0.0);
        return true;
    }

    bool testSceneMaskAndNonFinite() {
        tmx::FloatImage reference = constantImage(16, 16, 1.0f, 1.0f, 1.0f);
        tmx::FloatImage test = reference;
        // Differences only in the right half; mask selects the left half.
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 8; x < 16; ++x) test.rgb[(size_t(y) * 16 + x) * 3] = 5.0f;
        }
        const tmx::Mask left = tmx::regionMask(16, 16, 0, 0, 8, 16);
        CHECK(left.area() == 128);
        const tmx::SceneErrorMetrics masked = tmx::computeSceneError(reference, test, &left);
        CHECK(masked.pixelCount == 128);
        CHECK(masked.toneMappedRmse == 0.0);
        const tmx::SceneErrorMetrics full = tmx::computeSceneError(reference, test);
        CHECK(full.aboveThresholdFraction == 0.5);

        test = reference;
        test.rgb[0] = std::numeric_limits<float>::quiet_NaN();
        test.rgb[4] = std::numeric_limits<float>::infinity();
        const tmx::SceneErrorMetrics nonFinite = tmx::computeSceneError(reference, test);
        CHECK(nonFinite.nonFinitePixelCount == 2);
        CHECK(nonFinite.pixelCount == 256 - 2);
        CHECK(nonFinite.toneMappedRmse == 0.0);
        return true;
    }

    bool testSdrIdenticalIsInfinitePsnr() {
        tmx::Rgb8Image image = constantRgb8(40, 30, 0);
        std::mt19937 rng(7);
        for (uint8_t& v : image.rgb) v = static_cast<uint8_t>(rng() & 0xffu);
        const tmx::SdrErrorMetrics m = tmx::computeSdrError(image, image);
        CHECK(std::isinf(m.psnrDb) && m.psnrDb > 0.0);
        CHECK(m.mse == 0.0);
        CHECK(m.lumaSsim.mean == 1.0);
        CHECK(m.lumaSsim.windowCount == (40u - 10u) * (30u - 10u));
        CHECK(m.changedPixelCount == 0);
        CHECK(m.maxAbsCode == 0);
        return true;
    }

    bool testSdrKnownShiftAndNoise() {
        // A vertical step edge shifted right by one column.
        const uint32_t w = 32, h = 16;
        tmx::Rgb8Image reference = constantRgb8(w, h, 0);
        tmx::Rgb8Image shifted = constantRgb8(w, h, 0);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t p = (size_t(y) * w + x) * 3;
                const uint8_t a = x >= 16 ? 255 : 0;
                const uint8_t b = x >= 17 ? 255 : 0;
                reference.rgb[p] = reference.rgb[p + 1] = reference.rgb[p + 2] = a;
                shifted.rgb[p] = shifted.rgb[p + 1] = shifted.rgb[p + 2] = b;
            }
        }
        const tmx::SdrErrorMetrics shift = tmx::computeSdrError(reference, shifted);
        CHECK(near(shift.changedPixelFraction, 1.0 / 32.0, 1e-15));
        CHECK(near(shift.mse, 255.0 * 255.0 / 32.0, 1e-9));
        CHECK(near(shift.psnrDb, 10.0 * std::log10(32.0), 1e-9));
        CHECK(shift.maxAbsCode == 255);
        CHECK(shift.lumaSsim.mean < 1.0);

        // +-4 checkerboard noise around 128: MSE 16.
        const tmx::Rgb8Image grey = constantRgb8(w, h, 128);
        tmx::Rgb8Image noisy = grey;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t p = (size_t(y) * w + x) * 3;
                const uint8_t v = ((x + y) & 1u) ? 132 : 124;
                noisy.rgb[p] = noisy.rgb[p + 1] = noisy.rgb[p + 2] = v;
            }
        }
        const tmx::SdrErrorMetrics noise = tmx::computeSdrError(grey, noisy);
        CHECK(near(noise.mse, 16.0, 1e-12));
        CHECK(near(noise.psnrDb, 10.0 * std::log10(65025.0 / 16.0), 1e-9));
        CHECK(noise.changedPixelFraction == 1.0);
        tmx::SdrErrorOptions options{};
        options.changedPixelCodeThreshold = 4;
        CHECK(tmx::computeSdrError(grey, noisy, nullptr, options).changedPixelCount == 0);
        return true;
    }

    bool testSsimConstantOffsetMatchesFormula() {
        // Constant images: no variance, so SSIM = (2ab + C1) / (a^2 + b^2 + C1).
        const tmx::Plane a = constantPlane(20, 20, 0.4f);
        const tmx::Plane b = constantPlane(20, 20, 0.5f);
        const tmx::SsimResult s = tmx::computeSsim(a, b);
        const double c1 = 0.0001;
        const double expected = (2.0 * 0.4 * 0.5 + c1) / (0.16 + 0.25 + c1);
        CHECK(near(s.mean, expected, 1e-5));
        CHECK(s.windowCount == 100);

        // Mask: only centres whose pixel is included are averaged.
        const tmx::Mask region = tmx::regionMask(20, 20, 5, 5, 8, 8);
        CHECK(tmx::computeSsim(a, b, &region).windowCount == 9);

        bool threw = false;
        try { (void)tmx::computeSsim(constantPlane(10, 10, 0), constantPlane(10, 10, 0)); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        return true;
    }

    // -------------------------------------------------------------- stability

    bool testStabilityStaticIsZero() {
        const tmx::Plane luma = tmx::toneMappedLuma(randomImage(37, 23, 3));
        tmx::StabilityAccumulator accumulator(37, 23);
        for (int i = 0; i < 6; ++i) accumulator.addFrame(luma);
        const tmx::StabilityMetrics m = accumulator.metrics();
        CHECK(m.frameCount == 6);
        CHECK(m.pairMeanDelta.size() == 5);
        CHECK(m.meanFrameDelta == 0.0);
        CHECK(m.p99FrameDelta == 0.0);
        CHECK(m.maxFrameDelta == 0.0);
        CHECK(m.meanTemporalStdDev == 0.0);
        CHECK(m.p99TemporalStdDev == 0.0);
        CHECK(m.flickerEnergy == 0.0);
        CHECK(m.flickerPixelCount == 0);
        return true;
    }

    bool testStabilityAlternatingFrames() {
        const uint32_t w = 40, h = 40;
        // Left half alternates 0.3 / 0.4; right half is static at 0.6.
        tmx::Plane even = constantPlane(w, h, 0.6f);
        tmx::Plane odd = even;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w / 2; ++x) {
                even.values[size_t(y) * w + x] = 0.3f;
                odd.values[size_t(y) * w + x] = 0.4f;
            }
        }
        const double delta = double(0.4f) - double(0.3f);
        const auto run = [&](unsigned workers) {
            tmx::setWorkerCount(workers);
            tmx::StabilityAccumulator accumulator(w, h);
            for (int i = 0; i < 8; ++i) accumulator.addFrame(i % 2 ? odd : even);
            tmx::setWorkerCount(0);
            return accumulator.metrics();
        };
        const tmx::StabilityMetrics m = run(4);
        CHECK(near(m.meanFrameDelta, delta / 2.0, 1e-12));
        for (const double pair : m.pairMeanDelta) CHECK(near(pair, delta / 2.0, 1e-12));
        CHECK(near(m.maxFrameDelta, delta, 1e-12));
        CHECK(near(m.p99FrameDelta, delta, 1.0 / 65535.0));
        // Population std of an even alternation is half the step.
        CHECK(near(m.meanTemporalStdDev, delta / 4.0, 1e-9));
        CHECK(near(m.maxTemporalStdDev, delta / 2.0, 1e-7));
        CHECK(m.flickerPixelCount == w * h / 2);
        CHECK(m.flickerPixelFraction == 0.5);
        CHECK(near(m.flickerEnergy, delta / 2.0, 1e-7));

        // Results do not depend on the worker count.
        const tmx::StabilityMetrics single = run(1);
        CHECK(single.meanFrameDelta == m.meanFrameDelta);
        CHECK(single.p99FrameDelta == m.p99FrameDelta);
        CHECK(single.meanTemporalStdDev == m.meanTemporalStdDev);
        CHECK(single.flickerEnergy == m.flickerEnergy);

        // A mask restricted to the static half sees no flicker.
        const tmx::Mask right = tmx::regionMask(w, h, w / 2, 0, w, h);
        tmx::StabilityAccumulator masked(w, h, &right);
        for (int i = 0; i < 8; ++i) masked.addFrame(i % 2 ? odd : even);
        const tmx::StabilityMetrics staticHalf = masked.metrics();
        CHECK(staticHalf.pixelCount == w * h / 2);
        CHECK(staticHalf.meanFrameDelta == 0.0);
        CHECK(staticHalf.flickerEnergy == 0.0);
        return true;
    }

    // --------------------------------------------------- ghosting / recovery

    bool testTrailMaskAndEnergy() {
        const uint32_t w = 32, h = 32;
        const auto square = [&](uint32_t x0, float value) {
            tmx::Plane plane = constantPlane(w, h, 0.0f);
            for (uint32_t y = 8; y < 12; ++y) {
                for (uint32_t x = x0; x < x0 + 4; ++x) plane.values[size_t(y) * w + x] = value;
            }
            return plane;
        };
        const tmx::Plane past = square(4, 0.5f);
        const tmx::Plane now = square(12, 0.5f);
        const tmx::Mask trail = tmx::trailMask(now, past, 0.02);
        CHECK(trail.area() == 32);
        // The test frame keeps a 0.25 ghost where the object was.
        tmx::Plane test = now;
        for (uint32_t y = 8; y < 12; ++y) {
            for (uint32_t x = 4; x < 8; ++x) test.values[size_t(y) * w + x] = 0.25f;
        }
        const tmx::LumaErrorStats inTrail = tmx::lumaError(now, test, &trail);
        CHECK(inTrail.pixelCount == 32);
        CHECK(near(inTrail.sumAbs, 16 * 0.25, 1e-12));
        CHECK(near(inTrail.meanAbs, 0.125, 1e-12));
        const tmx::LumaErrorStats full = tmx::lumaError(now, test);
        CHECK(near(full.meanAbs, 16 * 0.25 / (w * h), 1e-12));
        CHECK(near(full.maxAbs, 0.25, 1e-12));
        CHECK(tmx::lumaError(now, now).meanAbs == 0.0);
        return true;
    }

    bool testRecovery() {
        const std::vector<double> errors{ 0.5, 0.4, 0.2, 0.05, 0.01, 0.012, 0.011, 0.01 };
        const tmx::RecoveryResult automatic = tmx::evaluateRecovery(errors);
        CHECK(automatic.thresholdFromSteadyState);
        CHECK(automatic.steadyStateFrameCount == 2);
        CHECK(near(automatic.steadyStateMedian, 0.0105, 1e-15));
        CHECK(near(automatic.threshold, 0.021, 1e-15));
        CHECK(automatic.firstBelowFrame == 4u);
        CHECK(automatic.settledFrame == 4u);

        const tmx::RecoveryResult absolute = tmx::evaluateRecovery(errors, 0.3);
        CHECK(!absolute.thresholdFromSteadyState);
        CHECK(absolute.firstBelowFrame == 2u);

        const std::vector<double> bounce{ 0.5, 0.01, 0.5, 0.01, 0.01, 0.01, 0.01, 0.01 };
        const tmx::RecoveryResult bounced = tmx::evaluateRecovery(bounce, 0.1);
        CHECK(bounced.firstBelowFrame == 1u);
        CHECK(bounced.settledFrame == 3u);

        const std::vector<double> never{ 0.5, 0.5, 0.6 };
        const tmx::RecoveryResult unrecovered = tmx::evaluateRecovery(never, 0.1);
        CHECK(!unrecovered.firstBelowFrame);
        CHECK(!unrecovered.settledFrame);
        return true;
    }

    // ------------------------------------------------------------------ luma

    bool testToneMappedLuma() {
        tmx::FloatImage image = constantImage(4, 1, 1.0f, 1.0f, 1.0f);
        image.rgb[3] = image.rgb[4] = image.rgb[5] = std::numeric_limits<float>::quiet_NaN();
        image.rgb[6] = image.rgb[7] = image.rgb[8] = std::numeric_limits<float>::infinity();
        image.rgb[9] = image.rgb[10] = image.rgb[11] = -1.0f;
        const tmx::Plane luma = tmx::toneMappedLuma(image);
        CHECK(near(luma.values[0], 0.5, 1e-6));
        CHECK(luma.values[1] == 0.0f);
        CHECK(luma.values[2] == 1.0f);
        CHECK(luma.values[3] == 0.0f);
        CHECK(luma.nonFiniteCount == 1);
        CHECK(near(tmx::toneMappedLuma(constantImage(1, 1, 1, 1, 1), 2.0).values[0],
            2.0 / 3.0, 1e-6));
        // AP1 weights: pure red has Y = 0.2722287.
        const double y = 0.2722287;
        CHECK(near(tmx::toneMappedLuma(constantImage(1, 1, 1, 0, 0)).values[0],
            y / (1.0 + y), 1e-6));
        const tmx::Plane sdr = tmx::encodedLuma(constantRgb8(2, 2, 255));
        CHECK(near(sdr.values[0], 1.0, 1e-6));
        return true;
    }

    // ------------------------------------------------------------------- I/O

    [[nodiscard]] Iridium::FrameCapture sceneCapture(uint32_t w, uint32_t h) {
        Iridium::FrameCapture capture{};
        capture.width = w;
        capture.height = h;
        capture.rowPitchBytes = w * 16 + 32; // padded rows
        capture.pixelFormat = Iridium::FrameCapturePixelFormat::Rgba32Float;
        capture.colorDomain = Iridium::FrameCaptureColorDomain::SceneLinearAcesCg;
        capture.pixels.assign(size_t(capture.rowPitchBytes) * h, std::byte{ 0xcd });
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const std::array<float, 4> rgba{ float(y * 100 + x), float(x) * 0.25f,
                    -float(y) - 0.5f, 7.0f };
                std::memcpy(capture.pixels.data() + size_t(y) * capture.rowPitchBytes +
                    size_t(x) * 16, rgba.data(), sizeof(rgba));
            }
        }
        return capture;
    }

    [[nodiscard]] std::vector<char> fileBytes(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    bool testPfmRoundTripMatchesEngine() {
        const uint32_t w = 7, h = 5;
        const Iridium::FrameCapture capture = sceneCapture(w, h);
        const std::filesystem::path enginePath = scratch("engine.pfm");
        Iridium::writeFrameCapturePfm(enginePath, capture);

        // The tool reads the engine's bottom-up file into top-left rows.
        const tmx::FloatImage image = tmx::readPfm(enginePath);
        CHECK(image.width == w && image.height == h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float* rgb = image.rgb.data() + (size_t(y) * w + x) * 3;
                CHECK(rgb[0] == float(y * 100 + x));
                CHECK(rgb[1] == float(x) * 0.25f);
                CHECK(rgb[2] == -float(y) - 0.5f);
            }
        }
        // First stored raster row is the bottom image row (y = h - 1).
        const std::vector<char> engineBytes = fileBytes(enginePath);
        const std::string header = "PF\n7 5\n-1.0\n";
        CHECK(engineBytes.size() == header.size() + size_t(w) * h * 12);
        CHECK(std::string(engineBytes.data(), header.size()) == header);
        float firstStored = 0.0f;
        std::memcpy(&firstStored, engineBytes.data() + header.size(), sizeof(float));
        CHECK(firstStored == float((h - 1) * 100));

        // The tool's writer is byte-identical to the engine's and the engine reads it.
        const std::filesystem::path toolPath = scratch("tool.pfm");
        tmx::writePfm(toolPath, image);
        CHECK(fileBytes(toolPath) == engineBytes);
        const Iridium::PfmImage engineRead = Iridium::readPfm(toolPath);
        CHECK(engineRead.rgb32f == image.rgb);

        // Grayscale PFM replicates; big-endian (positive scale) is byte-swapped.
        {
            std::ofstream output(scratch("gray_be.pfm"), std::ios::binary);
            output << "Pf\n2 1\n1.0\n";
            const unsigned char one[4] = { 0x3f, 0x80, 0x00, 0x00 }; // 1.0f big-endian
            const unsigned char two[4] = { 0x40, 0x00, 0x00, 0x00 }; // 2.0f
            output.write(reinterpret_cast<const char*>(one), 4);
            output.write(reinterpret_cast<const char*>(two), 4);
        }
        const tmx::FloatImage gray = tmx::readPfm(scratch("gray_be.pfm"));
        CHECK(gray.rgb == (std::vector<float>{ 1, 1, 1, 2, 2, 2 }));

        bool threw = false;
        {
            std::ofstream output(scratch("short.pfm"), std::ios::binary);
            output << "PF\n2 2\n-1.0\n0123";
        }
        try { (void)tmx::readPfm(scratch("short.pfm")); }
        catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        return true;
    }

    bool testTgaRoundTripMatchesEngine() {
        const uint32_t w = 5, h = 3;
        Iridium::FrameCapture capture{};
        capture.width = w;
        capture.height = h;
        capture.rowPitchBytes = w * 4;
        capture.pixelFormat = Iridium::FrameCapturePixelFormat::Rgba8Srgb;
        capture.colorDomain = Iridium::FrameCaptureColorDomain::DisplayEncodedSdr;
        capture.pixels.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                std::byte* p = capture.pixels.data() + (size_t(y) * w + x) * 4;
                p[0] = std::byte(x * 40);       // R
                p[1] = std::byte(y * 70);       // G
                p[2] = std::byte(200 - x - y);  // B
                p[3] = std::byte(255);
            }
        }
        const std::filesystem::path enginePath = scratch("engine.tga");
        Iridium::writeFrameCaptureTga(enginePath, capture);
        const tmx::Rgb8Image image = tmx::readTga(enginePath);
        CHECK(image.width == w && image.height == h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* rgb = image.rgb.data() + (size_t(y) * w + x) * 3;
                CHECK(rgb[0] == x * 40 && rgb[1] == y * 70 && rgb[2] == 200 - x - y);
            }
        }
        const std::filesystem::path toolPath = scratch("tool.tga");
        tmx::writeTga(toolPath, image);
        CHECK(fileBytes(toolPath) == fileBytes(enginePath));
        const Iridium::TgaImage engineRead = Iridium::readTga(toolPath);
        CHECK(engineRead.width == w && engineRead.height == h);

        // Bottom-left 24-bit TGA is flipped into top-left rows.
        {
            std::ofstream output(scratch("bottom.tga"), std::ios::binary);
            unsigned char header[18]{};
            header[2] = 2;
            header[12] = 1;
            header[14] = 2;
            header[16] = 24;
            header[17] = 0;
            output.write(reinterpret_cast<const char*>(header), 18);
            const unsigned char rows[6] = { 1, 2, 3, 4, 5, 6 }; // bottom row first, BGR
            output.write(reinterpret_cast<const char*>(rows), 6);
        }
        const tmx::Rgb8Image bottom = tmx::readTga(scratch("bottom.tga"));
        CHECK(bottom.rgb == (std::vector<uint8_t>{ 6, 5, 4, 3, 2, 1 }));

        // Masks and heatmaps.
        const tmx::Mask mask = tmx::readMask(enginePath);
        CHECK(mask.width == w && mask.include[0] == 1); // B = 200 > 127
        tmx::Plane heat = constantPlane(2, 1, 0.0f);
        heat.values[1] = 0.5f;
        tmx::writeHeatmap(scratch("heat.tga"), heat, 2.0);
        const tmx::Rgb8Image heatImage = tmx::readTga(scratch("heat.tga"));
        CHECK(heatImage.rgb == (std::vector<uint8_t>{ 0, 0, 0, 255, 255, 255 }));
        return true;
    }

    bool testAccumulateMean() {
        tmx::MeanAccumulator accumulator;
        accumulator.add(constantImage(3, 2, 1, 2, 3));
        accumulator.add(constantImage(3, 2, 2, 4, 6));
        accumulator.add(constantImage(3, 2, 3, 6, 9));
        const tmx::FloatImage mean = accumulator.mean();
        CHECK(accumulator.count() == 3);
        for (size_t p = 0; p < 6; ++p) {
            CHECK(mean.rgb[p * 3] == 2.0f && mean.rgb[p * 3 + 1] == 4.0f &&
                mean.rgb[p * 3 + 2] == 6.0f);
        }
        tmx::writePfm(scratch("mean.pfm"), mean);
        CHECK(tmx::readPfm(scratch("mean.pfm")).rgb == mean.rgb);
        bool threw = false;
        try { accumulator.add(constantImage(2, 2, 0, 0, 0)); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        return true;
    }

    bool testDomainsAndPercentiles() {
        CHECK(tmx::domainForPath("a/b.PFM") == tmx::Domain::Scene);
        CHECK(tmx::domainForPath("a/b.tga") == tmx::Domain::Sdr);
        std::vector<double> values{ 5, 1, 4, 2, 3 };
        CHECK(tmx::nearestRankPercentile(values, 0.5) == 3.0);
        CHECK(tmx::median({ 4, 1, 3, 2 }) == 2.5);
        return true;
    }

} // namespace

int main() {
    g_scratch = std::filesystem::temp_directory_path() /
        ("iridium_temporal_metrics_tests_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(g_scratch);

    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[] = {
        { "scene identical images give zero error", testSceneIdenticalIsZero },
        { "scene known exposure ratio", testSceneKnownScale },
        { "scene mask and non-finite exclusion", testSceneMaskAndNonFinite },
        { "sdr identical: infinite PSNR, SSIM 1", testSdrIdenticalIsInfinitePsnr },
        { "sdr known shift and noise", testSdrKnownShiftAndNoise },
        { "gaussian SSIM closed form and mask", testSsimConstantOffsetMatchesFormula },
        { "stability of a static sequence is zero", testStabilityStaticIsZero },
        { "stability of alternating frames", testStabilityAlternatingFrames },
        { "trail mask and trail energy", testTrailMaskAndEnergy },
        { "disocclusion recovery thresholds", testRecovery },
        { "tone-mapped and encoded luma", testToneMappedLuma },
        { "PFM round trip matches the engine writer", testPfmRoundTripMatchesEngine },
        { "TGA round trip matches the engine writer", testTgaRoundTripMatchesEngine },
        { "accumulate mean", testAccumulateMean },
        { "domains and percentiles", testDomainsAndPercentiles },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(g_scratch, ignored);
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
