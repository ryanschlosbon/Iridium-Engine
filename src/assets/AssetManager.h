#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <filesystem>
#include <fastgltf/core.hpp>
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/IRenderBackend.h"

namespace Iridium {

    struct Node {
        std::string name;
        int meshIndex = -1;
        glm::mat4 localTransform = glm::mat4(1.0f);
        // Note: We don't store globalTransform here because we calculate it 
        // on the fly during the render loop to save memory.
        std::vector<std::unique_ptr<Node>> children;
    };

    class AssetManager {
    public:
        // The AssetManager now only takes a pointer to the abstract interface.
        AssetManager(IRenderBackend* renderBackend);
        ~AssetManager();

        std::shared_ptr<ModelAsset> getModel(const std::string& path);

        // Instead of returning a Vulkan-tied 'Texture' struct, we return the lightweight ticket.
        TextureHandle loadHDRI(const std::string& path);

        std::function<void(std::shared_ptr<ModelAsset>)> onModelLoadedCallback;

    private:
        IRenderBackend* renderBackend;

        std::unordered_map<std::string, std::shared_ptr<ModelAsset>> modelCache;

        std::shared_ptr<ModelAsset> loadModelFromFile(const std::string& path);

        // All texture creation functions now return the RHI tickets
        TextureHandle loadTexture(const std::string& path);
        TextureHandle createDefaultTexture();
        TextureHandle createDefaultNormalTexture();
        TextureHandle createDefaultPbrTexture();

        void mergeMaterials(ModelAsset* model, std::vector<Vertex>& originalVertices,
            std::vector<uint32_t>& originalIndices);

        void flattenNodes(Node* node, glm::mat4 parentTransform, ModelAsset* model);

        void uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    };

} // namespace Iridium