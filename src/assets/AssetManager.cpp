#include "AssetManager.h"
#include <stb_image.h>
#include <fastgltf/tools.hpp> // Required for iteration tools
#include <fastgltf/glm_element_traits.hpp>
#include <glm/gtc/type_ptr.hpp>    // Required for glm::make_mat4
#include <glm/gtx/quaternion.hpp> // Required for mat4_cast
#include <iostream>

AssetManager::AssetManager(VkContext* context, VkCommandManager* cmdManager)
    : vkContext(context), vkCmdManager(cmdManager) {
}

AssetManager::~AssetManager() {
    // Cleanup logic for all cached models
    for (auto& pair : modelCache) {
        auto asset = pair.second;
        vkDestroyBuffer(vkContext->getDevice(), asset->vertexBuffer, nullptr);
        vkFreeMemory(vkContext->getDevice(), asset->vertexBufferMemory, nullptr);
        vkDestroyBuffer(vkContext->getDevice(), asset->indexBuffer, nullptr);
        vkFreeMemory(vkContext->getDevice(), asset->indexBufferMemory, nullptr);

        for (auto& tex : asset->textures) {
            vkDestroySampler(vkContext->getDevice(), tex.sampler, nullptr);
            vkDestroyImageView(vkContext->getDevice(), tex.view, nullptr);
            vkDestroyImage(vkContext->getDevice(), tex.image, nullptr);
            vkFreeMemory(vkContext->getDevice(), tex.memory, nullptr);
        }
    }
}

static glm::mat4 convertToGLM(const fastgltf::Node& node) {
    auto transform = fastgltf::getTransformMatrix(node); // Use library tool
    return glm::make_mat4(transform.data()); // Convert to GLM
}

void loadNodes(fastgltf::Asset& gltf, size_t nodeIndex, Node* parent, ModelAsset* model) {
    auto& gltfNode = gltf.nodes[nodeIndex];
    auto newNode = std::make_unique<Node>();

    newNode->name = std::string(gltfNode.name);

    // FIX: Call the helper function instead of direct assignment
    newNode->localTransform = convertToGLM(gltfNode);

    if (gltfNode.meshIndex.has_value()) {
        newNode->meshIndex = static_cast<int>(gltfNode.meshIndex.value());
    }

    Node* ptr = newNode.get();
    if (parent) {
        parent->children.push_back(std::move(newNode));
    }
    else {
        model->rootNodes.push_back(std::move(newNode));
    }

    for (auto& childIndex : gltfNode.children) {
        loadNodes(gltf, childIndex, ptr, model);
    }
}

// AssetManager.cpp helper
void flattenNodes(Node* node, glm::mat4 parentTransform, ModelAsset* model) {
    glm::mat4 globalTransform = parentTransform * node->localTransform;

    if (node->meshIndex != -1) {
        // 1. Find the material index for this part
        // We look at the first submesh of this mesh to decide its "bucket"
        int matIdx = 0;
        if (!model->meshToSubMeshes[node->meshIndex].empty()) {
            int firstSubMeshIdx = model->meshToSubMeshes[node->meshIndex][0];
            matIdx = model->subMeshes[firstSubMeshIdx].materialIndex;
        }

        // 2. Add to the correct material bucket
        ModelAsset::BakedPart part;
        part.meshIndex = node->meshIndex;
        part.transform = globalTransform;

        model->materialBuckets[matIdx].push_back(part);
    }

    for (auto& child : node->children) {
        flattenNodes(child.get(), globalTransform, model);
    }
}

// In AssetManager.cpp

void AssetManager::mergeMaterials(ModelAsset* model, std::vector<Vertex>& originalVertices, std::vector<uint32_t>& originalIndices) {
    std::vector<Vertex> mergedVertices;
    std::vector<uint32_t> mergedIndices;
    std::vector<SubMesh> mergedSubMeshes;

    // 1. Group by material to collapse draw calls
    for (auto& [matIdx, parts] : model->materialBuckets) {
        SubMesh superSubMesh{};
        superSubMesh.materialIndex = matIdx;
        superSubMesh.indexStart = static_cast<uint32_t>(mergedIndices.size());

        for (auto& part : parts) {
            // Track where these vertices start in the new global buffer
            uint32_t vertexStartInMerged = static_cast<uint32_t>(mergedVertices.size());

            const auto& subMeshIndices = model->meshToSubMeshes[part.meshIndex];
            for (int subIdx : subMeshIndices) {
                const SubMesh& sub = model->subMeshes[subIdx];

                // A. Transform and Copy Vertices
                // We must use the original indices to find which vertices this submesh uses
                std::map<uint32_t, uint32_t> localToMergedMap;

                for (uint32_t i = 0; i < sub.indexCount; ++i) {
                    uint32_t oldIdx = originalIndices[sub.indexStart + i];

                    // Avoid duplicating vertices if they are shared within the same part
                    if (localToMergedMap.find(oldIdx) == localToMergedMap.end()) {
                        Vertex v = originalVertices[oldIdx];

                        // APPLY TRANSFORM: Bake the part's position into the vertex
                        glm::vec4 bakedPos = part.transform * glm::vec4(v.pos, 1.0f);
                        v.pos = glm::vec3(bakedPos);

                        // Fix normals for rotation/scale
                        v.normal = glm::normalize(glm::mat3(glm::transpose(glm::inverse(part.transform))) * v.normal);

                        localToMergedMap[oldIdx] = static_cast<uint32_t>(mergedVertices.size());
                        mergedVertices.push_back(v);
                    }

                    // B. Map the index to the new vertex position
                    mergedIndices.push_back(localToMergedMap[oldIdx]);
                }
            }
        }

        superSubMesh.indexCount = static_cast<uint32_t>(mergedIndices.size()) - superSubMesh.indexStart;
        mergedSubMeshes.push_back(superSubMesh);
    }

    // 3. Finalize ModelAsset
    model->subMeshes = mergedSubMeshes;
    originalVertices = mergedVertices;
    originalIndices = mergedIndices;
    model->materialBuckets.clear();
}

// Public entry point that checks the cache first
std::shared_ptr<ModelAsset> AssetManager::getModel(const std::string& path) {
    if (modelCache.find(path) != modelCache.end()) {
        return modelCache[path];
    }

    auto newModel = loadModelFromFile(path);
    modelCache[path] = newModel;
    return newModel;
}

// ADDED AssetManager:: scope here
std::shared_ptr<ModelAsset> AssetManager::loadModelFromFile(const std::string& path) {
    fastgltf::Parser parser;
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        throw std::runtime_error("Failed to load glTF file: " + path);
    }

    auto folder = std::filesystem::path(path).parent_path();
    auto asset = parser.loadGltf(data.get(), folder, fastgltf::Options::LoadExternalBuffers);

    if (auto error = asset.error(); error != fastgltf::Error::None) {
        throw std::runtime_error("Failed to parse glTF: " + std::to_string(static_cast<int>(error)));
    }

    auto& gltf = asset.get();
    auto model = std::make_shared<ModelAsset>();
    model->filePath = path;

    // 1. Load Images
    for (auto& image : gltf.images) {
        std::string texturePath;
        if (auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
            texturePath = (folder / uri->uri.path()).string();
        }
        if (!texturePath.empty()) {
            model->textures.push_back(loadTexture(texturePath));
        }
    }

    // Add Fallback White Texture
    model->textures.push_back(createDefaultTexture());
    int whiteTextureIndex = static_cast<int>(model->textures.size() - 1);

    // 2. Load Materials
    std::vector<glm::vec3> materialColors;
    for (auto& mat : gltf.materials) {
        auto& factor = mat.pbrData.baseColorFactor;
        materialColors.push_back(glm::vec3(factor[0], factor[1], factor[2]));

        Material iridiumMat{};
        iridiumMat.baseColor = glm::vec4(factor[0], factor[1], factor[2], factor[3]);

        if (mat.pbrData.baseColorTexture.has_value()) {
            size_t texIndex = mat.pbrData.baseColorTexture.value().textureIndex;
            iridiumMat.textureIndex = static_cast<int>(gltf.textures[texIndex].imageIndex.value_or(whiteTextureIndex));
        }
        else {
            iridiumMat.textureIndex = whiteTextureIndex;
        }
        model->materials.push_back(iridiumMat);
    }

    // 3. Load Geometry & Map Submeshes to Nodes
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int subMeshGlobalIndex = 0;

    for (size_t i = 0; i < gltf.meshes.size(); ++i) {
        auto& mesh = gltf.meshes[i];
        model->meshToSubMeshes[i] = {}; // Initialize submesh list for this glTF mesh

        for (auto& primitive : mesh.primitives) {
            SubMesh subMesh{};
            subMesh.indexStart = static_cast<uint32_t>(indices.size());
            subMesh.materialIndex = primitive.materialIndex.value_or(0);

            uint32_t globalVertexOffset = static_cast<uint32_t>(vertices.size());
            glm::vec3 meshColor = (subMesh.materialIndex < materialColors.size()) ? materialColors[subMesh.materialIndex] : glm::vec3(1.0f);

            // Accessor Iteration
            auto posIt = primitive.findAttribute("POSITION");
            if (posIt != primitive.attributes.end()) {
                auto& accessor = gltf.accessors[posIt->accessorIndex];
                size_t initialVtxCount = vertices.size();
                vertices.resize(initialVtxCount + accessor.count);

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 v, size_t idx) {
                    Vertex& vertex = vertices[initialVtxCount + idx];
                    vertex.pos = v;
                    vertex.color = meshColor;
                    vertex.uv = { 0.0f, 0.0f };
                    vertex.normal = { 0.0f, 1.0f, 0.0f };
                    });

                auto uvIt = primitive.findAttribute("TEXCOORD_0");
                if (uvIt != primitive.attributes.end()) {
                    auto& accessor = gltf.accessors[uvIt->accessorIndex];
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, accessor, [&](glm::vec2 uv, size_t idx) {
                        vertices[initialVtxCount + idx].uv = uv;
                        });
                }
            }

            if (primitive.indicesAccessor.has_value()) {
                auto& accessor = gltf.accessors[primitive.indicesAccessor.value()];
                subMesh.indexCount = static_cast<uint32_t>(accessor.count);
                fastgltf::iterateAccessor<std::uint32_t>(gltf, accessor, [&](std::uint32_t idx) {
                    indices.push_back(idx + globalVertexOffset);
                    });
            }

            model->subMeshes.push_back(subMesh);
            model->meshToSubMeshes[i].push_back(subMeshGlobalIndex);
            subMeshGlobalIndex++;
        }
    }

    // 4. Load Scene Hierarchy (The Tree)
    auto& scene = gltf.scenes[gltf.defaultScene.value_or(0)];
    for (auto& nodeIndex : scene.nodeIndices) {
        loadNodes(gltf, nodeIndex, nullptr, model.get());
    }

    // 5. Bake the tree into a flat list for performance
    for (auto& root : model->rootNodes) {
        flattenNodes(root.get(), glm::mat4(1.0f), model.get());
    }

    mergeMaterials(model.get(), vertices, indices);

    model->totalIndices = static_cast<uint32_t>(indices.size());
    uploadToGPU(model.get(), vertices, indices);

    return model;
}

// SCOPED to AssetManager
Texture AssetManager::loadTexture(const std::string& path) {
    Texture tex{};
    int texWidth, texHeight, texChannels;
    stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
    if (!pixels) throw std::runtime_error("failed to load texture: " + path);

    VkDeviceSize imageSize = texWidth * texHeight * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    vkContext->createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(vkContext->getDevice(), stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(vkContext->getDevice(), stagingBufferMemory);
    stbi_image_free(pixels);

    vkContext->createImage(texWidth, texHeight, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory);

    // Call through our member vkCmdManager
    vkCmdManager->transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdManager->copyBufferToImage(stagingBuffer, tex.image, (uint32_t)texWidth, (uint32_t)texHeight);
    vkCmdManager->transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(vkContext->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(vkContext->getDevice(), stagingBufferMemory, nullptr);

    // ImageView & Sampler setup...
    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &tex.view);

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vkContext->getPhysicalDevice(), &props);
    samplerInfo.maxAnisotropy = props.limits.maxSamplerAnisotropy;
    vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &tex.sampler);

    return tex;
}

// SCOPED to AssetManager
Texture AssetManager::createDefaultTexture() {
    Texture tex{};
    unsigned char pixels[] = { 255, 255, 255, 255 };

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    vkContext->createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(vkContext->getDevice(), stagingBufferMemory, 0, 4, 0, &data);
    memcpy(data, pixels, 4);
    vkUnmapMemory(vkContext->getDevice(), stagingBufferMemory);

    vkContext->createImage(1, 1, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory);

    vkCmdManager->transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdManager->copyBufferToImage(stagingBuffer, tex.image, 1, 1);
    vkCmdManager->transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(vkContext->getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(vkContext->getDevice(), stagingBufferMemory, nullptr);

    // Create View & Sampler for 1x1...
    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vkContext->getDevice(), &viewInfo, nullptr, &tex.view);

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(vkContext->getDevice(), &samplerInfo, nullptr, &tex.sampler);

    return tex;
}

// SCOPED to AssetManager
void AssetManager::uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    VkDeviceSize vSize = sizeof(Vertex) * vertices.size();
    VkDeviceSize iSize = sizeof(uint32_t) * indices.size();

    vkContext->createGPUBuffer(vSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(),
        asset->vertexBuffer, asset->vertexBufferMemory, vkCmdManager);
    vkContext->createGPUBuffer(iSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(),
        asset->indexBuffer, asset->indexBufferMemory, vkCmdManager);
}