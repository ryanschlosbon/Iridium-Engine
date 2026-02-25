#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include <filesystem>
#include <fastgltf/core.hpp>
#include "renderer/VkContext.h"
#include "renderer/VkMesh.h"
#include "renderer/VkCommandManager.h"

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
    AssetManager(VkContext* context, VkCommandManager* cmdManager);
    ~AssetManager();

    // The main entry point for the ECS to get a model
    std::shared_ptr<ModelAsset> getModel(const std::string& path);
    Texture loadHDRI(const std::string& path);
    std::function<void(std::shared_ptr<ModelAsset>)> onModelLoadedCallback;

private:
    VkContext* vkContext;
    VkCommandManager* vkCmdManager;

    std::unordered_map<std::string, std::shared_ptr<ModelAsset>> modelCache;

    // Core logic moved from main.cpp
    std::shared_ptr<ModelAsset> loadModelFromFile(const std::string& path);
    Texture loadTexture(const std::string& path);
    Texture createDefaultTexture();
    Texture createDefaultNormalTexture();
    Texture createDefaultPbrTexture();
    void mergeMaterials(ModelAsset* model, std::vector<Vertex>& originalVertices,
        std::vector<uint32_t>& originalIndices);

    // Internal buffer helpers
    void uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
};