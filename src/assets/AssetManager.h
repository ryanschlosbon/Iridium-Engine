#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <vector>
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/IRenderBackend.h"
#include "assets/cooker/CookedArtifact.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/model/ModelRuntimeProduct.h"
#include "assets/MaterialProvenance.h"
#include "material/MaterialTextureCompatibility.h"

namespace Iridium {

    struct LoadedEnvironmentAsset {
        EnvironmentLightingHandles lighting;
        AssetGuid assetGuid;
        std::string cookKey;
        CookedEnvironmentManifest manifest;
        // The small shared split-sum product is retained so an explicit
        // scene-probe bake can emit a complete versioned environment artifact
        // without reading a renderer-owned texture back from the GPU.
        std::vector<std::byte> brdfLut;
    };

    class AssetManager {
    public:
        // The AssetManager now only takes a pointer to the abstract interface.
        explicit AssetManager(IRenderBackend* renderBackend);
        ~AssetManager();

        std::shared_ptr<ModelAsset> loadModelFromCookedArtifact(
            const CookedArtifact& artifact,
            std::span<const RuntimeMaterialBinding> materials);
        std::shared_ptr<ModelAsset>
            loadCompleteModelFromCookedArtifact(
                const CookedArtifact& artifact,
                std::span<const RuntimeTextureViewBinding>
                    textureViews,
                const RuntimeMaterialFallbacks& fallbacks);
        std::shared_ptr<ModelAsset>
            loadSelfContainedModelFromCookedArtifact(
                const CookedArtifact& artifact);
        std::shared_ptr<ModelAsset>
            loadSelfContainedModelFromCookedArtifactFile(
                const std::filesystem::path& path);
        std::shared_ptr<ModelAsset>
            replaceSelfContainedModelFromCookedArtifact(
                const CookedArtifact& artifact);
        std::shared_ptr<ModelAsset>
            replaceSelfContainedModelFromCookedProduct(
                const CookedArtifact& artifact,
                const CookedModelProductData& product);
        std::shared_ptr<ModelAsset>
            replaceSelfContainedModelFromCookedArtifactFile(
                const std::filesystem::path& path);
        [[nodiscard]] LoadedEnvironmentAsset
            loadEnvironmentFromCookedArtifact(
                const CookedArtifact& artifact);
        [[nodiscard]] LoadedEnvironmentAsset
            loadEnvironmentFromCookedArtifactFile(
                const std::filesystem::path& path);
        void releaseEnvironment(EnvironmentLightingHandles lighting);
        [[nodiscard]] std::shared_ptr<ModelAsset>
            findCookedModel(AssetGuid assetGuid) const;
        [[nodiscard]] std::optional<MaterialBinding>
            findCookedMaterial(
                AssetGuid materialGuid) const;

        // Instead of returning a Vulkan-tied 'Texture' struct, we return the lightweight ticket.
        [[nodiscard]] std::span<const MaterialProvenance> getMaterialProvenance(
            const ModelAsset& model) const;
        [[nodiscard]] void* getMaterialTexturePreview(TextureHandle texture) const;
        [[nodiscard]] std::optional<AssetGuid>
            publishEditorThumbnail(
                AssetGuid assetGuid,
                uint32_t width,
                uint32_t height,
                std::span<const std::byte> rgba8);
        void publishEditorDetailThumbnail(
            AssetGuid assetGuid,
            uint32_t width,
            uint32_t height,
            std::span<const std::byte> rgba8);
        [[nodiscard]] void* getEditorThumbnail(
            AssetGuid assetGuid);
        [[nodiscard]] void*
            getEditorDetailThumbnail(
                AssetGuid assetGuid);

        std::function<void(std::shared_ptr<ModelAsset>)> onModelLoadedCallback;

    private:
        IRenderBackend* renderBackend;

        std::unordered_map<AssetGuid, std::shared_ptr<ModelAsset>, AssetGuidHash>
            cookedModelCache;
        std::unordered_map<const ModelAsset*, std::vector<MaterialProvenance>>
            materialProvenanceCache;
        struct EditorThumbnailEntry {
            TextureHandle texture;
            uint64_t lastUseSerial = 0;
        };
        std::map<AssetGuid, EditorThumbnailEntry>
            editorThumbnails_;
        std::vector<TextureHandle> ownedEnvironmentTextures_;
        std::optional<AssetGuid>
            editorDetailThumbnailGuid_;
        EditorThumbnailEntry
            editorDetailThumbnail_;
        uint64_t editorThumbnailSerial_ = 0;
        static constexpr size_t
            EditorThumbnailCapacity = 512;
        [[nodiscard]] std::optional<AssetGuid>
            publishEditorThumbnailInternal(
                AssetGuid assetGuid,
                uint32_t width,
                uint32_t height,
                std::span<const std::byte> rgba8,
                bool detail);

        std::shared_ptr<ModelAsset>
            loadCompleteModelFromCookedProduct(
                const CookedArtifact& artifact,
                const CookedModelProductData& product,
                std::span<const RuntimeTextureViewBinding>
                    textureViews,
                const RuntimeMaterialFallbacks& fallbacks,
                bool notifyLoaded);
        std::shared_ptr<ModelAsset>
            loadSelfContainedModelFromCookedProduct(
                const CookedArtifact& artifact,
                const CookedModelProductData& product);

        // All texture creation functions now return the RHI tickets
        TextureHandle createDefaultTexture();
        TextureHandle createDefaultNormalTexture();
        TextureHandle createDefaultPbrTexture();

        void uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    };

} // namespace Iridium
