#pragma once

#include "renderer/rhi/FrameCapture.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Iridium {

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
        uint32_t directionalShadowBlockerSearchSamples = 0;
        uint32_t directionalShadowFilterSamples = 0;
        std::vector<std::pair<std::string, std::string>> contentHashes;
        uint64_t measuredFrameIndex = 0;
        uint64_t applicationFrameIndex = 0;
        uint64_t benchmarkStateFrameIndex = 0;
        uint64_t warmupFrameCount = 0;
        std::string debugView;
        std::string debugViewSemantics;
        std::vector<std::string> unavailableFields;
    };

    struct CaptureArtifactPaths {
        std::filesystem::path image;
        std::filesystem::path metadata;
        std::string imageSha256;
    };

    [[nodiscard]] std::string makeCaptureArtifactStem(
        const CaptureArtifactMetadata& metadata, uint32_t width, uint32_t height);
    [[nodiscard]] CaptureArtifactPaths writeCaptureArtifact(
        const std::filesystem::path& directory, const FrameCapture& capture,
        const CaptureArtifactMetadata& metadata);

} // namespace Iridium
