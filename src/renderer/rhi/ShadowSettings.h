#pragma once

#include <algorithm>
#include <cstdint>

namespace Iridium {

    enum class ShadowFilterMode : uint32_t {
        FixedPcf = 0,
        ContactHardeningPcss = 1,
    };

    enum class ShadowQualityProfile : uint32_t {
        Low = 0,
        Medium = 1,
        High = 2,
        Ultra = 3,
        Cinematic = 4,
    };

    struct ShadowFilterProfile {
        uint32_t blockerSearchSamples = 0;
        uint32_t filterSamples = 25;
        float maximumPenumbraTexels = 2.0f;
        bool contactHardening = false;
    };

    inline constexpr uint32_t kDirectionalShadowCascadeCount = 4;
    inline constexpr uint32_t kDirectionalShadowLightCapacity = 2;
    inline constexpr uint32_t kDirectionalShadowLayerCount =
        kDirectionalShadowCascadeCount * kDirectionalShadowLightCapacity;
    inline constexpr uint32_t kSpotShadowEntryCapacity = 256;
    inline constexpr uint32_t kPointShadowPool256Capacity = 32;
    inline constexpr uint32_t kPointShadowPool512Capacity = 16;
    inline constexpr uint32_t kPointShadowPool1024Capacity = 8;
    inline constexpr uint32_t kPointShadowEntryCapacity =
        kPointShadowPool256Capacity + kPointShadowPool512Capacity +
        kPointShadowPool1024Capacity;

    // Backend-neutral project policy. The editor may own and persist this
    // structure without depending on ImGui, GLM, or Vulkan objects.
    struct ProjectShadowSettings {
        ShadowFilterMode filterMode = ShadowFilterMode::ContactHardeningPcss;
        ShadowQualityProfile qualityProfile = ShadowQualityProfile::Ultra;
        // Apparent diameter of the directional emitter. The default matches
        // the mean apparent diameter of the sun as seen from Earth.
        float directionalSourceAngularDiameterDegrees = 0.535f;
        float maximumPenumbraTexels = 48.0f;
        uint32_t directionalResolution = 4096;
        uint32_t maximumDirectionalLights =
            kDirectionalShadowLightCapacity;
        uint32_t maximumCascadeUpdatesPerLight =
            kDirectionalShadowCascadeCount;
        float directionalSplitLambda = 0.85f;
        float directionalGuardBandFraction = 0.05f;
        float directionalDepthPaddingMeters = 100.0f;
        uint32_t spotAtlasResolution = 8192;
        uint64_t maximumSpotRenderedTexelsPerFrame =
            16ull * 1024ull * 1024ull;
        uint32_t maximumCompatibleSpotStaleFrames = 2;
        uint32_t pointPool256Capacity = kPointShadowPool256Capacity;
        uint32_t pointPool512Capacity = kPointShadowPool512Capacity;
        uint32_t pointPool1024Capacity = kPointShadowPool1024Capacity;
        uint64_t maximumPointRenderedTexelsPerFrame =
            12ull * 1024ull * 1024ull;
        uint32_t maximumCompatiblePointStaleFrames = 2;
    };

    [[nodiscard]] constexpr ShadowFilterProfile shadowFilterProfile(
        ShadowQualityProfile quality) noexcept {
        switch (quality) {
            case ShadowQualityProfile::Low:
                return { 0, 9, 2.0f, false };
            case ShadowQualityProfile::Medium:
                return { 8, 16, 12.0f, true };
            case ShadowQualityProfile::High:
                return { 12, 24, 24.0f, true };
            case ShadowQualityProfile::Ultra:
                return { 24, 48, 48.0f, true };
            case ShadowQualityProfile::Cinematic:
                return { 32, 64, 64.0f, true };
        }
        return { 0, 25, 2.0f, false };
    }

    [[nodiscard]] constexpr ShadowFilterProfile effectiveShadowFilterProfile(
        const ProjectShadowSettings& settings,
        uint32_t lightQuality) noexcept {
        const uint32_t projectQuality = static_cast<uint32_t>(
            settings.qualityProfile);
        const ShadowQualityProfile effectiveQuality =
            static_cast<ShadowQualityProfile>((std::min)(projectQuality,
                (std::min)(lightQuality,
                    static_cast<uint32_t>(ShadowQualityProfile::Ultra))));
        ShadowFilterProfile profile = shadowFilterProfile(effectiveQuality);
        profile.maximumPenumbraTexels = (std::min)(
            profile.maximumPenumbraTexels,
            settings.maximumPenumbraTexels);
        if (settings.filterMode == ShadowFilterMode::FixedPcf) {
            profile.blockerSearchSamples = 0;
            profile.filterSamples = 25;
            profile.maximumPenumbraTexels = 2.0f;
            profile.contactHardening = false;
        }
        return profile;
    }

} // namespace Iridium
