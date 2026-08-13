#include "assets/model/ModelRuntimeProduct.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace Iridium {

    namespace {

        void error(std::vector<CookDiagnostic>& diagnostics,
            std::string code, std::string field, std::string message) {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Error,
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

        bool valid(MaterialTextureBinding binding) {
            return binding.texture.isValid() &&
                binding.sampler.isValid();
        }

        PipelineStateDesc pipelineFor(
            const CompiledMaterial& material) {
            const bool deferred = material.closureClass ==
                MaterialClosureClass::StandardDeferred;
            const bool transmitted = std::ranges::any_of(
                material.complexLobes,
                [](const ComplexLobeRecord& lobe) {
                    return lobe.type ==
                            ComplexLobeType::ThinTransmission ||
                        lobe.type ==
                            ComplexLobeType::VolumeTransmission ||
                        lobe.type ==
                            ComplexLobeType::DiffuseTransmission;
                });
            const bool blended =
                material.standard.alphaMode ==
                    SourceAlphaMode::Blend ||
                transmitted;
            PipelineStateDesc pipeline;
            pipeline.shaderProgram = deferred
                ? ShaderProgram::CanonicalPbrGBuffer
                : blended
                    ? ShaderProgram::CanonicalComplexForward
                    : ShaderProgram::
                        CanonicalComplexOpaqueForward;
            pipeline.renderPass = deferred
                ? RenderPassClass::GBuffer
                : RenderPassClass::Forward;
            pipeline.topology =
                PrimitiveTopology::TriangleList;
            pipeline.polygonMode = PolygonMode::Fill;
            pipeline.cullMode = material.standard.doubleSided
                ? CullMode::None : CullMode::Back;
            pipeline.frontFace = FrontFace::Clockwise;
            pipeline.blendMode = blended
                ? BlendMode::AlphaBlend : BlendMode::Opaque;
            pipeline.depthTest = true;
            pipeline.depthCompare = deferred
                ? DepthCompare::Less
                : DepthCompare::LessOrEqual;
            pipeline.colorWriteMask = ColorWriteAll;
            pipeline.depthWrite = !blended;
            return pipeline;
        }

    } // namespace

    RuntimeModelCpuResult makeRuntimeModelCpuData(
        const CookedModelProductData& product,
        bool validateProduct) {
        RuntimeModelCpuResult result;
        if (validateProduct) {
            result.diagnostics =
                validateModelProduct(product);
        }
        if (hasCookErrors(result.diagnostics)) return result;
        if (product.vertices.size() > std::numeric_limits<uint32_t>::max() ||
            product.indices.size() > std::numeric_limits<uint32_t>::max()) {
            error(result.diagnostics, "MODEL_RUNTIME_32BIT_LIMIT", "/",
                "Current RHI draw packets require 32-bit model stream ranges.");
            return result;
        }

        RuntimeModelCpuData runtime;
        runtime.vertices.reserve(product.vertices.size());
        runtime.indices = product.indices;
        runtime.primitives.reserve(product.manifest.primitives.size());
        for (const CookedModelVertex& source : product.vertices) {
            runtime.vertices.push_back({
                .pos = {
                    source.position[0], source.position[1], source.position[2],
                },
                .color = {
                    source.color[0], source.color[1],
                    source.color[2], source.color[3],
                },
                .normal = {
                    source.normal[0], source.normal[1], source.normal[2],
                },
                .uv0 = { source.texCoord0[0], source.texCoord0[1] },
                .tangent = {
                    source.tangent[0], source.tangent[1],
                    source.tangent[2], source.tangent[3],
                },
                .uv1 = { source.texCoord1[0], source.texCoord1[1] },
            });
        }
        for (size_t index = 0;
            index < product.manifest.primitives.size(); ++index) {
            const CookedModelPrimitive& source =
                product.manifest.primitives[index];
            if (source.firstIndex > std::numeric_limits<uint32_t>::max() ||
                source.indexCount > std::numeric_limits<uint32_t>::max()) {
                error(result.diagnostics, "MODEL_RUNTIME_DRAW_RANGE",
                    "/primitives/" + std::to_string(index),
                    "Primitive draw range exceeds the current 32-bit RHI contract.");
                continue;
            }
            runtime.primitives.push_back({
                .indexStart = static_cast<uint32_t>(source.firstIndex),
                .indexCount = static_cast<uint32_t>(source.indexCount),
                .materialIndex = -1,
                .primitiveGuid = source.primitiveGuid,
                .materialGuid = source.materialGuid,
                .sourceNode = source.sourceNode,
                .sourceMesh = source.sourceMesh,
                .sourcePrimitive = source.sourcePrimitive,
                .attributeMask = source.attributeMask,
                .flags = source.flags,
                .coverage = static_cast<uint8_t>(source.coverage),
                .boundsMin = {
                    source.bounds.aabbMin[0], source.bounds.aabbMin[1],
                    source.bounds.aabbMin[2],
                },
                .boundsMax = {
                    source.bounds.aabbMax[0], source.bounds.aabbMax[1],
                    source.bounds.aabbMax[2],
                },
                .boundsSphereCenter = {
                    source.bounds.sphereCenter[0],
                    source.bounds.sphereCenter[1],
                    source.bounds.sphereCenter[2],
                },
                .boundsSphereRadius = source.bounds.sphereRadius,
            });
        }
        if (!hasCookErrors(result.diagnostics)) {
            result.data = std::move(runtime);
        }
        return result;
    }

    RuntimeCanonicalMaterialResult
        makeRuntimeCanonicalMaterials(
            const CookedModelProductData& product,
            std::span<const RuntimeTextureViewBinding>
                textureViews,
            const RuntimeMaterialFallbacks& fallbacks,
            bool validateProduct) {
        RuntimeCanonicalMaterialResult result;
        if (validateProduct) {
            std::vector<CookDiagnostic>
                productDiagnostics =
                    validateModelProduct(product);
            result.diagnostics.insert(
                result.diagnostics.end(),
                productDiagnostics.begin(),
                productDiagnostics.end());
        }
        if (hasCookErrors(result.diagnostics)) return result;
        if (!valid(fallbacks.white) ||
            !valid(fallbacks.normal) ||
            !valid(fallbacks.linearData)) {
            error(result.diagnostics,
                "MODEL_RUNTIME_FALLBACKS", "/materials",
                "Runtime material fallback texture views must be live.");
            return result;
        }

        using ViewKey = std::pair<AssetGuid, uint32_t>;
        std::map<ViewKey, const RuntimeTextureViewBinding*>
            viewsByOperation;
        for (size_t index = 0;
            index < textureViews.size(); ++index) {
            const RuntimeTextureViewBinding& view =
                textureViews[index];
            if (view.materialGuid.isNil() ||
                view.textureGuid.isNil() ||
                !valid(view.binding) ||
                !viewsByOperation.emplace(
                    ViewKey{ view.materialGuid,
                        view.operationIndex },
                    &view).second) {
                error(result.diagnostics,
                    "MODEL_RUNTIME_TEXTURE_VIEW",
                    "/texture_views/" +
                        std::to_string(index),
                    "Runtime texture views require unique material/operation identities and live handles.");
            }
        }
        if (hasCookErrors(result.diagnostics)) return result;

        std::set<ViewKey> consumedViews;
        result.materials.reserve(product.materials.size());
        for (size_t materialIndex = 0;
            materialIndex < product.materials.size();
            ++materialIndex) {
            const CookedModelMaterial& source =
                product.materials[materialIndex];
            CanonicalMaterialAsset canonical;
            canonical.name = source.compiled.sourceName.empty()
                ? source.sourceKey
                : source.compiled.sourceName;
            canonical.textures.fill(
                fallbacks.white.texture);
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Normal)] =
                fallbacks.normal.texture;
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::ClearcoatNormal)] =
                fallbacks.normal.texture;
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::MetallicRoughness)] =
                fallbacks.linearData.texture;
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Occlusion)] =
                fallbacks.linearData.texture;
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Transmission)] =
                fallbacks.linearData.texture;
            canonical.textures[static_cast<uint32_t>(
                SourceTextureSemantic::Thickness)] =
                fallbacks.linearData.texture;

            std::map<uint32_t,
                const CookedModelTextureBinding*>
                cookedBindings;
            for (const CookedModelTextureBinding& binding :
                source.textureBindings) {
                cookedBindings.emplace(
                    binding.operationIndex, &binding);
            }
            std::vector<MaterialTextureBinding> bindings;
            bindings.reserve(
                source.compiled.textureOperations.size());
            uint32_t maximumTextureIndex = 0;
            uint32_t maximumSamplerIndex = 0;
            for (size_t operationIndex = 0;
                operationIndex <
                    source.compiled.textureOperations.size();
                ++operationIndex) {
                const auto cooked =
                    cookedBindings.find(static_cast<uint32_t>(
                        operationIndex));
                const ViewKey key{
                    source.materialGuid,
                    static_cast<uint32_t>(operationIndex),
                };
                const auto runtime =
                    viewsByOperation.find(key);
                if (cooked == cookedBindings.end() ||
                    runtime == viewsByOperation.end() ||
                    runtime->second->textureGuid !=
                        cooked->second->textureGuid) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_TEXTURE_RESOLUTION",
                        "/materials/" +
                            std::to_string(materialIndex) +
                            "/texture_operations/" +
                            std::to_string(operationIndex),
                        "Compiled texture operation did not resolve through its stable texture GUID.");
                    continue;
                }
                consumedViews.insert(key);
                const CompiledTextureOperation& operation =
                    source.compiled.textureOperations[
                        operationIndex];
                if (operation.transform
                        .texCoordOverride.value_or(
                            operation.texCoord) > 1u) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_TEXCOORD",
                        "/materials/" +
                            std::to_string(materialIndex) +
                            "/texture_operations/" +
                            std::to_string(operationIndex),
                        "Current canonical vertex contract supports TEXCOORD_0 and TEXCOORD_1.");
                    continue;
                }
                const MaterialTextureBinding binding =
                    runtime->second->binding;
                canonical.textures[
                    static_cast<uint32_t>(
                        operation.semantic)] =
                    binding.texture;
                maximumTextureIndex = std::max(
                    maximumTextureIndex,
                    binding.texture.getIndex());
                maximumSamplerIndex = std::max(
                    maximumSamplerIndex,
                    binding.sampler.getIndex());
                bindings.push_back(binding);
            }
            if (hasCookErrors(result.diagnostics)) continue;

            std::vector<uint32_t> textureGenerations(
                static_cast<size_t>(maximumTextureIndex) +
                    1u, 0u);
            std::vector<uint32_t> samplerGenerations(
                static_cast<size_t>(maximumSamplerIndex) +
                    1u, 0u);
            for (const MaterialTextureBinding& binding :
                bindings) {
                textureGenerations[
                    binding.texture.getIndex()] =
                    binding.texture.getGeneration();
                samplerGenerations[
                    binding.sampler.getIndex()] =
                    binding.sampler.getGeneration();
            }
            const auto compiled =
                std::make_shared<const CompiledMaterial>(
                    source.compiled);
            const MaterialInstance instance(
                compiled, bindings);
            const MaterialPackResult packed =
                packMaterialInstance(instance, {
                    textureGenerations,
                    samplerGenerations,
                });
            if (!packed.succeeded()) {
                for (const MaterialPackDiagnostic& diagnostic :
                    packed.diagnostics) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_" + diagnostic.code,
                        "/materials/" +
                            std::to_string(materialIndex),
                        diagnostic.message);
                }
                continue;
            }
            canonical.packed = *packed.material;
            canonical.pipelineState =
                pipelineFor(source.compiled);
            result.materials.push_back({
                .materialGuid = source.materialGuid,
                .asset = std::move(canonical),
            });
        }

        if (consumedViews.size() != textureViews.size()) {
            error(result.diagnostics,
                "MODEL_RUNTIME_TEXTURE_VIEW_UNUSED",
                "/texture_views",
                "Runtime texture view set contains an unknown material operation.");
        }
        if (hasCookErrors(result.diagnostics)) {
            result.materials.clear();
        }
        return result;
    }

    ResolvedRuntimeModelCpuResult resolveRuntimeModelMaterials(
        RuntimeModelCpuData geometry,
        std::span<const RuntimeMaterialBinding> bindings) {
        ResolvedRuntimeModelCpuResult result;
        std::map<AssetGuid, MaterialBinding> available;
        for (const RuntimeMaterialBinding& binding : bindings) {
            if (binding.materialGuid.isNil() ||
                !available.emplace(
                    binding.materialGuid, binding.binding).second) {
                error(result.diagnostics,
                    "MODEL_RUNTIME_MATERIAL_BINDING", "/materials",
                    "Runtime material bindings require unique non-nil GUIDs.");
            }
        }
        if (hasCookErrors(result.diagnostics)) return result;

        ResolvedRuntimeModelCpuData resolved{
            .geometry = std::move(geometry),
        };
        std::map<AssetGuid, int> runtimeIndices;
        for (size_t index = 0;
            index < resolved.geometry.primitives.size(); ++index) {
            SubMesh& primitive = resolved.geometry.primitives[index];
            auto runtime = runtimeIndices.find(primitive.materialGuid);
            if (runtime == runtimeIndices.end()) {
                const auto material = available.find(primitive.materialGuid);
                if (material == available.end()) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_MATERIAL_MISSING",
                        "/primitives/" + std::to_string(index) +
                            "/material_guid",
                        "Cooked model references an unresolved material GUID.");
                    continue;
                }
                if (resolved.materials.size() >
                    static_cast<size_t>(std::numeric_limits<int>::max())) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_MATERIAL_LIMIT", "/materials",
                        "Runtime material binding count exceeds index limits.");
                    break;
                }
                const int materialIndex =
                    static_cast<int>(resolved.materials.size());
                resolved.materials.push_back(material->second);
                runtime = runtimeIndices.emplace(
                    primitive.materialGuid, materialIndex).first;
            }
            primitive.materialIndex = runtime->second;
        }
        if (!hasCookErrors(result.diagnostics)) {
            result.data = std::move(resolved);
        }
        return result;
    }

} // namespace Iridium
