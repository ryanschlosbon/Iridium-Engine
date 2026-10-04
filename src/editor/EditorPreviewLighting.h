#pragma once

#include "renderer/rhi/IRenderBackend.h"
#include "core/types/AssetGuid.h"
#include <cmath>

namespace Iridium {
    // Editor-document state, never serialized into the authored scene.
    struct EditorPreviewLighting {
        AssetGuid environmentAsset;
        float environmentRotationDegrees = 0.0f;
        float lightingEv = 0.0f;
        float backgroundEv = 0.0f;
        float exposureEv = 0.0f;
        float sunYawDegrees = 210.0f;
        float sunPitchDegrees = 45.0f;
        float sunEv = 0.0f;
        glm::vec3 sunColor{1.0f}; // Linear Rec.709, as explicitly labelled in the UI.
        bool sunEnabled = true;

        [[nodiscard]] EnvironmentLightingSettings environmentSettings() const noexcept {
            return {
                .lightingIntensity = std::exp2(lightingEv),
                .backgroundIntensity = std::exp2(backgroundEv),
                .rotationRadians = environmentRotationDegrees * 0.017453292519943295f,
            };
        }
    };
}
