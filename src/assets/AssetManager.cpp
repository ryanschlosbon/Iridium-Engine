#include "assets/AssetManager.h"
#include "assets/BuiltInAssets.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/model/ModelProduct.h"
#include "assets/model/ModelRuntimeProduct.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Iridium {

    namespace {

        CookedArtifact readCookedArtifactFile(
            const std::filesystem::path& path) {
            CookedArtifactBlob blob =
                readCookedArtifactBlobFile(path);
            CookedArtifactReadResult decoded =
                readCookedArtifact(
                    blob.bytes,
                    blob.artifactHash);
            if (!decoded.valid()) {
                std::string message =
                    "Cooked model container validation failed";
                for (const CookDiagnostic& diagnostic :
                    decoded.diagnostics) {
                    if (diagnostic.severity ==
                        CookDiagnosticSeverity::Error) {
                        message += ": " +
                            diagnostic.code + " " +
                            diagnostic.message;
                    }
                }
                throw std::runtime_error(message);
            }
            return std::move(*decoded.artifact);
        }

    } // namespace

    AssetManager::AssetManager(IRenderBackend* backend)
        : renderBackend(backend) {}

    AssetManager::~AssetManager() {
        std::set<MaterialHandle> freedMaterials;
        std::set<TextureHandle> freedTextures;
        std::set<GeometryHandle> freedGeometry;

        for (auto& pair : cookedModelCache) {
            auto asset = pair.second;
            if (!asset->ownsMaterials) continue;
            for (const MaterialBinding& binding : asset->materials) {
                if (binding.material.isValid() &&
                    freedMaterials.insert(binding.material).second) {
                    renderBackend->freeMaterial(binding.material);
                }
            }
        }

        for (auto& pair : cookedModelCache) {
            auto asset = pair.second;
            if (!asset->ownsTextures) continue;
            for (auto& textureHandle : asset->ownedTextures) {
                if (textureHandle.isValid() &&
                    freedTextures.insert(textureHandle).second) {
                    renderBackend->freeTexture(textureHandle);
                }
            }
        }
        for (const auto& [guid, thumbnail] :
            editorThumbnails_) {
            (void)guid;
            if (thumbnail.texture.isValid() &&
                freedTextures.insert(
                    thumbnail.texture).second) {
                renderBackend->freeTexture(
                    thumbnail.texture);
            }
        }
        if (editorDetailThumbnail_
                .texture.isValid() &&
            freedTextures.insert(
                editorDetailThumbnail_
                    .texture).second) {
            renderBackend->freeTexture(
                editorDetailThumbnail_
                    .texture);
        }
        for (TextureHandle texture : ownedEnvironmentTextures_) {
            if (texture.isValid() && freedTextures.insert(texture).second)
                renderBackend->freeTexture(texture);
        }

        for (auto& pair : cookedModelCache) {
            auto asset = pair.second;
            if (asset->ownsGeometry && asset->geometry.isValid() &&
                freedGeometry.insert(asset->geometry).second) {
                renderBackend->freeGeometry(asset->geometry);
            }
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

    LoadedEnvironmentAsset AssetManager::loadEnvironmentFromCookedArtifact(
        const CookedArtifact& artifact) {
        const CookedEnvironmentReadResult decoded =
            readCookedEnvironmentProduct(artifact);
        if (!decoded.valid()) {
            std::string message = "Cooked environment artifact validation failed";
            for (const CookDiagnostic& diagnostic : decoded.diagnostics)
                if (diagnostic.severity == CookDiagnosticSeverity::Error)
                    message += ": " + diagnostic.code + " " + diagnostic.message;
            throw std::runtime_error(message);
        }
        const CookedEnvironmentProductData& product = *decoded.data;
        std::vector<TextureHandle> allocated;
        const auto upload = [&](const EnvironmentImageProductDesc& image,
            std::span<const std::byte> payload, bool cube) {
            TextureDesc desc{};
            desc.width = image.width;
            desc.height = image.height;
            desc.format = image.format;
            desc.usageClass = TextureUsageClass::Environment;
            desc.mipLevels = image.mipLevels;
            desc.arrayLayers = image.arrayLayers;
            desc.topology = cube ? TextureTopology::Cube : TextureTopology::Texture2D;
            desc.sampler.addressU = SamplerAddressMode::ClampToEdge;
            desc.sampler.addressV = SamplerAddressMode::ClampToEdge;
            desc.sampler.addressW = SamplerAddressMode::ClampToEdge;
            desc.sampler.maxLod = image.mipLevels - 1u;
            TextureHandle handle = renderBackend->allocateTexture(desc, payload);
            allocated.push_back(handle);
            return handle;
        };
        try {
            EnvironmentLightingHandles handles{
                .radiance = upload(product.manifest.radiance,
                    product.radiance, true),
                .irradiance = upload(product.manifest.irradiance,
                    product.irradiance, true),
                .prefilteredSpecular = upload(
                    product.manifest.prefilteredSpecular,
                    product.prefilteredSpecular, true),
                .brdfLut = upload(product.manifest.brdfLut,
                    product.brdfLut, false),
            };
            ownedEnvironmentTextures_.insert(ownedEnvironmentTextures_.end(),
                allocated.begin(), allocated.end());
            return {
                .lighting = handles,
                .assetGuid = artifact.assetGuid,
                .cookKey = artifact.cookKey,
                .manifest = product.manifest,
                .brdfLut = product.brdfLut,
            };
        } catch (...) {
            for (TextureHandle handle : allocated) renderBackend->freeTexture(handle);
            throw;
        }
    }

    LoadedEnvironmentAsset
        AssetManager::loadEnvironmentFromCookedArtifactFile(
            const std::filesystem::path& path) {
        return loadEnvironmentFromCookedArtifact(readCookedArtifactFile(path));
    }

    void AssetManager::releaseEnvironment(
        EnvironmentLightingHandles lighting) {
        const std::array handles{ lighting.radiance, lighting.irradiance,
            lighting.prefilteredSpecular, lighting.brdfLut };
        for (TextureHandle handle : handles) {
            const auto owned = std::ranges::find(
                ownedEnvironmentTextures_, handle);
            if (owned == ownedEnvironmentTextures_.end()) continue;
            renderBackend->freeTexture(handle);
            ownedEnvironmentTextures_.erase(owned);
        }
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

    std::shared_ptr<ModelAsset> AssetManager::loadModelFromCookedArtifact(
        const CookedArtifact& artifact,
        std::span<const RuntimeMaterialBinding> materials) {
        if (const auto cached = cookedModelCache.find(artifact.assetGuid);
            cached != cookedModelCache.end()) {
            if (cached->second->artifactCookKey == artifact.cookKey) {
                return cached->second;
            }
            throw std::runtime_error(
                "Cooked model revision replacement is owned by M3.5 hot publish.");
        }

        const CookedModelReadResult decoded =
            readCookedModelProduct(artifact);
        if (!decoded.valid()) {
            std::string message = "Cooked model artifact validation failed";
            for (const CookDiagnostic& diagnostic : decoded.diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }
        RuntimeModelCpuResult runtime =
            makeRuntimeModelCpuData(*decoded.data);
        if (!runtime.valid()) {
            std::string message = "Cooked model runtime conversion failed";
            for (const CookDiagnostic& diagnostic : runtime.diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }
        ResolvedRuntimeModelCpuResult resolved =
            resolveRuntimeModelMaterials(
                std::move(*runtime.data), materials);
        if (!resolved.valid()) {
            std::string message =
                "Cooked model material resolution failed";
            for (const CookDiagnostic& diagnostic : resolved.diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }

        auto model = std::make_shared<ModelAsset>();
        model->filePath = "asset://" + artifact.assetGuid.toString();
        model->assetGuid = artifact.assetGuid;
        model->artifactCookKey = artifact.cookKey;
        model->transparencyExecutionMode =
            resolved.data->geometry.transparencyExecutionMode;
        model->ownsMaterials = false;
        model->ownsTextures = false;
        model->subMeshes =
            std::move(resolved.data->geometry.primitives);
        model->materials = std::move(resolved.data->materials);
        model->totalIndices =
            static_cast<uint32_t>(
                resolved.data->geometry.indices.size());
        uploadToGPU(model.get(), resolved.data->geometry.vertices,
            resolved.data->geometry.indices);
        cookedModelCache.emplace(artifact.assetGuid, model);
        if (onModelLoadedCallback) onModelLoadedCallback(model);
        return model;
    }

    std::shared_ptr<ModelAsset> AssetManager::loadBuiltInCubeModel() {
        if (const auto cached = cookedModelCache.find(kBuiltInCubeAssetGuid);
            cached != cookedModelCache.end()) {
            return cached->second;
        }

        struct Face {
            glm::vec3 normal;
            glm::vec3 tangent;
            glm::vec3 bitangent;
        };
        constexpr std::array faces{
            Face{ { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f } },
            Face{ { 0.0f, 0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f } },
            Face{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f, 0.0f } },
            Face{ { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
                { 0.0f, 1.0f, 0.0f } },
            Face{ { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f } },
            Face{ { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f } },
        };
        constexpr std::array<glm::vec2, 4> corners{
            glm::vec2{ -0.5f, -0.5f }, glm::vec2{ 0.5f, -0.5f },
            glm::vec2{ 0.5f, 0.5f }, glm::vec2{ -0.5f, 0.5f },
        };
        constexpr std::array<glm::vec2, 4> uvs{
            glm::vec2{ 0.0f, 0.0f }, glm::vec2{ 1.0f, 0.0f },
            glm::vec2{ 1.0f, 1.0f }, glm::vec2{ 0.0f, 1.0f },
        };
        constexpr std::array<uint32_t, 6> faceIndices{ 0, 2, 1, 0, 3, 2 };

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(faces.size() * corners.size());
        indices.reserve(faces.size() * faceIndices.size());
        for (const Face& face : faces) {
            const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());
            for (size_t index = 0; index < corners.size(); ++index) {
                const glm::vec2 corner = corners[index];
                vertices.push_back({
                    .pos = face.normal * 0.5f + face.tangent * corner.x +
                        face.bitangent * corner.y,
                    .color = glm::vec4(1.0f),
                    .normal = face.normal,
                    .uv0 = uvs[index],
                    .tangent = glm::vec4(face.tangent, 1.0f),
                    .uv1 = uvs[index],
                });
            }
            for (uint32_t index : faceIndices) {
                indices.push_back(firstVertex + index);
            }
        }

        CanonicalMaterialAsset material{};
        material.name = "Built-in Cube Material";
        material.packed.closureClass = static_cast<uint32_t>(
            MaterialClosureClass::StandardDeferred);
        material.packed.transparencyPolicy =
            packTransparencyPolicyWord(CompiledTransparencyPolicy{});
        material.packed.textureIndices.fill(
            PackedGpuMaterial::InvalidTextureIndex);
        material.packed.baseColorFactor = { 0.62f, 0.68f, 0.78f, 1.0f };
        material.packed.metallicRoughnessIorSpecular = {
            0.0f, 0.55f, 1.5f, 1.0f };
        material.packed.specularColorNormalScale = {
            1.0f, 1.0f, 1.0f, 1.0f };
        material.packed.diffuseFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        material.packed.specularGlossinessFactorGloss = {
            1.0f, 1.0f, 1.0f, 1.0f };
        material.packed.emissiveFactorStrength = { 0.0f, 0.0f, 0.0f, 1.0f };
        material.packed.surfaceParameters = { 1.0f, 0.5f, 0.0f, 0.0f };

        auto model = std::make_shared<ModelAsset>();
        model->filePath = "builtin://cube";
        model->assetGuid = kBuiltInCubeAssetGuid;
        model->artifactCookKey = "builtin-cube-v1";
        model->totalIndices = static_cast<uint32_t>(indices.size());
        model->subMeshes.push_back({
            .indexStart = 0,
            .indexCount = model->totalIndices,
            .materialIndex = 0,
            .sourcePrimitiveGuid = kBuiltInCubePrimitiveGuid,
            .primitiveGuid = kBuiltInCubePrimitiveGuid,
            .materialGuid = kBuiltInCubeMaterialGuid,
            .attributeMask = ModelAttributePosition | ModelAttributeColor0 |
                ModelAttributeNormal | ModelAttributeTexCoord0 |
                ModelAttributeTangent | ModelAttributeTexCoord1,
            .coverage = static_cast<uint8_t>(ModelCoverage::Opaque),
            .boundsMin = glm::vec3(-0.5f),
            .boundsMax = glm::vec3(0.5f),
            .boundsSphereCenter = glm::vec3(0.0f),
            .boundsSphereRadius = 0.8660254f,
        });

        std::vector<TextureHandle> allocatedTextures;
        const auto remember = [&allocatedTextures](TextureHandle texture) {
            allocatedTextures.push_back(texture);
            return texture;
        };
        try {
            const TextureHandle white = remember(createDefaultTexture());
            const TextureHandle normal =
                remember(createDefaultNormalTexture());
            const TextureHandle linearData =
                remember(createDefaultPbrTexture());
            material.textures.fill(white);
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Normal)] = normal;
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::ClearcoatNormal)] = normal;
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::MetallicRoughness)] = linearData;
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Occlusion)] = linearData;
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Transmission)] = linearData;
            material.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Thickness)] = linearData;
            model->materials.push_back(
                renderBackend->allocateCanonicalMaterial(material));
            uploadToGPU(model.get(), vertices, indices);
            model->ownedTextures = std::move(allocatedTextures);
        } catch (...) {
            for (const MaterialBinding& binding : model->materials) {
                if (binding.material.isValid()) {
                    renderBackend->freeMaterial(binding.material);
                }
            }
            for (TextureHandle texture : allocatedTextures) {
                if (texture.isValid()) {
                    renderBackend->freeTexture(texture);
                }
            }
            throw;
        }
        cookedModelCache.emplace(kBuiltInCubeAssetGuid, model);
        if (onModelLoadedCallback) onModelLoadedCallback(model);
        return model;
    }

    std::shared_ptr<ModelAsset>
        AssetManager::loadCompleteModelFromCookedArtifact(
            const CookedArtifact& artifact,
            std::span<const RuntimeTextureViewBinding>
                textureViews,
            const RuntimeMaterialFallbacks& fallbacks) {
        if (const auto cached =
                cookedModelCache.find(artifact.assetGuid);
            cached != cookedModelCache.end()) {
            if (cached->second->artifactCookKey ==
                artifact.cookKey) {
                return cached->second;
            }
            throw std::runtime_error(
                "Cooked model revision replacement is owned by M3.5 hot publish.");
        }

        const CookedModelReadResult decoded =
            readCookedModelProduct(artifact);
        if (!decoded.valid()) {
            std::string message =
                "Complete cooked model artifact validation failed";
            for (const CookDiagnostic& diagnostic :
                decoded.diagnostics) {
                if (diagnostic.severity ==
                    CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code +
                        " " + diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }
        return loadCompleteModelFromCookedProduct(
            artifact, *decoded.data, textureViews,
            fallbacks, true);
    }

    std::shared_ptr<ModelAsset>
        AssetManager::loadCompleteModelFromCookedProduct(
            const CookedArtifact& artifact,
            const CookedModelProductData& product,
            std::span<const RuntimeTextureViewBinding>
                textureViews,
            const RuntimeMaterialFallbacks& fallbacks,
            bool notifyLoaded) {
        RuntimeModelCpuResult geometry =
            makeRuntimeModelCpuData(product, false);
        if (!geometry.valid()) {
            throw std::runtime_error(
                "Complete cooked model geometry conversion failed.");
        }
        RuntimeCanonicalMaterialResult canonical =
            makeRuntimeCanonicalMaterials(product,
                textureViews, fallbacks, false);
        if (!canonical.valid()) {
            std::string message =
                "Complete cooked model material reconstruction failed";
            for (const CookDiagnostic& diagnostic :
                canonical.diagnostics) {
                if (diagnostic.severity ==
                    CookDiagnosticSeverity::Error) {
                    message += ": " + diagnostic.code +
                        " " + diagnostic.message;
                }
            }
            throw std::runtime_error(message);
        }

        std::vector<RuntimeMaterialBinding>
            runtimeBindings;
        runtimeBindings.reserve(canonical.materials.size());
        try {
            for (const RuntimeCanonicalMaterial& material :
                canonical.materials) {
                runtimeBindings.push_back({
                    .materialGuid = material.materialGuid,
                    .transparency = material.transparency,
                    .binding =
                        renderBackend->allocateCanonicalMaterial(
                            material.asset),
                });
            }
        } catch (...) {
            for (const RuntimeMaterialBinding& binding :
                runtimeBindings) {
                if (binding.binding.material.isValid()) {
                    renderBackend->freeMaterial(
                        binding.binding.material);
                }
            }
            throw;
        }

        ResolvedRuntimeModelCpuResult resolved =
            resolveRuntimeModelMaterials(
                std::move(*geometry.data),
                runtimeBindings);
        if (!resolved.valid()) {
            for (const RuntimeMaterialBinding& binding :
                runtimeBindings) {
                if (binding.binding.material.isValid()) {
                    renderBackend->freeMaterial(
                        binding.binding.material);
                }
            }
            throw std::runtime_error(
                "Complete cooked model material GUID resolution failed.");
        }
        for (const RuntimeMaterialBinding& binding : runtimeBindings) {
            const bool retained = std::ranges::any_of(
                resolved.data->materials,
                [&binding](const MaterialBinding& candidate) {
                    return candidate.material == binding.binding.material;
                });
            if (!retained && binding.binding.material.isValid()) {
                renderBackend->freeMaterial(binding.binding.material);
            }
        }

        auto model = std::make_shared<ModelAsset>();
        model->filePath =
            "asset://" + artifact.assetGuid.toString();
        model->assetGuid = artifact.assetGuid;
        model->artifactCookKey = artifact.cookKey;
        model->transparencyExecutionMode =
            resolved.data->geometry.transparencyExecutionMode;
        model->ownsMaterials = true;
        model->ownsTextures = false;
        model->subMeshes =
            std::move(resolved.data->geometry.primitives);
        model->materials =
            std::move(resolved.data->materials);
        model->totalIndices = static_cast<uint32_t>(
            resolved.data->geometry.indices.size());
        try {
            uploadToGPU(model.get(),
                resolved.data->geometry.vertices,
                resolved.data->geometry.indices);
        } catch (...) {
            for (const MaterialBinding& binding :
                model->materials) {
                if (binding.material.isValid()) {
                    renderBackend->freeMaterial(
                        binding.material);
                }
            }
            throw;
        }
        cookedModelCache.emplace(artifact.assetGuid, model);
        if (notifyLoaded && onModelLoadedCallback) {
            onModelLoadedCallback(model);
        }
        return model;
    }

    std::shared_ptr<ModelAsset>
        AssetManager::loadSelfContainedModelFromCookedArtifact(
            const CookedArtifact& artifact) {
        if (const auto cached =
                cookedModelCache.find(artifact.assetGuid);
            cached != cookedModelCache.end()) {
            if (cached->second->artifactCookKey ==
                artifact.cookKey) {
                return cached->second;
            }
            throw std::runtime_error(
                "Cooked model revision replacement is owned by M3.5 hot publish.");
        }
        const CookedModelReadResult decoded =
            readCookedModelProduct(artifact);
        if (!decoded.valid()) {
            throw std::runtime_error(
                "Self-contained cooked model validation failed.");
        }
        return loadSelfContainedModelFromCookedProduct(
            artifact, *decoded.data);
    }

    std::shared_ptr<ModelAsset>
        AssetManager::loadSelfContainedModelFromCookedProduct(
            const CookedArtifact& artifact,
            const CookedModelProductData& product) {
        std::vector<TextureHandle> allocatedTextures;
        const auto remember = [&allocatedTextures](
            TextureHandle texture) {
            allocatedTextures.push_back(texture);
            return MaterialTextureBinding{
                texture,
                SamplerHandle::fromParts(
                    texture.getIndex(),
                    texture.getGeneration()),
            };
        };
        try {
            const RuntimeMaterialFallbacks fallbacks{
                .white = remember(createDefaultTexture()),
                .normal =
                    remember(createDefaultNormalTexture()),
                .linearData =
                    remember(createDefaultPbrTexture()),
            };

            struct AllocatedView {
                uint32_t textureViewIndex = 0;
                SamplerDesc sampler;
                MaterialTextureBinding binding;
            };
            std::vector<AllocatedView> allocatedViews;
            std::vector<RuntimeTextureViewBinding>
                runtimeViews;
            for (const CookedModelMaterial& material :
                product.materials) {
                for (const CookedModelTextureBinding& cooked :
                    material.textureBindings) {
                    const CompiledTextureOperation& operation =
                        material.compiled.textureOperations.at(
                            cooked.operationIndex);
                    const CookedModelTextureView& view =
                        product.textureViews.at(
                            cooked.textureViewIndex);
                    const MaterialTextureCompatibilityPlan plan =
                        planMaterialTextureCompatibility(
                            operation.sampler,
                            view.manifest.width,
                            view.manifest.height);
                    auto existing = std::ranges::find_if(
                        allocatedViews,
                        [&cooked, &plan](
                            const AllocatedView& value) {
                            return value.textureViewIndex ==
                                    cooked.textureViewIndex &&
                                value.sampler == plan.sampler;
                        });
                    MaterialTextureBinding binding;
                    if (existing != allocatedViews.end()) {
                        binding = existing->binding;
                    } else {
                        TextureDesc desc{
                            .width = view.manifest.width,
                            .height = view.manifest.height,
                            .format =
                                view.manifest.storageFormat,
                            .mipLevels =
                                static_cast<uint32_t>(
                                    view.manifest.mips.size()),
                            .sampler = plan.sampler,
                        };
                        binding = remember(
                            renderBackend->allocateTexture(
                                desc, view.payload));
                        binding.reconstructNormalZ =
                            view.manifest.semantic ==
                                TextureSemantic::Normal &&
                            view.manifest.storageFormat ==
                                TextureFormat::BC5_UNorm;
                        allocatedViews.push_back({
                            .textureViewIndex =
                                cooked.textureViewIndex,
                            .sampler = plan.sampler,
                            .binding = binding,
                        });
                    }
                    runtimeViews.push_back({
                        .materialGuid =
                            material.materialGuid,
                        .operationIndex =
                            cooked.operationIndex,
                        .textureGuid =
                            cooked.textureGuid,
                        .binding = binding,
                    });
                }
            }
            std::shared_ptr<ModelAsset> model =
                loadCompleteModelFromCookedProduct(
                    artifact, product,
                    runtimeViews, fallbacks, false);
            model->ownedTextures =
                std::move(allocatedTextures);
            model->ownsTextures = true;
            if (onModelLoadedCallback) {
                onModelLoadedCallback(model);
            }
            return model;
        } catch (...) {
            for (TextureHandle texture :
                allocatedTextures) {
                if (texture.isValid()) {
                    renderBackend->freeTexture(texture);
                }
            }
            throw;
        }
    }

    std::shared_ptr<ModelAsset>
        AssetManager::loadSelfContainedModelFromCookedArtifactFile(
            const std::filesystem::path& path) {
        const CookedArtifact artifact =
            readCookedArtifactFile(path);
        std::shared_ptr<ModelAsset> model =
            loadSelfContainedModelFromCookedArtifact(
                artifact);
        model->filePath =
            path.lexically_normal().string();
        return model;
    }

    std::shared_ptr<ModelAsset>
        AssetManager::replaceSelfContainedModelFromCookedArtifact(
            const CookedArtifact& artifact) {
        const CookedModelReadResult decoded =
            readCookedModelProduct(artifact);
        if (!decoded.valid()) {
            throw std::runtime_error(
                "Self-contained cooked model replacement validation failed.");
        }
        return replaceSelfContainedModelFromCookedProduct(
            artifact, *decoded.data);
    }

    std::shared_ptr<ModelAsset>
        AssetManager::replaceSelfContainedModelFromCookedProduct(
            const CookedArtifact& artifact,
            const CookedModelProductData& product) {
        const auto found =
            cookedModelCache.find(artifact.assetGuid);
        if (found == cookedModelCache.end()) {
            return loadSelfContainedModelFromCookedProduct(
                artifact, product);
        }
        std::shared_ptr<ModelAsset> stable = found->second;
        if (stable->artifactCookKey == artifact.cookKey) {
            return stable;
        }

        auto previousNode =
            cookedModelCache.extract(found);
        auto loadedCallback =
            std::move(onModelLoadedCallback);
        onModelLoadedCallback = {};
        std::shared_ptr<ModelAsset> replacement;
        try {
            replacement =
                loadSelfContainedModelFromCookedProduct(
                    artifact, product);
        } catch (...) {
            onModelLoadedCallback =
                std::move(loadedCallback);
            cookedModelCache.insert(
                std::move(previousNode));
            throw;
        }
        onModelLoadedCallback =
            std::move(loadedCallback);

        using std::swap;
        swap(*stable, *replacement);
        cookedModelCache[artifact.assetGuid] =
            stable;

        if (replacement->ownsMaterials) {
            for (const MaterialBinding& binding :
                replacement->materials) {
                if (binding.material.isValid()) {
                    renderBackend->freeMaterial(
                        binding.material);
                }
            }
            replacement->ownsMaterials = false;
        }
        if (replacement->ownsTextures) {
            for (TextureHandle texture :
                replacement->ownedTextures) {
                if (texture.isValid()) {
                    renderBackend->freeTexture(texture);
                }
            }
            replacement->ownsTextures = false;
        }
        if (replacement->ownsGeometry &&
            replacement->geometry.isValid()) {
            renderBackend->freeGeometry(
                replacement->geometry);
            replacement->ownsGeometry = false;
        }
        if (onModelLoadedCallback) {
            onModelLoadedCallback(stable);
        }
        return stable;
    }

    std::shared_ptr<ModelAsset>
        AssetManager::replaceSelfContainedModelFromCookedArtifactFile(
            const std::filesystem::path& path) {
        const CookedArtifact artifact =
            readCookedArtifactFile(path);
        std::shared_ptr<ModelAsset> model =
            replaceSelfContainedModelFromCookedArtifact(
                artifact);
        model->filePath =
            path.lexically_normal().string();
        return model;
    }

    std::shared_ptr<ModelAsset>
        AssetManager::findCookedModel(
            AssetGuid assetGuid) const {
        const auto found =
            cookedModelCache.find(assetGuid);
        return found != cookedModelCache.end()
            ? found->second
            : std::shared_ptr<ModelAsset>{};
    }

    std::optional<MaterialBinding>
        AssetManager::findCookedMaterial(
            AssetGuid materialGuid) const {
        const auto runtime = findCookedMaterialRuntime(materialGuid);
        return runtime
            ? std::optional<MaterialBinding>{ runtime->binding }
            : std::nullopt;
    }

    std::optional<CookedMaterialRuntimeBinding>
        AssetManager::findCookedMaterialRuntime(
            AssetGuid materialGuid) const {
        for (const auto& [guid, model] :
            cookedModelCache) {
            (void)guid;
            if (!model) continue;
            for (const SubMesh& primitive :
                model->subMeshes) {
                if (primitive.materialGuid !=
                        materialGuid ||
                    primitive.materialIndex < 0 ||
                    static_cast<size_t>(
                        primitive.materialIndex) >=
                        model->materials.size()) {
                    continue;
                }
                return CookedMaterialRuntimeBinding{
                    .materialGuid = materialGuid,
                    .transparency = primitive.transparency,
                    .transparencyExecutionMode =
                        model->transparencyExecutionMode,
                    .binding = model->materials[
                        static_cast<size_t>(
                            primitive.materialIndex)],
                };
            }
        }
        return std::nullopt;
    }

    std::span<const MaterialProvenance> AssetManager::getMaterialProvenance(
        const ModelAsset& model) const {
        const auto found = materialProvenanceCache.find(&model);
        if (found == materialProvenanceCache.end()) return {};
        return found->second;
    }

    void* AssetManager::getMaterialTexturePreview(TextureHandle texture) const {
        return renderBackend != nullptr && texture.isValid()
            ? renderBackend->getEditorTextureID(texture) : nullptr;
    }

    std::optional<AssetGuid>
        AssetManager::publishEditorThumbnail(
            AssetGuid assetGuid,
            uint32_t width,
            uint32_t height,
            std::span<const std::byte> rgba8) {
        return publishEditorThumbnailInternal(
            assetGuid, width, height,
            rgba8, false);
    }

    void AssetManager::
        publishEditorDetailThumbnail(
            AssetGuid assetGuid,
            uint32_t width,
            uint32_t height,
            std::span<const std::byte> rgba8) {
        (void)publishEditorThumbnailInternal(
            assetGuid, width, height,
            rgba8, true);
    }

    std::optional<AssetGuid>
        AssetManager::
        publishEditorThumbnailInternal(
            AssetGuid assetGuid,
            uint32_t width,
            uint32_t height,
            std::span<const std::byte> rgba8,
            bool detail) {
        if (!renderBackend ||
            assetGuid.isNil() ||
            width == 0 || height == 0 ||
            rgba8.size() !=
                static_cast<size_t>(width) *
                    height * 4) {
            throw std::invalid_argument(
                "Editor thumbnail publication requires a GUID and complete RGBA8 pixels.");
        }
        TextureDesc description{
            .width = width,
            .height = height,
            .format = TextureFormat::RGBA8_sRGB,
            .usageClass =
                TextureUsageClass::Sampled2D,
            .mipLevels = 1,
            .sampler = {
                .minFilter = FilterMode::Linear,
                .magFilter = FilterMode::Linear,
                .mipmapFilter =
                    MipmapFilterMode::Nearest,
                .addressU =
                    SamplerAddressMode::ClampToEdge,
                .addressV =
                    SamplerAddressMode::ClampToEdge,
                .addressW =
                    SamplerAddressMode::ClampToEdge,
                .maxLod = 0,
            },
        };
        const TextureHandle texture =
            renderBackend->allocateTexture(
                description, rgba8);

        std::optional<AssetGuid> evicted;
        if (detail) {
            if (editorDetailThumbnail_
                    .texture.isValid()) {
                renderBackend->freeTexture(
                    editorDetailThumbnail_
                        .texture);
            }
            editorDetailThumbnailGuid_ =
                assetGuid;
            editorDetailThumbnail_ = {
                .texture = texture,
                .lastUseSerial =
                    ++editorThumbnailSerial_,
            };
            return evicted;
        }
        const auto existing =
            editorThumbnails_.find(assetGuid);
        if (existing !=
            editorThumbnails_.end()) {
            if (existing->second.texture.isValid()) {
                renderBackend->freeTexture(
                    existing->second.texture);
            }
            existing->second = {
                .texture = texture,
                .lastUseSerial =
                    ++editorThumbnailSerial_,
            };
            return evicted;
        }
        if (editorThumbnails_.size() >=
            EditorThumbnailCapacity) {
            const auto oldest =
                std::ranges::min_element(
                    editorThumbnails_,
                    [](const auto& lhs,
                        const auto& rhs) {
                        if (lhs.second
                                .lastUseSerial !=
                            rhs.second
                                .lastUseSerial) {
                            return lhs.second
                                .lastUseSerial <
                                rhs.second
                                .lastUseSerial;
                        }
                        return lhs.first <
                            rhs.first;
                    });
            evicted = oldest->first;
            if (oldest->second.texture.isValid()) {
                renderBackend->freeTexture(
                    oldest->second.texture);
            }
            editorThumbnails_.erase(oldest);
        }
        editorThumbnails_.emplace(
            assetGuid,
            EditorThumbnailEntry{
                .texture = texture,
                .lastUseSerial =
                    ++editorThumbnailSerial_,
            });
        return evicted;
    }

    void* AssetManager::getEditorThumbnail(
        AssetGuid assetGuid) {
        const auto found =
            editorThumbnails_.find(assetGuid);
        if (found ==
            editorThumbnails_.end()) {
            return nullptr;
        }
        found->second.lastUseSerial =
            ++editorThumbnailSerial_;
        return renderBackend != nullptr &&
            found->second.texture.isValid()
            ? renderBackend->getEditorTextureID(
                found->second.texture)
            : nullptr;
    }

    void* AssetManager::
        getEditorDetailThumbnail(
            AssetGuid assetGuid) {
        if (editorDetailThumbnailGuid_ !=
                std::optional(assetGuid) ||
            !editorDetailThumbnail_
                .texture.isValid()) {
            return nullptr;
        }
        editorDetailThumbnail_
            .lastUseSerial =
                ++editorThumbnailSerial_;
        return renderBackend != nullptr
            ? renderBackend->getEditorTextureID(
                editorDetailThumbnail_
                    .texture)
            : nullptr;
    }

} // namespace Iridium
