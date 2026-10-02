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
            const CompiledMaterial& material,
            TransparencyExecutionMode executionMode) {
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
            const bool classifiedSorted =
                executionMode == TransparencyExecutionMode::Classified &&
                material.transparency.resolvedClass ==
                    TransparencyClass::SortedSurface;
            const bool classifiedTransparent =
                executionMode == TransparencyExecutionMode::Classified &&
                blended;
            const bool usesGBuffer = deferred && !classifiedSorted;
            const bool transparentBlend = blended || classifiedSorted;
            PipelineStateDesc pipeline;
            pipeline.shaderProgram = usesGBuffer
                ? ShaderProgram::CanonicalPbrGBuffer
                : classifiedSorted
                    // SortedSurface is deliberately non-transmissive. Reuse the
                    // non-transmission forward program so its SPIR-V does not
                    // statically access the compatibility scene-copy/glass-depth
                    // descriptors before those resources are produced.
                    ? ShaderProgram::CanonicalComplexOpaqueForward
                : transparentBlend
                    ? ShaderProgram::CanonicalComplexForward
                    : ShaderProgram::
                        CanonicalComplexOpaqueForward;
            pipeline.renderPass = usesGBuffer
                ? RenderPassClass::GBuffer
                : classifiedSorted
                    ? RenderPassClass::Transparent
                    : RenderPassClass::Forward;
            pipeline.topology =
                PrimitiveTopology::TriangleList;
            pipeline.polygonMode = PolygonMode::Fill;
            pipeline.cullMode = material.standard.doubleSided
                ? CullMode::None : CullMode::Back;
            pipeline.frontFace = FrontFace::Clockwise;
            pipeline.blendMode = classifiedSorted || classifiedTransparent
                ? BlendMode::PremultipliedAlpha
                : blended ? BlendMode::AlphaBlend : BlendMode::Opaque;
            pipeline.depthTest = true;
            pipeline.depthCompare = usesGBuffer
                ? DepthCompare::Less
                : DepthCompare::LessOrEqual;
            pipeline.colorWriteMask = ColorWriteAll;
            pipeline.depthWrite = !transparentBlend;
            return pipeline;
        }

    } // namespace

    RuntimeModelCpuResult makeRuntimeModelCpuData(
        const CookedModelProductData& product,
        bool validateProduct,
        std::optional<TransparencyExecutionMode> executionModeOverride) {
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
        runtime.transparencyExecutionMode =
            executionModeOverride.value_or(
                product.manifest.transparencyExecutionMode);
        runtime.vertices.reserve(product.vertices.size());
        runtime.indices = product.indices;
        std::vector<GeometryArenaPrimitiveInput> arenaPrimitives;
        arenaPrimitives.reserve(product.manifest.primitives.size());
        for (const CookedModelPrimitive& primitive :
            product.manifest.primitives) {
            arenaPrimitives.push_back({
                .identity = {
                    primitive.sourcePrimitiveGuid,
                    primitive.primitiveGuid,
                },
                .firstVertex = primitive.firstVertex,
                .vertexCount = primitive.vertexCount,
                .firstIndex = primitive.firstIndex,
                .indexCount = primitive.indexCount,
            });
        }
        GeometryArenaBuildResult arena = buildGeometryArena(
            GeometryArenaAbiVersion, product.vertices.size(), product.indices,
            arenaPrimitives);
        if (!arena.valid()) {
            error(result.diagnostics, "MODEL_RUNTIME_GEOMETRY_ARENA", "/indices",
                "Cooked raster streams cannot be represented by the current "
                "versioned geometry-arena ABI (error " +
                std::to_string(static_cast<uint32_t>(arena.error)) + ").");
            return result;
        }
        runtime.geometryArena = std::move(*arena.data);
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
                .sourcePrimitiveGuid = source.sourcePrimitiveGuid,
                .primitiveGuid = source.primitiveGuid,
                .materialGuid = source.materialGuid,
                .sourceNode = source.sourceNode,
                .sourceMesh = source.sourceMesh,
                .sourcePrimitive = source.sourcePrimitive,
                .attributeMask = source.attributeMask,
                .flags = source.flags,
                .coverage = static_cast<uint8_t>(source.coverage),
                .transparency = source.transparency,
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
        if (hasCookErrors(result.diagnostics)) return result;
        if (!product.lodChains.empty()) {
            std::vector<SubMesh> allPrimitives =
                std::move(runtime.primitives);
            std::vector<bool> lodChildren(allPrimitives.size(), false);
            runtime.lodChains.reserve(product.lodChains.size());
            for (const CookedModelLodChain& cookedChain :
                    product.lodChains) {
                if (cookedChain.basePrimitiveIndex >= allPrimitives.size() ||
                    std::ranges::any_of(cookedChain.levels,
                        [&allPrimitives](const CookedModelLodLevel& level) {
                            return level.primitiveIndex >= allPrimitives.size();
                        })) {
                    error(result.diagnostics, "MODEL_RUNTIME_LOD_RANGE",
                        "/lod_chains", "LOD primitive index is out of range.");
                    return result;
                }
                const SubMesh& base =
                    allPrimitives[cookedChain.basePrimitiveIndex];
                ModelLodChain chain{
                    .sourcePrimitiveGuid = base.sourcePrimitiveGuid,
                    .basePrimitiveGuid = base.primitiveGuid,
                };
                chain.levels.reserve(cookedChain.levels.size());
                for (size_t levelIndex = 0;
                        levelIndex < cookedChain.levels.size(); ++levelIndex) {
                    const CookedModelLodLevel& cookedLevel =
                        cookedChain.levels[levelIndex];
                    if (levelIndex != 0u)
                        lodChildren[cookedLevel.primitiveIndex] = true;
                    chain.levels.push_back({
                        .geometricError = cookedLevel.geometricError,
                        .subMesh = allPrimitives[
                            cookedLevel.primitiveIndex],
                    });
                }
                runtime.lodChains.push_back(std::move(chain));
            }
            runtime.primitives.reserve(allPrimitives.size());
            for (size_t primitiveIndex = 0;
                    primitiveIndex < allPrimitives.size(); ++primitiveIndex) {
                if (!lodChildren[primitiveIndex])
                    runtime.primitives.push_back(
                        std::move(allPrimitives[primitiveIndex]));
            }
        }
        if (!hasCookErrors(result.diagnostics)) {
            result.data = std::move(runtime);
        }
        return result;
    }

    RuntimeModelLodResidencyStats applyRuntimeModelLodResidencyFloor(
        RuntimeModelCpuData& model, uint32_t minimumLodLevel) {
        if (minimumLodLevel >= MaximumGpuSceneLodLevels) {
            throw std::invalid_argument(
                "Runtime model LOD residency floor exceeds the GPU-scene ABI");
        }

        RuntimeModelLodResidencyStats stats{
            .requestedMinimumLevel = minimumLodLevel,
            .originalIndexBytes = model.geometryArena.uint16Indices.size() *
                    sizeof(uint16_t) +
                model.geometryArena.uint32Indices.size() * sizeof(uint32_t),
        };
        if (minimumLodLevel == 0u || model.lodChains.empty()) {
            stats.residentIndexBytes = stats.originalIndexBytes;
            return stats;
        }

        using Identity = GeometryArenaPrimitiveIdentity;
        const auto identityOf = [](const SubMesh& primitive) {
            return Identity{ primitive.sourcePrimitiveGuid,
                primitive.primitiveGuid };
        };
        std::map<Identity, const GeometryArenaPrimitiveRange*> sourceRanges;
        for (const GeometryArenaPrimitiveRange& range :
                model.geometryArena.primitives) {
            if (!sourceRanges.emplace(range.identity, &range).second) {
                throw std::logic_error(
                    "Runtime geometry arena contains duplicate primitive identity");
            }
        }

        std::set<Identity> chainIdentities;
        for (const ModelLodChain& chain : model.lodChains) {
            if (chain.levels.empty() ||
                chain.levels.front().subMesh.primitiveGuid !=
                    chain.basePrimitiveGuid) {
                throw std::logic_error(
                    "Runtime LOD chain lacks its canonical level zero");
            }
            for (const ModelLodLevel& level : chain.levels) {
                const Identity identity = identityOf(level.subMesh);
                if (!sourceRanges.contains(identity) ||
                    !chainIdentities.insert(identity).second) {
                    throw std::logic_error(
                        "Runtime LOD chain does not map uniquely to its arena");
                }
            }
        }

        // Map retained stable identities to the source payload whose index range
        // will actually be uploaded. The canonical identity maps to the first
        // retained coarse payload; the discarded child's identity is not kept.
        std::map<Identity, Identity> retainedPayloads;
        for (const GeometryArenaPrimitiveRange& range :
                model.geometryArena.primitives) {
            if (!chainIdentities.contains(range.identity))
                retainedPayloads.emplace(range.identity, range.identity);
        }

        std::vector<ModelLodChain> residentChains;
        residentChains.reserve(model.lodChains.size());
        std::vector<SubMesh> residentPrimitives = model.primitives;
        for (const ModelLodChain& sourceChain : model.lodChains) {
            const uint32_t applied = (std::min)(minimumLodLevel,
                static_cast<uint32_t>(sourceChain.levels.size() - 1u));
            stats.maximumAppliedLevel = (std::max)(
                stats.maximumAppliedLevel, applied);
            if (applied != 0u) ++stats.fallbackChainCount;
            stats.withheldPrimitiveRangeCount += applied;

            const Identity baseIdentity = identityOf(
                sourceChain.levels.front().subMesh);
            const Identity residentIdentity = identityOf(
                sourceChain.levels[applied].subMesh);
            retainedPayloads.emplace(baseIdentity, residentIdentity);
            for (size_t level = static_cast<size_t>(applied) + 1u;
                    level < sourceChain.levels.size(); ++level) {
                const Identity identity = identityOf(
                    sourceChain.levels[level].subMesh);
                retainedPayloads.emplace(identity, identity);
            }

            ModelLodChain residentChain{
                .sourcePrimitiveGuid = sourceChain.sourcePrimitiveGuid,
                .basePrimitiveGuid = sourceChain.basePrimitiveGuid,
                .residentBaseLevel = applied,
            };
            ModelLodLevel canonical = sourceChain.levels.front();
            canonical.subMesh.indexCount =
                sourceChain.levels[applied].subMesh.indexCount;
            canonical.subMesh.geometry = {};
            residentChain.levels.push_back(std::move(canonical));
            for (size_t level = static_cast<size_t>(applied) + 1u;
                    level < sourceChain.levels.size(); ++level) {
                residentChain.levels.push_back(sourceChain.levels[level]);
            }
            residentChains.push_back(std::move(residentChain));

            const auto primitive = std::ranges::find(
                residentPrimitives, sourceChain.basePrimitiveGuid,
                &SubMesh::primitiveGuid);
            if (primitive == residentPrimitives.end()) {
                throw std::logic_error(
                    "Runtime LOD canonical primitive is absent");
            }
            primitive->indexCount = sourceChain.levels[applied].subMesh.indexCount;
            primitive->geometry = {};
        }

        GeometryArenaData residentArena = model.geometryArena;
        residentArena.primitives.clear();
        residentArena.uint16Indices.clear();
        residentArena.uint32Indices.clear();
        residentArena.primitives.reserve(retainedPayloads.size());
        for (const auto& [retainedIdentity, payloadIdentity] :
                retainedPayloads) {
            const auto source = sourceRanges.find(payloadIdentity);
            if (source == sourceRanges.end()) {
                throw std::logic_error(
                    "Runtime LOD resident payload is absent from its arena");
            }
            GeometryArenaPrimitiveRange range = *source->second;
            range.identity = retainedIdentity;
            if (range.indexStream == GeometryArenaIndexStream::UInt16) {
                const uint64_t end = static_cast<uint64_t>(range.firstIndex) +
                    range.indexCount;
                if (end > model.geometryArena.uint16Indices.size()) {
                    throw std::logic_error(
                        "Runtime UInt16 LOD range exceeds its arena stream");
                }
                range.firstIndex = static_cast<uint32_t>(
                    residentArena.uint16Indices.size());
                residentArena.uint16Indices.insert(
                    residentArena.uint16Indices.end(),
                    model.geometryArena.uint16Indices.begin() +
                        source->second->firstIndex,
                    model.geometryArena.uint16Indices.begin() + end);
            } else {
                const uint64_t end = static_cast<uint64_t>(range.firstIndex) +
                    range.indexCount;
                if (end > model.geometryArena.uint32Indices.size()) {
                    throw std::logic_error(
                        "Runtime UInt32 LOD range exceeds its arena stream");
                }
                range.firstIndex = static_cast<uint32_t>(
                    residentArena.uint32Indices.size());
                residentArena.uint32Indices.insert(
                    residentArena.uint32Indices.end(),
                    model.geometryArena.uint32Indices.begin() +
                        source->second->firstIndex,
                    model.geometryArena.uint32Indices.begin() + end);
            }
            residentArena.primitives.push_back(range);
        }

        residentArena.stats.arenaIndexBytes =
            residentArena.uint16Indices.size() * sizeof(uint16_t) +
            residentArena.uint32Indices.size() * sizeof(uint32_t);
        residentArena.stats.savedIndexBytes =
            residentArena.stats.sourceIndexBytes >=
                    residentArena.stats.arenaIndexBytes
                ? residentArena.stats.sourceIndexBytes -
                    residentArena.stats.arenaIndexBytes
                : 0u;
        residentArena.stats.uint16IndexCount =
            residentArena.uint16Indices.size();
        residentArena.stats.uint32IndexCount =
            residentArena.uint32Indices.size();
        residentArena.stats.uint16PrimitiveCount = static_cast<uint32_t>(
            std::ranges::count(residentArena.primitives,
                GeometryArenaIndexStream::UInt16,
                &GeometryArenaPrimitiveRange::indexStream));
        residentArena.stats.uint32PrimitiveCount = static_cast<uint32_t>(
            residentArena.primitives.size() -
            residentArena.stats.uint16PrimitiveCount);

        stats.residentIndexBytes = residentArena.stats.arenaIndexBytes;
        if (stats.residentIndexBytes > stats.originalIndexBytes) {
            throw std::logic_error(
                "Runtime LOD residency floor increased physical index bytes");
        }
        model.primitives = std::move(residentPrimitives);
        model.lodChains = std::move(residentChains);
        model.geometryArena = std::move(residentArena);
        return stats;
    }

    RuntimeCanonicalMaterialResult
        makeRuntimeCanonicalMaterials(
            const CookedModelProductData& product,
            std::span<const RuntimeTextureViewBinding>
                textureViews,
            const RuntimeMaterialFallbacks& fallbacks,
            bool validateProduct,
            std::optional<TransparencyExecutionMode> executionModeOverride) {
        RuntimeCanonicalMaterialResult result;
        const TransparencyExecutionMode executionMode =
            executionModeOverride.value_or(
                product.manifest.transparencyExecutionMode);
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
        result.materials.reserve(product.manifest.primitives.size());
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
            std::vector<CompiledTransparencyPolicy> policies;
            if (executionMode == TransparencyExecutionMode::Classified) {
                for (const CookedModelPrimitive& primitive :
                        product.manifest.primitives) {
                    if (primitive.materialGuid == source.materialGuid &&
                        std::ranges::find(policies,
                            primitive.transparency) == policies.end()) {
                        policies.push_back(primitive.transparency);
                    }
                }
            }
            if (policies.empty())
                policies.push_back(source.compiled.transparency);
            std::ranges::sort(policies,
                [](const CompiledTransparencyPolicy& lhs,
                    const CompiledTransparencyPolicy& rhs) {
                    const uint32_t lhsWord = packTransparencyPolicyWord(lhs);
                    const uint32_t rhsWord = packTransparencyPolicyWord(rhs);
                    if (lhsWord != rhsWord) return lhsWord < rhsWord;
                    if (lhs.priority != rhs.priority)
                        return lhs.priority < rhs.priority;
                    return lhs.thinSheetThicknessMeters <
                        rhs.thinSheetThicknessMeters;
                });

            for (const CompiledTransparencyPolicy& policy : policies) {
                CompiledMaterial runtimeCompiled = source.compiled;
                runtimeCompiled.transparency = policy;
                const auto compiled =
                    std::make_shared<const CompiledMaterial>(
                        std::move(runtimeCompiled));
                const MaterialInstance instance(compiled, bindings);
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
                CanonicalMaterialAsset variant = canonical;
                variant.packed = *packed.material;
                if (executionMode == TransparencyExecutionMode::Classified &&
                    policy.resolvedClass >=
                        TransparencyClass::SortedSurface &&
                    policy.resolvedClass <=
                        TransparencyClass::WeightedOit) {
                    variant.packed.featureFlags |=
                        MaterialFeatureClassifiedTransparencyExecution;
                }
                variant.pipelineState = pipelineFor(*compiled,
                    executionMode);
                result.materials.push_back({
                    .materialGuid = source.materialGuid,
                    .transparency = policy,
                    .asset = std::move(variant),
                });
            }
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
        const bool classified = geometry.transparencyExecutionMode ==
            TransparencyExecutionMode::Classified;
        for (size_t index = 0; index < bindings.size(); ++index) {
            const RuntimeMaterialBinding& binding = bindings[index];
            const bool duplicate = std::ranges::any_of(
                bindings.first(index), [&](const RuntimeMaterialBinding& prior) {
                    return prior.materialGuid == binding.materialGuid &&
                        (!classified || prior.transparency ==
                            binding.transparency);
                });
            if (binding.materialGuid.isNil() || duplicate) {
                error(result.diagnostics,
                    "MODEL_RUNTIME_MATERIAL_BINDING", "/materials",
                    classified
                        ? "Classified runtime material bindings require unique non-nil GUID/policy identities."
                        : "Runtime material bindings require unique non-nil GUIDs.");
            }
        }
        if (hasCookErrors(result.diagnostics)) return result;

        ResolvedRuntimeModelCpuData resolved{
            .geometry = std::move(geometry),
        };
        struct RuntimeIndex {
            AssetGuid materialGuid;
            CompiledTransparencyPolicy transparency;
            int index = -1;
        };
        std::vector<RuntimeIndex> runtimeIndices;
        const auto resolvePrimitive = [&](SubMesh& primitive,
                std::string field) {
            const auto matchesPolicy = [&](const auto& candidate) {
                return candidate.materialGuid == primitive.materialGuid &&
                    (!classified || candidate.transparency ==
                        primitive.transparency);
            };
            auto runtime = std::ranges::find_if(runtimeIndices, matchesPolicy);
            if (runtime == runtimeIndices.end()) {
                const auto material = std::ranges::find_if(
                    bindings, matchesPolicy);
                if (material == bindings.end()) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_MATERIAL_MISSING",
                        std::move(field) + "/material_guid",
                        classified
                            ? "Cooked model references an unresolved material GUID/policy variant."
                            : "Cooked model references an unresolved material GUID.");
                    return;
                }
                if (resolved.materials.size() >
                    static_cast<size_t>(std::numeric_limits<int>::max())) {
                    error(result.diagnostics,
                        "MODEL_RUNTIME_MATERIAL_LIMIT", "/materials",
                        "Runtime material binding count exceeds index limits.");
                    return;
                }
                const int materialIndex =
                    static_cast<int>(resolved.materials.size());
                resolved.materials.push_back(material->binding);
                runtimeIndices.push_back({
                    .materialGuid = primitive.materialGuid,
                    .transparency = primitive.transparency,
                    .index = materialIndex,
                });
                runtime = std::prev(runtimeIndices.end());
            }
            primitive.materialIndex = runtime->index;
        };
        for (size_t index = 0;
            index < resolved.geometry.primitives.size(); ++index) {
            resolvePrimitive(resolved.geometry.primitives[index],
                "/primitives/" + std::to_string(index));
        }
        for (size_t chainIndex = 0;
                chainIndex < resolved.geometry.lodChains.size(); ++chainIndex) {
            ModelLodChain& chain = resolved.geometry.lodChains[chainIndex];
            for (size_t levelIndex = 0;
                    levelIndex < chain.levels.size(); ++levelIndex) {
                resolvePrimitive(chain.levels[levelIndex].subMesh,
                    "/lod_chains/" + std::to_string(chainIndex) +
                        "/levels/" + std::to_string(levelIndex));
            }
        }
        if (!hasCookErrors(result.diagnostics)) {
            result.data = std::move(resolved);
        }
        return result;
    }

} // namespace Iridium
