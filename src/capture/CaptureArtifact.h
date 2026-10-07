#pragma once

#include "core/types/FrameCapture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Iridium {

    // M9 G6c: the captured frame's sub-pixel jitter, from that frame's view
    // record (render_configuration.temporal_jitter).
    struct CaptureTemporalJitter {
        bool enabled = false;
        uint32_t sequenceLength = 0;
        uint32_t sequenceIndex = 0;
        // ndc * extent / 2.
        std::array<double, 2> offsetPixels{};
        std::array<double, 2> offsetNdc{};
        uint64_t turnsSinceCut = 0;
        bool historyReset = false;
    };

    struct CaptureArtifactMetadata {
        std::string buildConfiguration;
        std::string sourceCommit;
        std::string sourceBranch;
        bool sourceDirtyAtConfigure = false;
        bool validationEnabled = false;
        bool cpuProfilingEnabled = false;
        bool gpuProfilingRequested = false;
        bool gpuProfilingAvailable = false;
        bool windowVisible = true;
        bool windowDecorated = true;
        // Optional fixture sanity gate, not a substitute for image comparison.
        // Intentional black/constant diagnostic captures remain valid by default.
        bool requireSpatialSignal = false;
        std::string compiler;
        std::string shaderCompiler;
        std::string operatingSystem;
        std::string cpuName;
        uint64_t systemMemoryBytes = 0;
        std::string gpuName;
        std::string gpuUuid;
        std::string gpuDriver;
        std::string vulkanDeviceApiVersion;
        std::string vulkanLoaderApiVersion;
        std::string vulkanSdkVersion;
        std::vector<std::string> applicationEnabledLayers;
        std::vector<std::string> activeTools;
        std::string swapchainFormat;
        std::string swapchainColorSpace;
        std::string presentMode;
        std::string outputMode;
        std::string reconstructionMode;
        std::string qualitySettings;
        std::string cacheState;
        std::string outputOperator;
        double manualExposureEv = 0.0;
        std::string gamutMapping;
        std::string displayProfile;
        std::string outputTransfer;
        double paperWhiteNits = 100.0;
        double peakNits = 100.0;
        std::string acesPackageVersion;
        std::string acesTransformId;
        std::string fixtureId;
        uint32_t fixtureRevision = 0;
        std::string cameraId;
        std::string manifestPath;
        std::string manifestSha256;
        std::string modelLoadMode;
        std::string modelLocation;
        std::string modelAssetGuid;
        std::string modelArtifactCookKey;
        std::string environmentLoadMode;
        std::string environmentLocation;
        std::string environmentAssetGuid;
        std::string environmentArtifactCookKey;
        std::string environmentSourceTextureGuid;
        std::string environmentSourcePrimaries;
        double environmentRadianceScale = 0.0;
        bool directionalShadowActive = false;
        uint32_t directionalShadowOwnerCount = 0;
        std::string directionalShadowOwner;
        uint32_t directionalShadowLightSlot = 0;
        uint32_t directionalShadowResolution = 0;
        uint32_t directionalShadowCascadeCount = 0;
        uint32_t directionalShadowSampleableMask = 0;
        uint32_t omittedShadowDirectionalLights = 0;
        std::string directionalShadowFormat;
        std::string directionalShadowFilter;
        float directionalShadowSourceAngularDiameterDegrees = 0.0f;
        float directionalShadowMaximumPenumbraTexels = 0.0f;
        float directionalShadowReceiverDepthBiasTexels = 0.0f;
        float directionalShadowReceiverPlaneClampTexels = 0.0f;
        float directionalShadowNormalOffsetTexels = 0.0f;
        uint32_t directionalShadowBlockerSearchSamples = 0;
        uint32_t directionalShadowFilterSamples = 0;
        std::vector<std::pair<std::string, std::string>> contentHashes;
        uint64_t measuredFrameIndex = 0;
        uint64_t applicationFrameIndex = 0;
        uint64_t benchmarkStateFrameIndex = 0;
        uint64_t warmupFrameCount = 0;
        std::string debugView;
        std::string debugViewSemantics;
        // Null in the sidecar when the frame's view record was not observed.
        std::optional<CaptureTemporalJitter> temporalJitter;
        std::vector<std::string> unavailableFields;
    };

    struct CaptureArtifactPaths {
        std::filesystem::path image;
        std::filesystem::path metadata;
        std::string imageSha256;
    };

    // RGB signal statistics of a written image (image.signal in the sidecar).
    struct CaptureSignalSummary {
        bool floating = false;
        uint64_t pixelCount = 0;
        uint64_t finitePixels = 0;
        uint64_t nonzeroPixels = 0;
        std::array<double, 3> minimum{};
        std::array<double, 3> maximum{};
        double maximumRange = 0.0;
    };

    // An image written under its temporary name, not yet committed: the
    // capture's pixels are no longer needed (M9 G6c capture sequences stream
    // each image to disk as its readback completes and commit the sidecars
    // at the end of the run).
    struct PendingCaptureImage {
        CaptureArtifactPaths paths;   // final names; imageSha256 is set
        std::filesystem::path temporaryImage;
        std::filesystem::path temporaryMetadata;
        std::string stem;
        uint64_t captureId = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        FrameCapturePixelFormat pixelFormat = FrameCapturePixelFormat::Rgba8Srgb;
        FrameCaptureColorDomain colorDomain =
            FrameCaptureColorDomain::LegacyDisplayReferred;
        CaptureSignalSummary signal{};
    };

    [[nodiscard]] std::string makeCaptureArtifactStem(
        const CaptureArtifactMetadata& metadata, uint32_t width, uint32_t height);
    // writeCaptureImage then commitCaptureArtifact (discarding on failure).
    [[nodiscard]] CaptureArtifactPaths writeCaptureArtifact(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata);
    // Writes the image under a temporary name and checks its signal. The
    // stem comes from the naming fields of `metadata` (fixture, revision,
    // camera, debug view, measured frame) and the requireSpatialSignal gate
    // applies; nothing else of `metadata` is read.
    [[nodiscard]] PendingCaptureImage writeCaptureImage(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata);
    // Writes the sidecar and renames both files to their final names.
    // `metadata` must name the same stem. On failure nothing is committed and
    // the temporary image is removed.
    [[nodiscard]] CaptureArtifactPaths commitCaptureArtifact(
        const PendingCaptureImage& image, const CaptureArtifactMetadata& metadata);
    // Removes an uncommitted image (failure paths).
    void discardCaptureImage(const PendingCaptureImage& image) noexcept;

} // namespace Iridium
