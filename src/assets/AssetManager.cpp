#include "assets/AssetManager.h" // Update include path if necessary
#include <stb_image.h>
#include <fastgltf/tools.hpp> 
#include <fastgltf/glm_element_traits.hpp>
#include <glm/gtc/type_ptr.hpp>    
#include <glm/gtx/quaternion.hpp> 
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <variant>

namespace Iridium {

    AssetManager::AssetManager(IRenderBackend* backend)
        : renderBackend(backend) {}

    AssetManager::~AssetManager() {
        std::set<MaterialHandle> freedMaterials;
        std::set<TextureHandle> freedTextures;
        std::set<GeometryHandle> freedGeometry;

        for (auto& pair : modelCache) {
            auto asset = pair.second;

            for (const MaterialBinding& binding : asset->materials) {
                if (binding.material.isValid() && freedMaterials.insert(binding.material).second) {
                    renderBackend->freeMaterial(binding.material);
                }
            }
        }

        for (auto& pair : modelCache) {
            auto asset = pair.second;

            for (auto& textureHandle : asset->ownedTextures) {
                if (textureHandle.isValid() && freedTextures.insert(textureHandle).second) {
                    renderBackend->freeTexture(textureHandle);
                }
            }
        }

        for (auto& pair : modelCache) {
            auto asset = pair.second;

            if (asset->geometry.isValid() && freedGeometry.insert(asset->geometry).second) {
                renderBackend->freeGeometry(asset->geometry);
            }
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
        // The glTF factors are multiplied by this texture. White preserves both
        // roughness (G) and metallic (B) factors when no texture is supplied.
        const unsigned char pixels[] = { 255, 255, 255, 255 };
        TextureDesc desc{};
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::RGBA8_UNorm;
        return renderBackend->allocateTexture(desc, std::as_bytes(std::span(pixels)));
    }

    TextureHandle AssetManager::createDefaultTexture() {
        const unsigned char pixels[] = { 255, 255, 255, 255 };
        TextureDesc desc{};
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::RGBA8_sRGB;
        return renderBackend->allocateTexture(desc, std::as_bytes(std::span(pixels)));
    }

    TextureHandle AssetManager::createDefaultNormalTexture() {
        const unsigned char pixels[] = { 128, 128, 255, 255 };
        TextureDesc desc{};
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::RGBA8_UNorm;
        return renderBackend->allocateTexture(desc, std::as_bytes(std::span(pixels)));
    }

    TextureHandle AssetManager::loadTexture(const std::string& path, TextureFormat format) {
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) throw std::runtime_error("failed to load texture: " + path);

        TextureDesc desc{};
        desc.width = static_cast<uint32_t>(texWidth);
        desc.height = static_cast<uint32_t>(texHeight);
        desc.format = format;
        const auto pixelSpan = std::span(pixels, static_cast<size_t>(texWidth) * texHeight * 4);
        TextureHandle handle = renderBackend->allocateTexture(desc, std::as_bytes(pixelSpan));

        stbi_image_free(pixels); // Memory is now on the GPU, we can dump the CPU copy!
        return handle;
    }

    TextureHandle AssetManager::loadTexture(std::span<const std::byte> encodedBytes, TextureFormat format) {
        if (encodedBytes.empty() || encodedBytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            throw std::runtime_error("invalid encoded texture payload");
        }

        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(encodedBytes.data()),
            static_cast<int>(encodedBytes.size()),
            &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) throw std::runtime_error("failed to decode embedded texture");

        TextureDesc desc{};
        desc.width = static_cast<uint32_t>(texWidth);
        desc.height = static_cast<uint32_t>(texHeight);
        desc.format = format;
        const auto pixelSpan = std::span(pixels, static_cast<size_t>(texWidth) * texHeight * 4);
        TextureHandle handle = renderBackend->allocateTexture(desc, std::as_bytes(pixelSpan));

        stbi_image_free(pixels);
        return handle;
    }

    TextureHandle AssetManager::loadHDRI(const std::string& path) {
        stbi_set_flip_vertically_on_load(true);
        int texWidth, texHeight, texChannels;
        float* pixels = stbi_loadf(path.c_str(), &texWidth, &texHeight, &texChannels, 4);
        stbi_set_flip_vertically_on_load(false);

        if (!pixels) throw std::runtime_error("failed to load HDRI: " + path);

        TextureDesc desc{};
        desc.width = static_cast<uint32_t>(texWidth);
        desc.height = static_cast<uint32_t>(texHeight);
        desc.format = TextureFormat::RGBA32_SFloat;
        desc.sampler.addressU = SamplerAddressMode::Repeat;
        desc.sampler.addressV = SamplerAddressMode::ClampToEdge;
        desc.sampler.addressW = SamplerAddressMode::ClampToEdge;
        const auto pixelSpan = std::span(pixels, static_cast<size_t>(texWidth) * texHeight * 4);
        TextureHandle handle = renderBackend->allocateTexture(desc, std::as_bytes(pixelSpan));

        stbi_image_free(pixels);
        return handle;
    }

    // --- GEOMETRY PROCESSING ---

    void AssetManager::uploadToGPU(ModelAsset* asset, const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
        // The massive Vulkan buffer creation logic is completely gone. 
        // We just hand the raw data to the backend and store the ticket!
        GeometryDesc desc{};
        desc.vertexStride = sizeof(Vertex);
        desc.indexFormat = IndexFormat::UInt32;
        asset->geometry = renderBackend->allocateGeometry(desc,
            std::as_bytes(std::span(vertices)), std::as_bytes(std::span(indices)));
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
        constexpr auto extensions = fastgltf::Extensions::KHR_materials_emissive_strength |
            fastgltf::Extensions::KHR_materials_transmission;
        fastgltf::Parser parser(extensions);

        auto data = fastgltf::GltfDataBuffer::FromPath(path);
        if (data.error() != fastgltf::Error::None) {
            throw std::runtime_error("Failed to load glTF file: " + path);
        }

        auto folder = std::filesystem::path(path).parent_path();
        auto asset = parser.loadGltf(data.get(), folder,
            fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages);

        if (auto error = asset.error(); error != fastgltf::Error::None) {
            throw std::runtime_error("Failed to parse glTF: " + std::to_string(static_cast<int>(error)));
        }

        auto& gltf = asset.get();
        auto model = std::make_shared<ModelAsset>();
        model->filePath = path;

        // 1. Load Textures Locally 
        std::vector<TextureHandle> localTextures;
        localTextures.push_back(createDefaultTexture());
        int whiteTextureIndex = 0;

        localTextures.push_back(createDefaultNormalTexture());
        int flatNormalIndex = 1;

        localTextures.push_back(createDefaultPbrTexture());
        int flatPbrIndex = 2;

        using ImageFormatKey = std::pair<size_t, TextureFormat>;
        std::map<ImageFormatKey, std::optional<TextureHandle>> textureCache;

        const auto resolveImageTexture = [this, &gltf, &folder, &localTextures, &textureCache](
            size_t imageIndex, TextureFormat format, TextureHandle fallback) {
            if (imageIndex >= gltf.images.size()) {
                return fallback;
            }

            const ImageFormatKey key{ imageIndex, format };
            if (const auto cacheIt = textureCache.find(key); cacheIt != textureCache.end()) {
                return cacheIt->second.value_or(fallback);
            }

            std::optional<TextureHandle> resolvedTexture;
            const auto& image = gltf.images[imageIndex];
            try {
                if (const auto* uri = std::get_if<fastgltf::sources::URI>(&image.data)) {
                    const std::filesystem::path texturePath = folder / uri->uri.path();
                    if (std::filesystem::exists(texturePath)) {
                        TextureHandle handle = loadTexture(texturePath.string(), format);
                        localTextures.push_back(handle);
                        resolvedTexture = handle;
                    }
                }
                else if (const auto* array = std::get_if<fastgltf::sources::Array>(&image.data)) {
                    TextureHandle handle = loadTexture(
                        std::span<const std::byte>(array->bytes.data(), array->bytes.size()), format);
                    localTextures.push_back(handle);
                    resolvedTexture = handle;
                }
                else if (const auto* vector = std::get_if<fastgltf::sources::Vector>(&image.data)) {
                    TextureHandle handle = loadTexture(
                        std::span<const std::byte>(vector->bytes.data(), vector->bytes.size()), format);
                    localTextures.push_back(handle);
                    resolvedTexture = handle;
                }
                else if (const auto* bytes = std::get_if<fastgltf::sources::ByteView>(&image.data)) {
                    TextureHandle handle = loadTexture(
                        std::span<const std::byte>(bytes->bytes.data(), bytes->bytes.size()), format);
                    localTextures.push_back(handle);
                    resolvedTexture = handle;
                }
                else if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
                    const auto bufferBytes = fastgltf::DefaultBufferDataAdapter{}(gltf, view->bufferViewIndex);
                    TextureHandle handle = loadTexture(
                        std::span<const std::byte>(bufferBytes.data(), bufferBytes.size()), format);
                    localTextures.push_back(handle);
                    resolvedTexture = handle;
                }
            }
            catch (...) {
                // Preserve the correct semantic fallback for an unreadable image.
            }

            textureCache.emplace(key, resolvedTexture);
            return resolvedTexture.value_or(fallback);
        };

        // 2. Load Materials
        auto addMaterial = [this, &model](const MaterialAsset& materialAsset) {
            model->materials.push_back(renderBackend->allocateMaterial(materialAsset));
        };

        for (auto& mat : gltf.materials) {
            auto& factor = mat.pbrData.baseColorFactor;

            glm::vec4 baseColor = glm::vec4(factor[0], factor[1], factor[2], factor[3]);
            float metallic = mat.pbrData.metallicFactor;
            float roughness = mat.pbrData.roughnessFactor;
            const glm::vec4 emissiveFactor(
                mat.emissiveFactor[0] * mat.emissiveStrength,
                mat.emissiveFactor[1] * mat.emissiveStrength,
                mat.emissiveFactor[2] * mat.emissiveStrength,
                0.0f);

            // Map the textures
            TextureHandle albedoMap = localTextures[whiteTextureIndex];
            if (mat.pbrData.baseColorTexture.has_value()) {
                size_t texIndex = mat.pbrData.baseColorTexture.value().textureIndex;
                if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
                    albedoMap = resolveImageTexture(gltf.textures[texIndex].imageIndex.value(),
                        TextureFormat::RGBA8_sRGB, albedoMap);
                }
            }

            TextureHandle normalMap = localTextures[flatNormalIndex];
            if (mat.normalTexture.has_value()) {
                size_t texIndex = mat.normalTexture.value().textureIndex;
                if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
                    normalMap = resolveImageTexture(gltf.textures[texIndex].imageIndex.value(),
                        TextureFormat::RGBA8_UNorm, normalMap);
                }
            }

            TextureHandle pbrMap = localTextures[flatPbrIndex];
            if (mat.pbrData.metallicRoughnessTexture.has_value()) {
                size_t texIndex = mat.pbrData.metallicRoughnessTexture.value().textureIndex;
                if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
                    pbrMap = resolveImageTexture(gltf.textures[texIndex].imageIndex.value(),
                        TextureFormat::RGBA8_UNorm, pbrMap);
                }
            }

            TextureHandle emissiveMap = localTextures[whiteTextureIndex];
            if (mat.emissiveTexture.has_value()) {
                size_t texIndex = mat.emissiveTexture->textureIndex;
                if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
                    emissiveMap = resolveImageTexture(gltf.textures[texIndex].imageIndex.value(),
                        TextureFormat::RGBA8_sRGB, emissiveMap);
                }
            }

            const float transmissionFactor = mat.transmission
                ? mat.transmission->transmissionFactor
                : 0.0f;
            TextureHandle transmissionMap = localTextures[whiteTextureIndex];
            if (mat.transmission && mat.transmission->transmissionTexture.has_value()) {
                const size_t texIndex = mat.transmission->transmissionTexture->textureIndex;
                if (texIndex < gltf.textures.size() && gltf.textures[texIndex].imageIndex.has_value()) {
                    transmissionMap = resolveImageTexture(gltf.textures[texIndex].imageIndex.value(),
                        TextureFormat::RGBA8_UNorm, transmissionMap);
                }
            }

            const bool isTransparent = mat.alphaMode == fastgltf::AlphaMode::Blend
                || transmissionFactor > 0.0f;
            const bool isMasked = !isTransparent && mat.alphaMode == fastgltf::AlphaMode::Mask;

            MaterialAsset materialAsset{};
            materialAsset.name = std::string(mat.name);
            materialAsset.albedoMap = albedoMap;
            materialAsset.normalMap = normalMap;
            materialAsset.pbrMap = pbrMap;
            materialAsset.emissiveMap = emissiveMap;
            materialAsset.transmissionMap = transmissionMap;
            materialAsset.baseColor = baseColor;
            materialAsset.emissiveFactor = emissiveFactor;
            materialAsset.metallic = metallic;
            materialAsset.roughness = roughness;
            materialAsset.normalScale = mat.normalTexture ? mat.normalTexture->scale : 1.0f;
            materialAsset.renderQueue = isTransparent ? RenderQueue::Transparent : RenderQueue::Opaque;
            materialAsset.alphaCutoff = isMasked ? mat.alphaCutoff : 0.0f;
            materialAsset.transmissionFactor = transmissionFactor;
            materialAsset.pipelineState.shaderProgram = isTransparent
                ? ShaderProgram::PbrForward
                : ShaderProgram::PbrGBuffer;
            materialAsset.pipelineState.renderPass = isTransparent
                ? RenderPassClass::Forward
                : RenderPassClass::GBuffer;
            materialAsset.pipelineState.topology = PrimitiveTopology::TriangleList;
            materialAsset.pipelineState.polygonMode = PolygonMode::Fill;
            materialAsset.pipelineState.cullMode = mat.doubleSided ? CullMode::None : CullMode::Back;
            materialAsset.pipelineState.frontFace = FrontFace::Clockwise;
            materialAsset.pipelineState.blendMode = isTransparent
                ? BlendMode::AlphaBlend
                : BlendMode::Opaque;
            materialAsset.pipelineState.depthTest = true;
            materialAsset.pipelineState.depthCompare = isTransparent
                ? DepthCompare::LessOrEqual
                : DepthCompare::Less;
            materialAsset.pipelineState.colorWriteMask = ColorWriteAll;
            materialAsset.pipelineState.depthWrite = !isTransparent;

            addMaterial(materialAsset);
        }

        const bool needsDefaultMaterial = gltf.materials.empty() || std::any_of(
            gltf.meshes.begin(), gltf.meshes.end(), [](const fastgltf::Mesh& mesh) {
                return std::any_of(mesh.primitives.begin(), mesh.primitives.end(),
                    [](const fastgltf::Primitive& primitive) { return !primitive.materialIndex.has_value(); });
            });
        const int defaultMaterialIndex = needsDefaultMaterial
            ? static_cast<int>(model->materials.size())
            : -1;

        if (needsDefaultMaterial) {

            MaterialAsset materialAsset{};
            materialAsset.name = "Default Material";
            materialAsset.albedoMap = localTextures[whiteTextureIndex];
            materialAsset.normalMap = localTextures[flatNormalIndex];
            materialAsset.pbrMap = localTextures[flatPbrIndex];
            materialAsset.emissiveMap = localTextures[whiteTextureIndex];
            materialAsset.transmissionMap = localTextures[whiteTextureIndex];
            materialAsset.metallic = 1.0f;
            materialAsset.renderQueue = RenderQueue::Opaque;
            materialAsset.alphaCutoff = 0.0f;
            materialAsset.transmissionFactor = 0.0f;
            materialAsset.pipelineState.shaderProgram = ShaderProgram::PbrGBuffer;
            materialAsset.pipelineState.renderPass = RenderPassClass::GBuffer;
            materialAsset.pipelineState.topology = PrimitiveTopology::TriangleList;
            materialAsset.pipelineState.polygonMode = PolygonMode::Fill;
            materialAsset.pipelineState.cullMode = CullMode::Back;
            materialAsset.pipelineState.frontFace = FrontFace::Clockwise;
            materialAsset.pipelineState.blendMode = BlendMode::Opaque;
            materialAsset.pipelineState.depthTest = true;
            materialAsset.pipelineState.depthCompare = DepthCompare::Less;
            materialAsset.pipelineState.colorWriteMask = ColorWriteAll;
            materialAsset.pipelineState.depthWrite = true;

            addMaterial(materialAsset);
        }

        model->ownedTextures = std::move(localTextures);

        // 3. Load Geometry & Map Submeshes to Nodes
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        int subMeshGlobalIndex = 0;

        for (size_t i = 0; i < gltf.meshes.size(); ++i) {
            auto& mesh = gltf.meshes[i];
            model->meshToSubMeshes[i] = {};

            for (auto& primitive : mesh.primitives) {
                SubMesh subMesh{};
                subMesh.indexStart = static_cast<uint32_t>(indices.size());
                subMesh.materialIndex = primitive.materialIndex.has_value()
                    ? static_cast<int>(*primitive.materialIndex)
                    : defaultMaterialIndex;

                uint32_t globalVertexOffset = static_cast<uint32_t>(vertices.size());

                auto posIt = primitive.findAttribute("POSITION");
                if (posIt != primitive.attributes.end()) {
                    auto& accessor = gltf.accessors[posIt->accessorIndex];
                    size_t initialVtxCount = vertices.size();
                    vertices.resize(initialVtxCount + accessor.count);

                    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, accessor, [&](glm::vec3 v, size_t idx) {
                        Vertex& vertex = vertices[initialVtxCount + idx];
                        vertex.pos = v;
                        // Material base color is applied in the shader. Vertex color
                        // defaults to white and must never duplicate that factor.
                        vertex.color = glm::vec3(1.0f);
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

                    auto colorIt = primitive.findAttribute("COLOR_0");
                    if (colorIt != primitive.attributes.end()) {
                        auto& colorAccessor = gltf.accessors[colorIt->accessorIndex];
                        if (colorAccessor.type == fastgltf::AccessorType::Vec4) {
                            fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, colorAccessor,
                                [&](glm::vec4 color, size_t idx) {
                                    vertices[initialVtxCount + idx].color = glm::vec3(color);
                                });
                        }
                        else if (colorAccessor.type == fastgltf::AccessorType::Vec3) {
                            fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, colorAccessor,
                                [&](glm::vec3 color, size_t idx) {
                                    vertices[initialVtxCount + idx].color = color;
                                });
                        }
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
