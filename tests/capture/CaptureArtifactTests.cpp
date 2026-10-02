#include "capture/CaptureArtifact.h"
#include "capture/PfmImage.h"
#include "capture/TgaImage.h"
#include "utils/Sha256.h"
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    std::filesystem::path testRoot() {
        return std::filesystem::current_path() / "capture_artifact_test_output";
    }

    FrameCapture makeRgbaCapture() {
        FrameCapture capture{};
        capture.captureId = 7;
        capture.width = 2;
        capture.height = 2;
        capture.rowPitchBytes = 8;
        capture.pixelFormat = FrameCapturePixelFormat::Rgba8Srgb;
        capture.pixels = {
            std::byte{ 255 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 },
            std::byte{ 0 }, std::byte{ 255 }, std::byte{ 0 }, std::byte{ 255 },
            std::byte{ 0 }, std::byte{ 0 }, std::byte{ 255 }, std::byte{ 255 },
            std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 },
        };
        return capture;
    }

    FrameCapture makeSceneLinearCapture() {
        FrameCapture capture{};
        capture.captureId = 8;
        capture.width = 2;
        capture.height = 2;
        capture.rowPitchBytes = 2 * 4 * sizeof(float);
        capture.pixelFormat = FrameCapturePixelFormat::Rgba32Float;
        capture.colorDomain = FrameCaptureColorDomain::SceneLinearAcesCg;
        constexpr std::array<float, 16> pixels = {
            -0.25f, 0.0f, 0.5f, 1.0f,
            1.0f, 2.0f, 16.0f, 1.0f,
            0.125f, 0.25f, 0.375f, 1.0f,
            100.0f, 4.0f, 0.75f, 1.0f,
        };
        capture.pixels.resize(sizeof(pixels));
        std::memcpy(capture.pixels.data(), pixels.data(), sizeof(pixels));
        return capture;
    }

    bool testCanonicalTgaRoundTrip() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        const std::filesystem::path path = root / "roundtrip.tga";
        writeFrameCaptureTga(path, makeRgbaCapture());
        const TgaImage image = readTga(path);
        CHECK(image.width == 2);
        CHECK(image.height == 2);
        CHECK(image.bgra8.size() == 16);
        // The first top-left source pixel was RGBA red and is stored as BGRA.
        CHECK(image.bgra8[0] == std::byte{ 0 });
        CHECK(image.bgra8[1] == std::byte{ 0 });
        CHECK(image.bgra8[2] == std::byte{ 255 });
        CHECK(image.bgra8[3] == std::byte{ 255 });
        // The first pixel of the second row was blue; rows were not flipped.
        CHECK(image.bgra8[8] == std::byte{ 255 });
        CHECK(image.bgra8[9] == std::byte{ 0 });
        CHECK(image.bgra8[10] == std::byte{ 0 });
        std::filesystem::remove_all(root);
        return true;
    }

    bool testArtifactNamingMetadataAndNoOverwrite() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        CaptureArtifactMetadata metadata{};
        metadata.buildConfiguration = "Test";
        metadata.sourceCommit = "abc";
        metadata.sourceBranch = "feature/test";
        metadata.fixtureId = "Material Lab V1";
        metadata.fixtureRevision = 3;
        metadata.cameraId = "Front/V1";
        metadata.manifestPath = "manifest.json";
        metadata.manifestSha256 = std::string(64, '1');
        metadata.contentHashes.emplace_back("scene.gltf", std::string(64, '2'));
        metadata.measuredFrameIndex = 12;
        metadata.applicationFrameIndex = 20;
        metadata.benchmarkStateFrameIndex = 20;
        metadata.warmupFrameCount = 8;
        metadata.directionalShadowOwnerCount = 2;
        metadata.directionalShadowReceiverDepthBiasTexels = 1.25f;
        metadata.directionalShadowReceiverPlaneClampTexels = 2.0f;
        metadata.directionalShadowNormalOffsetTexels = 0.5f;
        metadata.debugView = "Base Color";
        metadata.debugViewSemantics = "opaque base color";

        const FrameCapture capture = makeRgbaCapture();
        const CaptureArtifactPaths paths = writeCaptureArtifact(root, capture, metadata);
        CHECK(paths.image.filename().generic_string() ==
            "material_lab_v1__r3__front_v1__base_color__2x2__mf12.tga");
        CHECK(paths.metadata.filename().generic_string() ==
            "material_lab_v1__r3__front_v1__base_color__2x2__mf12.json");
        CHECK(paths.imageSha256 == sha256File(paths.image));
        CHECK(std::filesystem::file_size(paths.image) == 18 + 16);
        std::filesystem::path temporaryImage = paths.image;
        temporaryImage += ".tmp";
        std::filesystem::path temporaryMetadata = paths.metadata;
        temporaryMetadata += ".tmp";
        CHECK(!std::filesystem::exists(temporaryImage));
        CHECK(!std::filesystem::exists(temporaryMetadata));

        std::ifstream metadataInput(paths.metadata, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(metadataInput)),
            std::istreambuf_iterator<char>());
        CHECK(text.find("legacy_display_referred") != std::string::npos);
        CHECK(text.find(paths.imageSha256) != std::string::npos);
        CHECK(text.find("post_transparency_pre_ui_scene_color") != std::string::npos);
        CHECK(text.find("\"owner_count\": 2") != std::string::npos);
        CHECK(text.find("\"receiver_depth_bias_texels\": 1.25") !=
            std::string::npos);
        CHECK(text.find("\"receiver_plane_clamp_texels\": 2.0") !=
            std::string::npos);
        CHECK(text.find("\"normal_offset_texels\": 0.5") !=
            std::string::npos);
        metadataInput.close();

        bool rejectedOverwrite = false;
        try {
            (void)writeCaptureArtifact(root, capture, metadata);
        }
        catch (const std::runtime_error&) {
            rejectedOverwrite = true;
        }
        CHECK(rejectedOverwrite);
        std::filesystem::remove_all(root);
        return true;
    }

    bool testSceneLinearPfmArtifactRoundTrip() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        CaptureArtifactMetadata metadata{};
        metadata.fixtureId = "HDR Volume";
        metadata.fixtureRevision = 1;
        metadata.cameraId = "Reference";
        metadata.debugView = "Scene Linear";
        const FrameCapture capture = makeSceneLinearCapture();

        const CaptureArtifactPaths paths = writeCaptureArtifact(root, capture, metadata);
        CHECK(paths.image.extension() == ".pfm");
        CHECK(paths.imageSha256 == sha256File(paths.image));
        const PfmImage image = readPfm(paths.image);
        CHECK(image.width == 2);
        CHECK(image.height == 2);
        constexpr std::array<float, 12> expected = {
            -0.25f, 0.0f, 0.5f,
            1.0f, 2.0f, 16.0f,
            0.125f, 0.25f, 0.375f,
            100.0f, 4.0f, 0.75f,
        };
        CHECK(image.rgb32f.size() == expected.size());
        for (size_t index = 0; index < expected.size(); ++index) {
            CHECK(image.rgb32f[index] == expected[index]);
        }

        std::ifstream metadataInput(paths.metadata, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(metadataInput)),
            std::istreambuf_iterator<char>());
        CHECK(text.find("scene_linear_acescg_ap1") != std::string::npos);
        CHECK(text.find("pfm_rgb32f_little_endian") != std::string::npos);
        CHECK(text.find("post_transparency_pre_output_scene_color") !=
            std::string::npos);
        CHECK(text.find("\"output_operator\": \"none\"") != std::string::npos);
        CHECK(text.find("\"exposure\": \"unapplied\"") != std::string::npos);
        metadataInput.close();
        std::filesystem::remove_all(root);
        return true;
    }

    bool testFinalSdrArtifactMetadata() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        CaptureArtifactMetadata metadata{};
        metadata.fixtureId = "Color Volume";
        metadata.fixtureRevision = 1;
        metadata.cameraId = "Reference";
        metadata.debugView = "Final";
        metadata.outputOperator = "aces_fitted_legacy";
        metadata.manualExposureEv = 2.0;
        metadata.gamutMapping = "ap1_to_rec709_matrix_then_clip_negative";
        metadata.displayProfile = "windows_sdr_rec709_srgb";
        metadata.outputTransfer = "iec_61966_2_1_srgb";
        metadata.paperWhiteNits = 100.0;
        metadata.peakNits = 100.0;
        metadata.acesPackageVersion = "v2.0.0+2025.04.04";
        metadata.acesTransformId = "legacy_fitted_compatibility";
        FrameCapture capture = makeRgbaCapture();
        capture.colorDomain = FrameCaptureColorDomain::DisplayEncodedSdr;

        const CaptureArtifactPaths paths = writeCaptureArtifact(root, capture, metadata);
        CHECK(paths.image.filename().generic_string().ends_with("__final-sdr.tga"));
        std::ifstream metadataInput(paths.metadata, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(metadataInput)),
            std::istreambuf_iterator<char>());
        CHECK(text.find("post_output_transform_pre_ui_display_color") !=
            std::string::npos);
        CHECK(text.find("display_encoded_sdr_rec709") != std::string::npos);
        CHECK(text.find("\"exposure\": 2.0") != std::string::npos);
        CHECK(text.find("\"output_operator\": \"aces_fitted_legacy\"") !=
            std::string::npos);
        CHECK(text.find("v2.0.0+2025.04.04") != std::string::npos);
        metadataInput.close();
        std::filesystem::remove_all(root);
        return true;
    }

    bool testFinalHdrArtifactMetadata() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        CaptureArtifactMetadata metadata{};
        metadata.fixtureId = "Color Volume";
        metadata.fixtureRevision = 1;
        metadata.cameraId = "Reference";
        metadata.debugView = "Final";
        metadata.outputOperator = "aces2";
        metadata.gamutMapping = "aces2_jmh_chroma_and_gamut_compression_lut128_tetrahedral";
        metadata.displayProfile = "windows_hdr10_rec2100_pq";
        metadata.outputTransfer = "st2084_pq";
        metadata.paperWhiteNits = 203.0;
        metadata.peakNits = 1000.0;
        metadata.acesPackageVersion = "v2.0.0+2025.04.04";
        metadata.acesTransformId = "hdr-transform";
        FrameCapture capture = makeSceneLinearCapture();
        capture.colorDomain = FrameCaptureColorDomain::DisplayLinearHdr;

        const CaptureArtifactPaths paths = writeCaptureArtifact(root, capture, metadata);
        CHECK(paths.image.filename().generic_string().ends_with("__final-hdr.pfm"));
        CHECK(readPfm(paths.image).rgb32f.size() == 12);
        std::ifstream metadataInput(paths.metadata, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(metadataInput)),
            std::istreambuf_iterator<char>());
        CHECK(text.find("display_linear_hdr_relative_paper_white") !=
            std::string::npos);
        CHECK(text.find("\"primaries\": \"rec2020\"") != std::string::npos);
        CHECK(text.find("\"paper_white_nits\": 203.0") != std::string::npos);
        CHECK(text.find("\"peak_nits\": 1000.0") != std::string::npos);
        metadataInput.close();
        std::filesystem::remove_all(root);
        return true;
    }

    bool testRejectsMalformedTga() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::filesystem::path path = root / "truncated.tga";
        {
            std::ofstream output(path, std::ios::binary);
            constexpr std::array<char, 3> bytes{ 0, 0, 2 };
            output.write(bytes.data(), bytes.size());
        }
        bool rejected = false;
        try {
            (void)readTga(path);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        std::filesystem::remove_all(root);
        return true;
    }

    bool testRejectsNoncanonicalTgaHeader() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        const std::filesystem::path path = root / "noncanonical.tga";
        writeFrameCaptureTga(path, makeRgbaCapture());
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(8);
            const char nonzeroOrigin = 1;
            file.write(&nonzeroOrigin, 1);
        }
        bool rejected = false;
        try {
            (void)readTga(path);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        std::filesystem::remove_all(root);
        return true;
    }

    bool testRejectsUnormArtifactMetadata() {
        const std::filesystem::path root = testRoot();
        std::filesystem::remove_all(root);
        FrameCapture capture = makeRgbaCapture();
        capture.pixelFormat = FrameCapturePixelFormat::Rgba8Unorm;
        CaptureArtifactMetadata metadata{};
        bool rejected = false;
        try {
            (void)writeCaptureArtifact(root, capture, metadata);
        }
        catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!std::filesystem::exists(root));
        return true;
    }

    bool testSignalSanityAndPadding() {
        const auto root = testRoot();
        std::filesystem::remove_all(root);
        CaptureArtifactMetadata metadata{};
        metadata.requireSpatialSignal = true;
        FrameCapture capture = makeSceneLinearCapture();
        auto paths = writeCaptureArtifact(root / "populated", capture, metadata);
        auto readSignal = [](const std::filesystem::path& path) {
            std::ifstream stream(path);
            return nlohmann::json::parse(stream).at("image").at("signal");
        };
        auto signal = readSignal(paths.metadata);
        CHECK(signal["has_finite_spatial_signal"] == true);
        CHECK(signal["finite_rgb_pixels"] == 4);
        CHECK(signal["nonzero_finite_rgb_pixels"] == 4);
        CHECK(signal["finite_pixel_rgb_min"][0] == -0.25);
        CHECK(signal["finite_pixel_rgb_max"][0] == 100.0);
        CHECK(signal["maximum_spatial_channel_range"] == 100.25);

        auto rejectedWithoutArtifacts = [&](const std::string& name) {
            const auto directory = root / name;
            try { (void)writeCaptureArtifact(directory, capture, metadata); }
            catch (const std::runtime_error&) {
                return !std::filesystem::exists(directory) || std::filesystem::is_empty(directory);
            }
            return false;
        };
        // Padding is nonzero, and alpha varies, but all RGB values are black.
        capture.rowPitchBytes += 16;
        capture.pixels.assign(size_t(capture.rowPitchBytes) * capture.height, std::byte{ 255 });
        for (uint32_t y = 0; y < capture.height; ++y) {
            for (uint32_t x = 0; x < capture.width; ++x) {
                const std::array<float, 4> rgba{ 0.0f, -0.0f, 0.0f, float(x + y) };
                std::memcpy(capture.pixels.data() + size_t(y) * capture.rowPitchBytes + size_t(x) * 16,
                    rgba.data(), sizeof(rgba));
            }
        }
        CHECK(rejectedWithoutArtifacts("black"));
        metadata.requireSpatialSignal = false;
        paths = writeCaptureArtifact(root / "intentional-black", capture, metadata);
        signal = readSignal(paths.metadata);
        CHECK(signal["has_finite_spatial_signal"] == false);
        CHECK(signal["finite_rgb_pixels"] == 4);
        CHECK(signal["nonzero_finite_rgb_pixels"] == 0);
        CHECK(signal["maximum_spatial_channel_range"] == 0.0);

        metadata.requireSpatialSignal = true;
        capture = makeSceneLinearCapture();
        const float bad = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(capture.pixels.data(), &bad, sizeof(bad));
        CHECK(rejectedWithoutArtifacts("nonfinite"));
        metadata.requireSpatialSignal = false;
        paths = writeCaptureArtifact(root / "diagnostic-nonfinite", capture, metadata);
        signal = readSignal(paths.metadata);
        CHECK(signal["finite_rgb_pixels"] == 3);
        CHECK(signal["has_finite_spatial_signal"] == false);

        metadata.requireSpatialSignal = true;
        capture = makeRgbaCapture();
        capture.pixelFormat = FrameCapturePixelFormat::Bgra8Srgb;
        capture.rowPitchBytes = 12;
        capture.pixels.assign(24, std::byte{ 255 });
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                const std::array<std::byte, 4> pixel{
                    std::byte{ 32 }, std::byte{ 16 }, std::byte{ 8 }, std::byte(x + y) };
                std::memcpy(capture.pixels.data() + y * 12 + x * 4, pixel.data(), pixel.size());
            }
        }
        CHECK(rejectedWithoutArtifacts("constant-color"));
        capture.pixels[0] = std::byte{ 64 }; // Blue only, not red, changes.
        paths = writeCaptureArtifact(root / "bgra-padding", capture, metadata);
        signal = readSignal(paths.metadata);
        CHECK(signal["has_finite_spatial_signal"] == true);
        CHECK(signal["finite_pixel_rgb_max"][0] == 8.0 / 255.0);
        CHECK(signal["finite_pixel_rgb_min"][2] == 32.0 / 255.0);
        CHECK(signal["finite_pixel_rgb_max"][2] == 64.0 / 255.0);
        std::filesystem::remove_all(root);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "Canonical TGA round trip", testCanonicalTgaRoundTrip },
        { "Artifact naming, metadata, and no-overwrite", testArtifactNamingMetadataAndNoOverwrite },
        { "Scene-linear PFM artifact round trip", testSceneLinearPfmArtifactRoundTrip },
        { "Final SDR artifact metadata", testFinalSdrArtifactMetadata },
        { "Final HDR artifact metadata", testFinalHdrArtifactMetadata },
        { "Malformed TGA rejection", testRejectsMalformedTga },
        { "Noncanonical TGA header rejection", testRejectsNoncanonicalTgaHeader },
        { "UNORM artifact metadata rejection", testRejectsUnormArtifactMetadata },
        { "RGB signal sanity, nonfinite rejection, and padding", testSignalSanityAndPadding },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else ++failures;
        }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::filesystem::remove_all(testRoot());
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
