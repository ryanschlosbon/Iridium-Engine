#pragma once

#include "assets/AssetGuid.h"

#include <cstdint>
#include <string>

namespace Iridium {

    enum class SkyMode : int32_t {
        Skybox = 0,
        Hdri = 1,
        Simulated = 2,
    };

    // Mode-specific data deliberately remains separate. Skybox and simulated
    // atmosphere can grow without reinterpreting the HDRI authoring contract.
    struct SkyboxSkySettings {
        AssetGuid cubemapAssetGuid;
        float intensity = 1.0f;
        float rotationDegrees = 0.0f;
        bool visibleToCamera = true;
    };

    struct HdriSkySettings {
        AssetGuid environmentAssetGuid;
        float lightingIntensity = 1.0f;
        float backgroundIntensity = 1.0f;
        float rotationDegrees = 0.0f;
        bool visibleToCamera = true;
        bool affectsLighting = true;
    };

    struct SimulatedSkySettings {
        float turbidity = 2.0f;
        float ozone = 0.35f;
        float groundAlbedo = 0.30f;
        float atmosphereHeightKilometers = 100.0f;
        bool sunDisk = true;
        bool aerialPerspective = true;
    };

    struct SkyComponent {
        bool enabled = true;
        SkyMode mode = SkyMode::Hdri;
        SkyboxSkySettings skybox;
        HdriSkySettings hdri;
        SimulatedSkySettings simulated;
        int32_t priority = 0;

        // Transient editor/runtime state; never serialized or cooked.
        AssetGuid resolvedEnvironmentAssetGuid;
        AssetGuid requestedEnvironmentAssetGuid;
        std::string requestedAssetSourcePath;
        std::string assetResolutionDiagnostic;
    };

} // namespace Iridium
