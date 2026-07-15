#include "assets/AssetManager.h" // Update include path if necessary
#include <stb_image.h>
#include <fastgltf/tools.hpp> 
#include <fastgltf/glm_element_traits.hpp>
#include <glm/gtc/type_ptr.hpp>    
#include <glm/gtx/quaternion.hpp> 
#include <iostream>

namespace Iridium {

    AssetManager::AssetManager(IRenderBackend* backend)
        : renderBackend(backend) {}

    AssetManager::~AssetManager() {
        // We gracefully ask the backend to destroy the resources. 
        // The backend's Generational Slot Map will handle the actual Vulkan destruction safely.
        for (auto& pair : modelCache) {
            auto asset = pair.second;

            if (asset->geometry.isValid()) {
                renderBackend->freeGeometry(asset->geometry);
            }

            for (auto& matHandle : asset->materials) {
                renderBackend->freeMaterial(matHandle);
            }

            // Note: We let the backend's internal cleanup() function nuke the Texture Vault 
            // on shutdown so we don't accidentally double-free shared textures here.
        }
    }

    static glm::mat4 convertToGLM(const fastgltf::Node& node) {
        auto transform = fastgltf::getTransformMatrix(node);
        return glm::make_mat4(transform.data());
    }

    void loadNodes(fastgltf::Asset& gltf, size_t nodeIndex, Node* parent, ModelAsset* model) {
        auto& gltfNode = gltf.nodes[nodeIndex];
        auto newNode = std::make_unique<Node>();

        newNode->meshIndex = -1;
        newNode->name = std::string(gltfNode.name);
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

    void AssetManager::flattenNodes(Node* node, glm::mat4 parentTransform, ModelAsset* model) {
        glm::mat4 globalTransform = parentTransform * node->localTransform;

        if (node->meshIndex != -1) {
            const auto& subMeshIndices = model->meshToSubMeshes[node->meshIndex];

            for (int subIdx : subMeshIndices) {
                int matIdx = model->subMeshes[subIdx].materialIndex;

                ModelAsset::BakedPart part;
                part.subMeshIndex = subIdx;
                part.transform = globalTransform;

                model->materialBuckets[matIdx].push_back(part);
            }
        }

        for (auto& child : node->children) {
            flattenNodes(child.get(), globalTransform, model);
        }
    }

    // --- THE TEXTURE ABSTRACTIONS ---

    TextureHandle AssetManager::createDefaultPbrTexture() {
        // R: 255 (No Shadows), G: 255 (Max Roughness), B: 0 (Zero Metallic)
        unsigned char pixels[] = { 255, 255, 0, 255 };
        return renderBackend->allocateTexture(1, 1, 4, pixels, false);
    }

    TextureHandle AssetManager::createDefaultTexture() {
        unsigned char pixels[] = { 255, 255, 255, 255 };
        return renderBackend->allocateTexture(1, 1, 4, pixels, false);
    }

    TextureHandle AssetManager::createDefaultNormalTexture() {
        unsigned char pixels[] = { 128, 128, 255, 255 };
        return renderBackend->allocateTexture(1, 1, 4, pixels, false);
    }

    TextureHandle AssetManager::loadTexture(const std::string& path) {
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) throw std::runtime_error("failed to load texture: " + path);

        TextureHandle handle = renderBackend->allocateTexture(texWidth, texHeight, 4, pixels, false);

        stbi_image_free(pixels); // Memory is now on the GPU, we can dump the CPU copy!
        return handle;
    }

    TextureHandle AssetManager::loadHDRI(const std::string& path) {
        stbi_set_flip_vertically_on_load(true);
        int texWidth, texHeight, texChannels;
        float* pixels = stbi_loadf(path.c_str(), &texWidth, &texHeight, &texChannels, 4);
        stbi_set_flip_vertically_on_load(false);

        if (!pixels) throw std::runtime_error("failed to load HDRI: " + path);

        // Tell the backend it's an HDRI so it uses the 32-bit float format
        TextureHandle handle = renderBackend->allocateTexture(texWidth, texHeight, 4, pixels, true);

        stbi_image_free(pixels);
        return handle;
    }

    // --- GEOMETRY PROCESSING ---

    void AssetManager::uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        // The massive Vulkan buffer creation logic is completely gone. 
        // We just hand the raw data to the backend and store the ticket!
        asset->geometry = renderBackend->allocateGeometry(
            vertices.data(), vertices.size() * sizeof(Vertex),
            indices.data(), indices.size() * sizeof(uint32_t)
        );
    }

    void AssetManager::mergeMaterials(ModelAsset* model, std::vector<Vertex>& originalVertices, std::vector<uint32_t>& originalIndices) {
        std::vector<Vertex> mergedVertices;
        std::vector<uint32_t> mergedIndices;
        std::vector<SubMesh> mergedSubMeshes;

        for (auto& [matIdx, parts] : model->materialBuckets) {
            SubMesh superSubMesh{};
            superSubMesh.materialIndex = matIdx;
            superSubMesh.indexStart = static_cast<uint32_t>(mergedIndices.size());

            for (auto& part : parts) {
                const SubMesh& sub = model->subMeshes[part.subMeshIndex];
                std::map<uint32_t, uint32_t> localToMergedMap;

                for (uint32_t i = 0; i < sub.indexCount; ++i) {
                    uint32_t oldIdx = originalIndices[sub.indexStart + i];

                    if (localToMergedMap.find(oldIdx) == localToMergedMap.end()) {
                        Vertex v = originalVertices[oldIdx];

                        glm::vec4 bakedPos = part.transform * glm::vec4(v.pos, 1.0f);
                        v.pos = glm::vec3(bakedPos);

                        v.normal = glm::normalize(glm::mat3(glm::transpose(glm::inverse(part.transform))) * v.normal);
                        glm::vec3 t = glm::mat3(part.transform) * glm::vec3(v.tangent);
                        v.tangent = glm::vec4(glm::normalize(t), v.tangent.w);

                        localToMergedMap[oldIdx] = static_cast<uint32_t>(mergedVertices.size());
                        mergedVertices.push_back(v);
                    }
                    mergedIndices.push_back(localToMergedMap[oldIdx]);
                }
            }

            superSubMesh.indexCount = static_cast<uint32_t>(mergedIndices.size()) - superSubMesh.indexStart;
            mergedSubMeshes.push_back(superSubMesh);
        }

        model->subMeshes = mergedSubMeshes;
        originalVertices = mergedVertices;
        originalIndices = mergedIndices;
        model->materialBuckets.clear();
    }

    // --- THE MAIN IMPORTER ---

    std::shared_ptr<ModelAsset> AssetManager::getModel(const std::string& path) {
        if (modelCache.find(path) != modelCache.end()) {
            return modelCache[path];
        }

        auto newModel = loadModelFromFile(path);
        modelCache[path] = newModel;
        return newModel;
    }

    std::shared_ptr<ModelAsset> AssetManager::loadModelFromFile(const std::string& path) {
        constexpr auto extensions = fastgltf::Extensions::KHR_materials_emissive_strength;
        fastgltf::Parser parser(extensions);

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

        std::vector<bool> isImageLinear(gltf.images.size(), false);
        for (const auto& mat : gltf.materials) {
            if (mat.normalTexture.has_value()) {
                if (auto idx = gltf.textures[mat.normalTexture->textureIndex].imageIndex) isImageLinear[*idx] = true;
            }
            if (mat.pbrData.metallicRoughnessTexture.has_value()) {
                if (auto idx = gltf.textures[mat.pbrData.metallicRoughnessTexture->textureIndex].imageIndex) isImageLinear[*idx] = true;
            }
            if (mat.occlusionTexture.has_value()) {
                if (auto idx = gltf.textures[mat.occlusionTexture->textureIndex].imageIndex) isImageLinear[*idx] = true;
            }
        }

        // 1. Load Textures Locally 
        std::vector<TextureHandle> localTextures;
        localTextures.push_back(createDefaultTexture());
        int whiteTextureIndex = 0;

        localTextures.push_back(createDefaultNormalTexture());
        int flatNormalIndex = 1;

        localTextures.push_back(createDefaultPbrTexture());
        int flatPbrIndex = 2;

        std::vector<int> gltfToOurTextureMap(gltf.images.size(), -1);

        for (size_t i = 0; i < gltf.images.size(); ++i) {
            auto& image = gltf.images[i];
            if (auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
                std::string texturePath = (folder / uri->uri.path()).string();
                if (!std::filesystem::exists(texturePath)) continue;

                try {
                    localTextures.push_back(loadTexture(texturePath));
                    gltfToOurTextureMap[i] = static_cast<int>(localTextures.size() - 1);
                }
                catch (...) {}
            }
        }

        // 2. Load Materials
        std::vector<glm::vec3> materialColors;
        for (auto& mat : gltf.materials) {
            auto& factor = mat.pbrData.baseColorFactor;
            materialColors.push_back(glm::vec3(factor[0], factor[1], factor[2]));

            glm::vec4 baseColor = glm::vec4(factor[0], factor[1], factor[2], factor[3]);
            float metallic = mat.pbrData.metallicFactor;
            float roughness = mat.pbrData.roughnessFactor;
            float maxEmissiveColor = std::max({ mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2] });
            float emissive = mat.emissiveStrength * maxEmissiveColor;

            if (emissive > 0.0f) {
                baseColor.r = std::max(baseColor.r, mat.emissiveFactor[0]);
                baseColor.g = std::max(baseColor.g, mat.emissiveFactor[1]);
                baseColor.b = std::max(baseColor.b, mat.emissiveFactor[2]);
            }

            // Map the textures
            int albedoIdx = whiteTextureIndex;
            if (mat.pbrData.baseColorTexture.has_value()) {
                size_t texIndex = mat.pbrData.baseColorTexture.value().textureIndex;
                if (gltf.textures[texIndex].imageIndex.has_value()) {
                    int mapIdx = gltfToOurTextureMap[gltf.textures[texIndex].imageIndex.value()];
                    albedoIdx = (mapIdx != -1) ? mapIdx : whiteTextureIndex;
                }
            }

            int normalIdx = flatNormalIndex;
            if (mat.normalTexture.has_value()) {
                size_t texIndex = mat.normalTexture.value().textureIndex;
                if (gltf.textures[texIndex].imageIndex.has_value()) {
                    int mapIdx = gltfToOurTextureMap[gltf.textures[texIndex].imageIndex.value()];
                    normalIdx = (mapIdx != -1) ? mapIdx : flatNormalIndex;
                }
            }

            int pbrIdx = flatPbrIndex;
            if (mat.pbrData.metallicRoughnessTexture.has_value()) {
                size_t texIndex = mat.pbrData.metallicRoughnessTexture.value().textureIndex;
                if (gltf.textures[texIndex].imageIndex.has_value()) {
                    int mapIdx = gltfToOurTextureMap[gltf.textures[texIndex].imageIndex.value()];
                    pbrIdx = (mapIdx != -1) ? mapIdx : flatPbrIndex;
                }
            }

            // NEW TRANSPARENCY DETECTION
            bool isTransparent = (mat.alphaMode == fastgltf::AlphaMode::Blend);

            if (mat.transmission && mat.transmission->transmissionFactor > 0.0f) {
                isTransparent = true;
                baseColor.a = 1.0f - mat.transmission->transmissionFactor;
            }

            if (baseColor.a < 0.99f) {
                isTransparent = true;
            }

            // --- DIAGNOSTIC PRINT ---
            // Let's see exactly what this material is reporting!
            std::cout << "Material: " << mat.name << "\n"
                << "  - AlphaMode: " << (int)mat.alphaMode << " (0=Opaque, 1=Mask, 2=Blend)\n"
                << "  - BaseColor Alpha: " << baseColor.a << "\n"
                << "  - Transmission: " << (mat.transmission ? "YES" : "NO") << "\n"
                << "  -> QUEUE: " << (isTransparent ? "TRANSPARENT" : "OPAQUE") << "\n\n";

            if (isTransparent) {
                model->isTransparent = true;
                model->materialIsTransparent.push_back(true);
            }
            else {
                model->materialIsTransparent.push_back(false);
            }

            // We let the backend generate the Vulkan descriptor sets and we just store the Handle
            MaterialHandle newMaterial = renderBackend->allocateMaterial(
                localTextures[albedoIdx], localTextures[normalIdx], localTextures[pbrIdx],
                baseColor, metallic, roughness, emissive
            );

            model->materials.push_back(newMaterial);
        }

        // 3. Load Geometry & Map Submeshes to Nodes
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        int subMeshGlobalIndex = 0;

        std::vector<int> gltfToOurTextureMap(gltf.images.size(), -1);

        for (size_t i = 0; i < gltf.meshes.size(); ++i) {
            auto& mesh = gltf.meshes[i];
            model->meshToSubMeshes[i] = {};

            for (auto& primitive : mesh.primitives) {
                SubMesh subMesh{};
                subMesh.indexStart = static_cast<uint32_t>(indices.size());
                subMesh.materialIndex = primitive.materialIndex.value_or(0);

                uint32_t globalVertexOffset = static_cast<uint32_t>(vertices.size());
                glm::vec3 meshColor = (subMesh.materialIndex < materialColors.size()) ? materialColors[subMesh.materialIndex] : glm::vec3(1.0f);

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
                        vertex.tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
                        });

                    auto uvIt = primitive.findAttribute("TEXCOORD_0");
                    if (uvIt == primitive.attributes.end()) {
                        uvIt = primitive.findAttribute("TEXCOORD_1");
                    }

                    if (uvIt != primitive.attributes.end()) {
                        auto& uvAccessor = gltf.accessors[uvIt->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, uvAccessor, [&](glm::vec2 uv, size_t idx) {
                            vertices[initialVtxCount + idx].uv = uv;
                            });
                    }

                    auto normIt = primitive.findAttribute("NORMAL");
                    if (normIt != primitive.attributes.end()) {
                        auto& normAccessor = gltf.accessors[normIt->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, normAccessor, [&](glm::vec3 n, size_t idx) {
                            vertices[initialVtxCount + idx].normal = n;
                            });
                    }

                    auto tanIt = primitive.findAttribute("TANGENT");
                    if (tanIt != primitive.attributes.end()) {
                        auto& tanAccessor = gltf.accessors[tanIt->accessorIndex];
                        fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, tanAccessor, [&](glm::vec4 t, size_t idx) {
                            vertices[initialVtxCount + idx].tangent = t;
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

                auto tanIt = primitive.findAttribute("TANGENT");
                if (tanIt == primitive.attributes.end() && primitive.indicesAccessor.has_value()) {
                    for (size_t j = 0; j < subMesh.indexCount; ++j) {
                        vertices[indices[subMesh.indexStart + j]].tangent = glm::vec4(0.0f);
                    }

                    for (size_t j = 0; j < subMesh.indexCount; j += 3) {
                        uint32_t i0 = indices[subMesh.indexStart + j];
                        uint32_t i1 = indices[subMesh.indexStart + j + 1];
                        uint32_t i2 = indices[subMesh.indexStart + j + 2];

                        Vertex& v0 = vertices[i0];
                        Vertex& v1 = vertices[i1];
                        Vertex& v2 = vertices[i2];

                        glm::vec3 edge1 = v1.pos - v0.pos;
                        glm::vec3 edge2 = v2.pos - v0.pos;
                        glm::vec2 deltaUV1 = v1.uv - v0.uv;
                        glm::vec2 deltaUV2 = v2.uv - v0.uv;

                        float f = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
                        if (std::isinf(f) || std::isnan(f)) f = 1.0f;

                        glm::vec3 tangent;
                        tangent.x = f * (deltaUV2.y * edge1.x - deltaUV1.y * edge2.x);
                        tangent.y = f * (deltaUV2.y * edge1.y - deltaUV1.y * edge2.y);
                        tangent.z = f * (deltaUV2.y * edge1.z - deltaUV1.y * edge2.z);

                        v0.tangent += glm::vec4(tangent, 0.0f);
                        v1.tangent += glm::vec4(tangent, 0.0f);
                        v2.tangent += glm::vec4(tangent, 0.0f);
                    }

                    for (size_t j = 0; j < subMesh.indexCount; ++j) {
                        Vertex& v = vertices[indices[subMesh.indexStart + j]];
                        glm::vec3 t = glm::vec3(v.tangent);

                        if (glm::length(t) > 0.0f) {
                            glm::vec3 n = v.normal;
                            t = glm::normalize(t - n * glm::dot(n, t));
                            v.tangent = glm::vec4(t, 1.0f);
                        }
                        else {
                            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                        }
                    }
                }

                model->subMeshes.push_back(subMesh);
                model->meshToSubMeshes[i].push_back(subMeshGlobalIndex);
                subMeshGlobalIndex++;
            }
        }

        auto& scene = gltf.scenes[gltf.defaultScene.value_or(0)];
        for (auto& nodeIndex : scene.nodeIndices) {
            loadNodes(gltf, nodeIndex, nullptr, model.get());
        }

        for (auto& root : model->rootNodes) {
            flattenNodes(root.get(), glm::mat4(1.0f), model.get());
        }

        mergeMaterials(model.get(), vertices, indices);

        model->totalIndices = static_cast<uint32_t>(indices.size());

        // Let the RHI upload the buffers!
        uploadToGPU(model.get(), vertices, indices);

        if (onModelLoadedCallback) {
            onModelLoadedCallback(model);
        }

        return model;
    }

} // namespace Iridium