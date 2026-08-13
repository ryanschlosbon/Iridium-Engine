#pragma once
#include <memory>
#include <vector>
#include <string>
#include "assets/AssetGuid.h"

namespace Iridium {
    struct ModelAsset;
}

struct MeshComponent {
    struct MaterialOverride {
        Iridium::AssetGuid sourceMaterialGuid;
        Iridium::AssetGuid materialGuid;

        auto operator<=>(const MaterialOverride&) const = default;
    };

    std::shared_ptr<Iridium::ModelAsset> model;
    Iridium::AssetGuid assetGuid;
    bool enabled = true;

    // Requests are consumed by Application outside the editor frame.
    Iridium::AssetGuid requestedAssetGuid;
    std::string requestedAssetSourcePath;
    std::string assetResolutionDiagnostic;
    std::vector<MaterialOverride> materialOverrides;
    std::vector<Iridium::AssetGuid>
        requestedMaterialAssetRoots;

};
