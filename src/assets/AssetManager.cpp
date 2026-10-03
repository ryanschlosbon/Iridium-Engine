#include "assets/AssetManager.h"
#include "assets/BuiltInAssets.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/model/ModelProduct.h"
#include "assets/model/ModelRuntimeProduct.h"
#include "material/MaterialAuthoringPatch.h"
#include "assets/model/MaterialPreviewPolicy.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
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

    AssetManager::AssetManager(IRenderBackend* backend,
        TransparencyExecutionMode runtimeTransparencyExecutionMode,
        uint32_t minimumResidentLodLevel)
        : renderBackend(backend),
          runtimeTransparencyExecutionMode_(
              runtimeTransparencyExecutionMode),
          minimumResidentLodLevel_(minimumResidentLodLevel) {
        if (minimumResidentLodLevel_ >= MaximumGpuSceneLodLevels) {
            throw std::invalid_argument(
                "Asset manager LOD residency floor exceeds the GPU-scene ABI");
        }
    }

    void AssetManager::requestMaterialPreview(AssetGuid document, AssetGuid root,
        const std::map<AssetGuid, SourceMaterial>& sources, const nlohmann::json& settings,
        std::string_view sourceCookKey, const nlohmann::json& publishedSettings) {
        if (sources.empty()) return;
        auto& preview = materialPreviews_[document];
        if (preview.root == root && preview.settings == settings && preview.publishedSettings == publishedSettings && preview.sourceCookKey == sourceCookKey && !preview.sources.empty()) return;
        preview.root = root;
        preview.sources = sources;
        preview.settings = settings;
        preview.publishedSettings = publishedSettings;
        preview.sourceCookKey = sourceCookKey;
        preview.pending = true;
        preview.requestSerial = ++previewRequestSerial_;
    }

    std::shared_ptr<ModelAsset> AssetManager::findMaterialPreview(AssetGuid document) const {
        const auto found = materialPreviews_.find(document);
        if (found == materialPreviews_.end()) return {};
        const auto parent = findCookedModel(found->second.root);
        return parent && parent->artifactCookKey == found->second.cookKey ? found->second.model : nullptr;
    }

    std::string AssetManager::materialPreviewDiagnostic(AssetGuid document) const {
        const auto found = materialPreviews_.find(document);
        return found == materialPreviews_.end() ? std::string{} : found->second.diagnostic;
    }

    void AssetManager::processMaterialPreviews(std::span<const AssetGuid> openDocuments) {
        auto completion = previewCompiler_.poll();
        const auto release = [&](MaterialPreview& preview) {
            for (const auto& binding : preview.ownedBindings) renderBackend->freeMaterial(binding.material);
            preview.ownedBindings.clear();
            preview.canonicalAssets.clear();
            preview.model.reset();
        };
        for (auto it = materialPreviews_.begin(); it != materialPreviews_.end();) {
            if (std::ranges::find(openDocuments, it->first) == openDocuments.end()) {
                release(it->second);
                it = materialPreviews_.erase(it);
                continue;
            }
            auto& preview = it++->second;
            const auto parent = findCookedModel(preview.root);
            const auto input = previewInputs_.find(preview.root);
            if (!parent || input == previewInputs_.end()) continue;
            if (preview.cookKey != parent->artifactCookKey) {
                release(preview);
                preview.pending = true;
                preview.requestSerial = ++previewRequestSerial_;
                preview.cookKey = parent->artifactCookKey;
            }
            const bool completed = completion && completion->serial == preview.requestSerial;
            if (!preview.pending && !completed) continue;
            if (preview.sourceCookKey != input->second.cookKey) {
                preview.diagnostic = "Waiting for matching source material revision after reimport...";
                continue;
            }
            std::vector<MaterialBinding> allocated;
            try {
                if (!completed) {
                    if (previewCompiler_.busy()) continue;
                    auto inputs = input->second;
                    auto policies = applyPreviewPolicySettings(inputs.product, preview.sources, preview.settings);
                    auto compile = [inputs = std::move(inputs), sources = preview.sources,
                        settings = preview.settings, publishedSettings = preview.publishedSettings,
                        executionMode = runtimeTransparencyExecutionMode_]() {
                        CookedModelProductData product = inputs.product;
                        auto views = inputs.views;
                        const auto patches = settings.value("material_overrides", nlohmann::json::object());
                        for (auto& material : product.materials) {
                            const auto source = sources.find(material.materialGuid);
                            if (source == sources.end()) continue;
                            const auto patch = patches.value(material.materialGuid.toString(),
                                nlohmann::json{{"schema_version", 1}, {"values", nlohmann::json::object()}});
                            auto edited = withMaterialAuthoringPatch(source->second, patch);
                            auto compiled = compileSourceMaterial(edited);
                            const auto publishedPatch = publishedSettings.value("material_overrides", nlohmann::json::object()).value(
                                material.materialGuid.toString(), nlohmann::json{{"schema_version", 1}, {"values", nlohmann::json::object()}});
                            const auto baseline = compileSourceMaterial(withMaterialAuthoringPatch(source->second, publishedPatch));
                            if (!compiled.succeeded()) {
                                std::string error = "Preview compilation failed";
                                for (const auto& diagnostic : compiled.diagnostics) error += ": " + diagnostic.message;
                                throw std::runtime_error(error);
                            }
                            // Topology-dependent routing must stay consistent with the
                            // cooked primitive contract. Do not pretend a scalar update
                            // can validate a new closed-volume transport classification.
                            if (!baseline.succeeded()) throw std::runtime_error("Imported source material cannot be previewed");
                            if (compiled.material->standard.alphaMode != material.compiled.standard.alphaMode ||
                                compiled.material->transparency.resolvedClass != baseline.material->transparency.resolvedClass)
                                throw std::runtime_error("Coverage/transport route changed: Apply and reimport is required. Last valid preview retained.");
                            std::vector<CookedModelTextureBinding> remapped;
                            std::vector<RuntimeTextureViewBinding> remappedViews;
                            for (uint32_t operation = 0; operation < compiled.material->textureOperations.size(); ++operation) {
                                const auto semantic = compiled.material->textureOperations[operation].semantic;
                                const auto old = std::ranges::find_if(material.compiled.textureOperations,
                                    [&](const auto& value) { return value.semantic == semantic; });
                                if (old == material.compiled.textureOperations.end())
                                    throw std::runtime_error("New texture bindings require Apply and reimport");
                                const auto oldIndex = static_cast<uint32_t>(old - material.compiled.textureOperations.begin());
                                const auto binding = std::ranges::find_if(material.textureBindings,
                                    [&](const auto& value) { return value.operationIndex == oldIndex; });
                                const auto view = std::ranges::find_if(views, [&](const auto& value) {
                                    return value.materialGuid == material.materialGuid && value.operationIndex == oldIndex;
                                });
                                if (binding == material.textureBindings.end() || view == views.end())
                                    throw std::runtime_error("Preview texture is not resident");
                                remapped.push_back(*binding);
                                remapped.back().operationIndex = operation;
                                remappedViews.push_back(*view);
                                remappedViews.back().operationIndex = operation;
                            }
                            std::erase_if(views, [&](const auto& value) { return value.materialGuid == material.materialGuid; });
                            views.insert(views.end(), remappedViews.begin(), remappedViews.end());
                            material.textureBindings = std::move(remapped);
                            const auto policy = material.compiled.transparency;
                            material.compiled = *compiled.material;
                            material.compiled.transparency = policy;
                        }
                        const auto canonical = makeRuntimeCanonicalMaterials(product, views,
                            inputs.fallbacks, false, executionMode);
                        if (!canonical.valid()) throw std::runtime_error("Preview material packing failed; last valid preview retained");
                        return canonical;
                    };
                    if (previewCompiler_.submit(preview.requestSerial, std::move(compile))) {
                        preview.preparedPrimitivePolicies = std::move(policies);
                        preview.pending = false;
                        preview.diagnostic = "Compiling private preview; showing the last valid material...";
                    }
                    continue;
                }
                if (!completion->diagnostic.empty()) throw std::runtime_error(completion->diagnostic);
                const auto& canonical = completion->result;
                const auto bindPrimitives = [&](ModelAsset& model) {
                    for (auto& primitive : model.subMeshes) {
                        const auto policy = preview.preparedPrimitivePolicies.find(primitive.primitiveGuid);
                        if (policy == preview.preparedPrimitivePolicies.end())
                            throw std::runtime_error("Preview primitive policy is unresolved");
                        const auto material = std::ranges::find_if(canonical.materials, [&](const auto& value) {
                            return value.materialGuid == primitive.materialGuid &&
                                (runtimeTransparencyExecutionMode_ != TransparencyExecutionMode::Classified ||
                                    value.transparency == policy->second);
                        });
                        if (material == canonical.materials.end()) throw std::runtime_error("Preview material slot is unresolved");
                        primitive.transparency = policy->second;
                        primitive.materialIndex = static_cast<int>(material - canonical.materials.begin());
                    }
                };
                bool updateOnly = preview.model && preview.canonicalAssets.size() == canonical.materials.size();
                for (size_t index = 0; updateOnly && index < canonical.materials.size(); ++index) {
                    const auto& before = preview.canonicalAssets[index];
                    const auto& after = canonical.materials[index].asset;
                    updateOnly = before.pipelineState == after.pipelineState && before.textures == after.textures &&
                        before.packed.closureClass == after.packed.closureClass;
                }
                if (updateOnly) {
                    auto rebound = preview.model;
                    if (preview.preparedPrimitivePolicies != preview.publishedPrimitivePolicies) {
                        rebound = std::make_shared<ModelAsset>(*preview.model);
                        bindPrimitives(*rebound);
                    }
                    for (size_t index = 0; index < canonical.materials.size(); ++index) {
                        const auto& next = canonical.materials[index].asset;
                        if (std::memcmp(&preview.canonicalAssets[index].packed, &next.packed, sizeof(PackedGpuMaterial)) != 0)
                            renderBackend->updateCanonicalMaterial(preview.ownedBindings[index].material, next.packed);
                        preview.canonicalAssets[index] = next;
                    }
                    preview.model = std::move(rebound);
                    preview.publishedPrimitivePolicies = preview.preparedPrimitivePolicies;
                    preview.diagnostic = "Private live preview. Apply publishes to shared scene materials; Revert discards draft edits.";
                    continue;
                }
                auto model = std::make_shared<ModelAsset>(*parent);
                model->ownsGeometry = model->ownsMaterials = model->ownsTextures = false;
                model->materials.clear();
                for (const auto& material : canonical.materials) {
                    allocated.push_back(renderBackend->allocateCanonicalMaterial(material.asset));
                    model->materials.push_back(allocated.back());
                }
                bindPrimitives(*model);
                release(preview);
                preview.model = std::move(model);
                preview.ownedBindings = std::move(allocated);
                preview.publishedPrimitivePolicies = preview.preparedPrimitivePolicies;
                for (const auto& material : canonical.materials) preview.canonicalAssets.push_back(material.asset);
                preview.diagnostic = "Private live preview. Apply publishes to shared scene materials; Revert discards draft edits.";
            } catch (const std::exception& error) {
                preview.pending = false;
                for (const auto& binding : allocated) renderBackend->freeMaterial(binding.material);
                preview.diagnostic = error.what();
            }
        }
    }

    AssetManager::~AssetManager() {
        processMaterialPreviews({});
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
            if (!asset->ownsGeometry) continue;
            if (!asset->geometryArena.empty()) {
                renderBackend->freeGeometryArena(asset->geometryArena);
                for (GeometryHandle handle : asset->geometryArena)
                    freedGeometry.insert(handle);
            }
            else if (asset->geometry.isValid() &&
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
        asset->sourceIndexBytes = indices.size() * sizeof(uint32_t);
        asset->arenaIndexBytes = asset->sourceIndexBytes;
        for (SubMesh& primitive : asset->subMeshes) {
            primitive.geometry = asset->geometry;
            primitive.indexFormat = 1;
        }
    }

    void AssetManager::uploadArenaToGPU(ModelAsset* asset,
        const RuntimeModelCpuData& geometry) {
        GeometryArenaAllocation allocation =
            renderBackend->allocateGeometryArena(sizeof(Vertex),
                std::as_bytes(std::span(geometry.vertices)),
                geometry.geometryArena);
        if (!allocation.valid() || allocation.primitiveGeometry.size() !=
                geometry.geometryArena.primitives.size()) {
            if (!allocation.primitiveGeometry.empty())
                renderBackend->freeGeometryArena(
                    allocation.primitiveGeometry);
            throw std::runtime_error(
                "Backend returned an incomplete geometry arena allocation");
        }
        try {
            const auto assignRange = [&](SubMesh& primitive) {
                const GeometryArenaPrimitiveIdentity identity{
                    primitive.sourcePrimitiveGuid, primitive.primitiveGuid };
                const auto found = std::ranges::find(
                    geometry.geometryArena.primitives, identity,
                    &GeometryArenaPrimitiveRange::identity);
                if (found == geometry.geometryArena.primitives.end()) {
                    throw std::logic_error(
                        "Runtime primitive is absent from its geometry arena");
                }
                const size_t rangeIndex = static_cast<size_t>(
                    found - geometry.geometryArena.primitives.begin());
                primitive.geometry =
                    allocation.primitiveGeometry[rangeIndex];
                primitive.indexStart = found->firstIndex;
                primitive.vertexOffset = found->vertexOffset;
                primitive.indexFormat = found->indexStream ==
                        GeometryArenaIndexStream::UInt16 ? 0u : 1u;
            };
            for (SubMesh& primitive : asset->subMeshes) {
                assignRange(primitive);
            }
            for (ModelLodChain& chain : asset->lodChains) {
                for (ModelLodLevel& level : chain.levels)
                    assignRange(level.subMesh);
            }
        }
        catch (...) {
            renderBackend->freeGeometryArena(
                allocation.primitiveGeometry);
            throw;
        }
        asset->geometryArena = std::move(
            allocation.primitiveGeometry);
        asset->geometry = asset->geometryArena.front();
        asset->sourceIndexBytes = geometry.geometryArena.stats.sourceIndexBytes;
        asset->arenaIndexBytes = geometry.geometryArena.stats.arenaIndexBytes;
        asset->arenaSavedIndexBytes = geometry.geometryArena.stats.savedIndexBytes;
        asset->arenaUInt16IndexCount =
            geometry.geometryArena.stats.uint16IndexCount;
        asset->arenaUInt32IndexCount =
            geometry.geometryArena.stats.uint32IndexCount;
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
        const RuntimeModelLodResidencyStats lodResidency =
            applyRuntimeModelLodResidencyFloor(
                *runtime.data, minimumResidentLodLevel_);
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
        model->lodChains =
            std::move(resolved.data->geometry.lodChains);
        model->materials = std::move(resolved.data->materials);
        model->totalIndices =
            static_cast<uint32_t>(
                resolved.data->geometry.indices.size());
        model->lodResidentBaseLevel = lodResidency.maximumAppliedLevel;
        model->lodFallbackChainCount = lodResidency.fallbackChainCount;
        model->lodWithheldPrimitiveRangeCount =
            lodResidency.withheldPrimitiveRangeCount;
        model->lodWithheldIndexBytes = lodResidency.withheldIndexBytes();
        uploadArenaToGPU(model.get(), resolved.data->geometry);
        if (lodResidency.fallbackChainCount != 0u) {
            std::cout << "IRIDIUM_LOD_PHYSICAL_FALLBACK {\"asset_guid\":\""
                << model->assetGuid.toString() << "\",\"requested_floor\":"
                << lodResidency.requestedMinimumLevel
                << ",\"maximum_applied_floor\":"
                << lodResidency.maximumAppliedLevel
                << ",\"fallback_chains\":"
                << lodResidency.fallbackChainCount
                << ",\"withheld_ranges\":"
                << lodResidency.withheldPrimitiveRangeCount
                << ",\"withheld_index_bytes\":"
                << lodResidency.withheldIndexBytes() << "}\n";
        }
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
            makeRuntimeModelCpuData(product, false,
                runtimeTransparencyExecutionMode_);
        if (!geometry.valid()) {
            throw std::runtime_error(
                "Complete cooked model geometry conversion failed.");
        }
        const RuntimeModelLodResidencyStats lodResidency =
            applyRuntimeModelLodResidencyFloor(
                *geometry.data, minimumResidentLodLevel_);
        RuntimeCanonicalMaterialResult canonical =
            makeRuntimeCanonicalMaterials(product,
                textureViews, fallbacks, false,
                runtimeTransparencyExecutionMode_);
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
        model->lodChains =
            std::move(resolved.data->geometry.lodChains);
        model->materials =
            std::move(resolved.data->materials);
        model->totalIndices = static_cast<uint32_t>(
            resolved.data->geometry.indices.size());
        model->lodResidentBaseLevel = lodResidency.maximumAppliedLevel;
        model->lodFallbackChainCount = lodResidency.fallbackChainCount;
        model->lodWithheldPrimitiveRangeCount =
            lodResidency.withheldPrimitiveRangeCount;
        model->lodWithheldIndexBytes = lodResidency.withheldIndexBytes();
        try {
            uploadArenaToGPU(model.get(), resolved.data->geometry);
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
        if (lodResidency.fallbackChainCount != 0u) {
            std::cout << "IRIDIUM_LOD_PHYSICAL_FALLBACK {\"asset_guid\":\""
                << model->assetGuid.toString() << "\",\"requested_floor\":"
                << lodResidency.requestedMinimumLevel
                << ",\"maximum_applied_floor\":"
                << lodResidency.maximumAppliedLevel
                << ",\"fallback_chains\":"
                << lodResidency.fallbackChainCount
                << ",\"withheld_ranges\":"
                << lodResidency.withheldPrimitiveRangeCount
                << ",\"withheld_index_bytes\":"
                << lodResidency.withheldIndexBytes() << "}\n";
        }
        PreviewInputs inputs;
        inputs.product.materials = product.materials;
        inputs.product.manifest.primitives = product.manifest.primitives;
        inputs.product.manifest.transparencyExecutionMode = product.manifest.transparencyExecutionMode;
        inputs.views.assign(textureViews.begin(), textureViews.end());
        inputs.fallbacks = fallbacks;
        inputs.cookKey = artifact.cookKey;
        previewInputs_.insert_or_assign(artifact.assetGuid, std::move(inputs));
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
            !replacement->geometryArena.empty()) {
            renderBackend->freeGeometryArena(
                replacement->geometryArena);
            replacement->ownsGeometry = false;
        }
        else if (replacement->ownsGeometry &&
                replacement->geometry.isValid()) {
            renderBackend->freeGeometry(replacement->geometry);
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
        return editorBridge_ != nullptr && texture.isValid()
            ? editorBridge_->editorTextureId(texture) : nullptr;
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
        return editorBridge_ != nullptr &&
            found->second.texture.isValid()
            ? editorBridge_->editorTextureId(
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
        return editorBridge_ != nullptr
            ? editorBridge_->editorTextureId(
                editorDetailThumbnail_
                    .texture)
            : nullptr;
    }

} // namespace Iridium
