#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "core/types/AssetGuid.h"

namespace Iridium {
    struct ModelAsset;
}

struct MeshComponent {
    static constexpr int32_t MaximumLodLevel = 15;

    struct MaterialOverride {
        Iridium::AssetGuid sourceMaterialGuid;
        Iridium::AssetGuid materialGuid;

        auto operator<=>(const MaterialOverride&) const = default;
    };

    std::shared_ptr<Iridium::ModelAsset> model;
    Iridium::AssetGuid assetGuid;
    bool enabled = true;
    // Finest-to-coarsest authoring ceiling. Zero pins hero content to LOD0;
    // the default permits every level supported by the current GPU-scene ABI.
    int32_t maximumLodLevel = MaximumLodLevel;

    // Requests are consumed by Application outside the editor frame.
    Iridium::AssetGuid requestedAssetGuid;
    std::string requestedAssetSourcePath;
    std::string assetResolutionDiagnostic;
    std::vector<MaterialOverride> materialOverrides;
    std::vector<Iridium::AssetGuid>
        requestedMaterialAssetRoots;

};
