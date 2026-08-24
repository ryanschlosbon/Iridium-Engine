#pragma once

#include "assets/model/ModelProduct.h"
#include "renderer/rhi/Mesh.h"

#include <optional>
#include <vector>

namespace Iridium {

    struct RuntimeModelCpuData {
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::LegacyTwoBucket;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<SubMesh> primitives;
    };

    struct RuntimeModelCpuResult {
        std::optional<RuntimeModelCpuData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    struct RuntimeMaterialBinding {
        AssetGuid materialGuid;
        CompiledTransparencyPolicy transparency;
        MaterialBinding binding;
    };

    struct RuntimeTextureViewBinding {
        AssetGuid materialGuid;
        uint32_t operationIndex = 0;
        AssetGuid textureGuid;
        // Two-channel normal products must request shader-side positive-Z
        // reconstruction through this binding; the texture handle alone does
        // not expose storage format through the backend-neutral material API.
        MaterialTextureBinding binding;
    };

    struct RuntimeMaterialFallbacks {
        MaterialTextureBinding white;
        MaterialTextureBinding normal;
        MaterialTextureBinding linearData;
    };

    struct RuntimeCanonicalMaterial {
        AssetGuid materialGuid;
        CompiledTransparencyPolicy transparency;
        CanonicalMaterialAsset asset;
    };

    struct RuntimeCanonicalMaterialResult {
        std::vector<RuntimeCanonicalMaterial> materials;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return !materials.empty() &&
                !hasCookErrors(diagnostics);
        }
    };

    struct ResolvedRuntimeModelCpuData {
        RuntimeModelCpuData geometry;
        std::vector<MaterialBinding> materials;
    };

    struct ResolvedRuntimeModelCpuResult {
        std::optional<ResolvedRuntimeModelCpuData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    // Converts an already validated cooked CPU product to the current RHI upload
    // layout. It does not parse source, allocate GPU resources, or merge primitives.
    [[nodiscard]] RuntimeModelCpuResult makeRuntimeModelCpuData(
        const CookedModelProductData& product,
        bool validateProduct = true);
    // Resolves stable texture GUID/operation identities into live RHI views,
    // then reconstructs the exact packed M2 material and pipeline contract.
    // It performs no source parsing and no GPU allocation.
    [[nodiscard]] RuntimeCanonicalMaterialResult
        makeRuntimeCanonicalMaterials(
            const CookedModelProductData& product,
            std::span<const RuntimeTextureViewBinding> textureViews,
            const RuntimeMaterialFallbacks& fallbacks,
            bool validateProduct = true);
    [[nodiscard]] ResolvedRuntimeModelCpuResult resolveRuntimeModelMaterials(
        RuntimeModelCpuData geometry,
        std::span<const RuntimeMaterialBinding> bindings);

} // namespace Iridium
