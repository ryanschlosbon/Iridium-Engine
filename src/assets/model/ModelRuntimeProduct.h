#pragma once

#include "assets/model/ModelProduct.h"
#include "renderer/rhi/GeometryArena.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/Mesh.h"

#include <optional>
#include <vector>

namespace Iridium {

    struct RuntimeModelCpuData {
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        std::vector<Vertex> vertices;
        // M7.3 losslessly derives split local-index streams from the canonical
        // cooked data. The legacy UInt32 vector remains until every direct
        // geometry uploader has migrated to the arena allocation contract.
        GeometryArenaData geometryArena;
        std::vector<uint32_t> indices;
        std::vector<SubMesh> primitives;
        std::vector<ModelLodChain> lodChains;
    };

    struct RuntimeModelCpuResult {
        std::optional<RuntimeModelCpuData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    struct RuntimeModelLodResidencyStats {
        uint32_t requestedMinimumLevel = 0;
        uint32_t maximumAppliedLevel = 0;
        uint32_t fallbackChainCount = 0;
        uint32_t withheldPrimitiveRangeCount = 0;
        uint64_t originalIndexBytes = 0;
        uint64_t residentIndexBytes = 0;

        [[nodiscard]] uint64_t withheldIndexBytes() const noexcept {
            return originalIndexBytes - residentIndexBytes;
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
        bool validateProduct = true,
        std::optional<TransparencyExecutionMode> executionModeOverride =
            std::nullopt);
    // Qualification seam for M7.5: physically omits fine index ranges from the
    // GPU arena while rebinding each canonical primitive identity to its first
    // retained coarser range. Vertex data remains parent-contained until M7.9
    // externalizes independently streamable child products.
    [[nodiscard]] RuntimeModelLodResidencyStats
        applyRuntimeModelLodResidencyFloor(
            RuntimeModelCpuData& model, uint32_t minimumLodLevel);
    // Resolves stable texture GUID/operation identities into live RHI views,
    // then reconstructs the exact packed M2 material and pipeline contract.
    // It performs no source parsing and no GPU allocation.
    [[nodiscard]] RuntimeCanonicalMaterialResult
        makeRuntimeCanonicalMaterials(
            const CookedModelProductData& product,
            std::span<const RuntimeTextureViewBinding> textureViews,
            const RuntimeMaterialFallbacks& fallbacks,
            bool validateProduct = true,
            std::optional<TransparencyExecutionMode> executionModeOverride =
                std::nullopt);
    [[nodiscard]] ResolvedRuntimeModelCpuResult resolveRuntimeModelMaterials(
        RuntimeModelCpuData geometry,
        std::span<const RuntimeMaterialBinding> bindings);

} // namespace Iridium
