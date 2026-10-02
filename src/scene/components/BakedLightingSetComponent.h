#pragma once

#include "assets/AssetGuid.h"

#include <string>

namespace Iridium {

    // Renderer-neutral authored assignment. The referenced product owns stable
    // entity/primitive associations and solver provenance; no GPU handles or
    // source paths are retained in the scene.
    struct BakedLightingSetComponent {
        bool enabled = true;
        AssetGuid lightingAssetGuid;
        float diffuseIntensity = 1.0f;
        float specularIntensity = 1.0f;
        bool applyLightmaps = true;
        bool applyProbeVolumes = true;
        bool applyVisibility = true;

        // Transient publication state; never serialized or cooked.
        AssetGuid requestedLightingAssetGuid;
        AssetGuid resolvedLightingAssetGuid;
        std::string publicationDiagnostic;
    };

} // namespace Iridium
