#include "assets/model/ModelProduct.h"
#include "assets/model/ModelLodGenerator.h"
#include "assets/model/ModelRuntimeProduct.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition)                                                        \
        do {                                                                        \
            if (!(condition)) {                                                      \
                std::cerr << __FILE__ << ':' << __LINE__                             \
                          << ": CHECK failed: " #condition << '\n';                  \
                return false;                                                        \
            }                                                                        \
        } while (false)

    AssetGuid guid(const char* value) {
        const auto parsed = AssetGuid::parse(value);
        if (!parsed) std::abort();
        return *parsed;
    }

    CookedModelProductData fixture() {
        CookedModelProductData data;
        data.vertices = {
            {
                .position = { 0.0f, 0.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 0.0f, 0.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, 1.0f },
            },
            {
                .position = { 1.0f, 0.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 1.0f, 0.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, 1.0f },
            },
            {
                .position = { 0.0f, 1.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 0.0f, 1.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, 1.0f },
            },
            {
                .position = { 10.0f, 0.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 0.0f, 0.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, -1.0f },
            },
            {
                .position = { 11.0f, 0.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 1.0f, 0.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, -1.0f },
            },
            {
                .position = { 10.0f, 1.0f, 0.0f },
                .normal = { 0.0f, 0.0f, 1.0f },
                .texCoord0 = { 0.0f, 1.0f },
                .tangent = { 1.0f, 0.0f, 0.0f, -1.0f },
            },
        };
        data.indices = { 0, 1, 2, 3, 4, 5 };
        for (const CookedModelVertex& vertex : data.vertices) {
            data.rtPositions.push_back(vertex.position);
        }
        data.rtIndices = { 0, 1, 2, 0, 1, 2 };

        const AssetGuid opaqueMaterial =
            guid("0198fe3d-8840-7c23-9801-001122334455");
        const AssetGuid transparentMaterial =
            guid("0198fe3d-8840-7c23-9801-001122334456");
        SourceMaterial opaqueSource;
        opaqueSource.name = "opaque";
        SourceMaterial transparentSource;
        transparentSource.localIndex = 1;
        transparentSource.name = "transparent";
        transparentSource.alphaMode = {
            SourceAlphaMode::Blend,
            SourceValueOrigin::Authored,
        };
        transparentSource.doubleSided = {
            true,
            SourceValueOrigin::Authored,
        };
        const auto opaqueCompiled =
            compileSourceMaterial(opaqueSource);
        const auto transparentCompiled =
            compileSourceMaterial(transparentSource);
        if (!opaqueCompiled.succeeded() ||
            !transparentCompiled.succeeded()) {
            std::abort();
        }
        data.materials = {
            {
                .materialGuid = opaqueMaterial,
                .sourceKey = "materials/0",
                .compiled = *opaqueCompiled.material,
            },
            {
                .materialGuid = transparentMaterial,
                .sourceKey = "materials/1",
                .compiled = *transparentCompiled.material,
            },
        };
        data.manifest = {
            .vertexCount = data.vertices.size(),
            .indexCount = data.indices.size(),
            .rtPositionCount = data.rtPositions.size(),
            .rtIndexCount = data.rtIndices.size(),
            .primitives = {
                {
                    .sourcePrimitiveGuid =
                        guid("0198fe3d-8840-7c23-9801-001122334466"),
                    .primitiveGuid =
                        guid("0198fe3d-8840-7c23-9801-001122334466"),
                    .materialGuid = opaqueMaterial,
                    .sourceKey = "nodes/0/meshes/0/primitives/0",
                    .sourceNode = 0,
                    .sourceMesh = 0,
                    .sourcePrimitive = 0,
                    .attributeMask = ModelAttributePosition |
                        ModelAttributeNormal | ModelAttributeTexCoord0 |
                        ModelAttributeTangent,
                    .firstVertex = 0,
                    .vertexCount = 3,
                    .firstIndex = 0,
                    .indexCount = 3,
                    .rtFirstPosition = 0,
                    .rtPositionCount = 3,
                    .rtFirstIndex = 0,
                    .rtIndexCount = 3,
                    .indexFormat = ModelIndexFormat::UInt16,
                    .bounds = {
                        .aabbMin = { 0.0f, 0.0f, 0.0f },
                        .aabbMax = { 1.0f, 1.0f, 0.0f },
                        .sphereCenter = { 0.5f, 0.5f, 0.0f },
                        .sphereRadius = 0.707107f,
                    },
                },
                {
                    .sourcePrimitiveGuid =
                        guid("0198fe3d-8840-7c23-9801-001122334477"),
                    .primitiveGuid =
                        guid("0198fe3d-8840-7c23-9801-001122334477"),
                    .materialGuid = transparentMaterial,
                    .sourceKey = "nodes/1/meshes/0/primitives/0",
                    .sourceNode = 1,
                    .sourceMesh = 0,
                    .sourcePrimitive = 0,
                    .attributeMask = ModelAttributePosition |
                        ModelAttributeNormal | ModelAttributeTexCoord0 |
                        ModelAttributeTangent,
                    .firstVertex = 3,
                    .vertexCount = 3,
                    .firstIndex = 3,
                    .indexCount = 3,
                    .rtFirstPosition = 3,
                    .rtPositionCount = 3,
                    .rtFirstIndex = 3,
                    .rtIndexCount = 3,
                    .coverage = ModelCoverage::Transparent,
                    .indexFormat = ModelIndexFormat::UInt16,
                    .flags = ModelPrimitiveDoubleSided |
                        ModelPrimitiveMirroredTransform,
                    .rtFlags = ModelRtBuildInput | ModelRtAllowAnyHit,
                    .bounds = {
                        .aabbMin = { 10.0f, 0.0f, 0.0f },
                        .aabbMax = { 11.0f, 1.0f, 0.0f },
                        .sphereCenter = { 10.5f, 0.5f, 0.0f },
                        .sphereRadius = 0.707107f,
                    },
                },
            },
        };
        return data;
    }

    CookedModelProductData lodFixture() {
        CookedModelProductData data = fixture();
        const CookedModelVertex extra{
            .position = { 1.0f, 1.0f, 0.0f },
            .normal = { 0.0f, 0.0f, 1.0f },
            .texCoord0 = { 1.0f, 1.0f },
            .tangent = { 1.0f, 0.0f, 0.0f, 1.0f },
        };
        data.vertices.insert(data.vertices.begin() + 3, extra);
        data.indices = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 0, 1, 2 };
        data.rtPositions.clear();
        for (size_t index = 0; index < 7; ++index)
            data.rtPositions.push_back(data.vertices[index].position);
        data.rtIndices = { 0, 1, 2, 2, 1, 3, 0, 1, 2 };

        CookedModelPrimitive& base = data.manifest.primitives[0];
        base.vertexCount = 4;
        base.indexCount = 6;
        base.rtPositionCount = 4;
        base.rtIndexCount = 6;
        base.lodSection = kCookedModelLodSection;
        CookedModelPrimitive& transparent = data.manifest.primitives[1];
        transparent.firstVertex = 4;
        transparent.firstIndex = 6;
        transparent.rtFirstPosition = 4;
        transparent.rtFirstIndex = 6;

        CookedModelPrimitive coarse = base;
        coarse.primitiveGuid =
            guid("0198fe3d-8840-7c23-9801-001122334488");
        coarse.sourceKey += "/lod/1";
        coarse.firstIndex = 9;
        coarse.indexCount = 3;
        coarse.rtFirstPosition = 0;
        coarse.rtPositionCount = 0;
        coarse.rtFirstIndex = 0;
        coarse.rtIndexCount = 0;
        coarse.rtFlags = 0;
        data.manifest.primitives.push_back(coarse);
        data.manifest.vertexCount = data.vertices.size();
        data.manifest.indexCount = data.indices.size();
        data.manifest.rtPositionCount = data.rtPositions.size();
        data.manifest.rtIndexCount = data.rtIndices.size();
        data.lodChains = {{
            .basePrimitiveIndex = 0,
            .levels = {
                { .primitiveIndex = 0, .geometricError = 0.0f },
                { .primitiveIndex = 2, .geometricError = 0.25f },
            },
        }};
        return data;
    }

    const CookSection* findSection(const CookProduct& product, uint32_t id) {
        const auto found = std::ranges::find_if(product.sections,
            [id](const CookSection& section) { return section.id == id; });
        return found == product.sections.end() ? nullptr : &*found;
    }

    bool testDeterministicRoundTripAndPrimitivePreservation() {
        const CookedModelProductData source = fixture();
        CHECK(validateModelProduct(source).empty());

        const CookProduct first = makeCookedModelProduct(source);
        const CookProduct second = makeCookedModelProduct(source);
        CHECK(!hasCookErrors(first.diagnostics));
        CHECK(first.sections == second.sections);
        CHECK(first.sections.size() == 7);

        const CookSection* manifestSection =
            findSection(first, kCookedModelManifestSection);
        const CookSection* materialSection =
            findSection(first, kCookedModelMaterialSection);
        const CookSection* textureViewSection =
            findSection(first, kCookedModelTextureViewSection);
        const CookSection* vertexSection =
            findSection(first, kCookedModelVertexSection);
        const CookSection* indexSection =
            findSection(first, kCookedModelIndexSection);
        const CookSection* rtPositionSection =
            findSection(first, kCookedModelRtPositionSection);
        const CookSection* rtIndexSection =
            findSection(first, kCookedModelRtIndexSection);
        CHECK(manifestSection && materialSection &&
            textureViewSection &&
            vertexSection && indexSection &&
            rtPositionSection && rtIndexSection);

        std::vector<CookDiagnostic> diagnostics;
        const auto manifest =
            readModelManifest(manifestSection->bytes, diagnostics);
        const auto materials =
            readModelMaterials(materialSection->bytes, diagnostics);
        const auto textureViews =
            readModelTextureViews(
                textureViewSection->bytes, diagnostics);
        const auto vertices =
            readModelVertices(vertexSection->bytes, diagnostics);
        const auto rtPositions =
            readModelRtPositions(rtPositionSection->bytes, diagnostics);
        const auto rtIndices = readModelIndices(
            rtIndexSection->bytes, diagnostics, "/rt_indices");
        CHECK(diagnostics.empty());
        CHECK(manifest && materials && textureViews &&
            vertices &&
            rtPositions && rtIndices);
        CHECK(*manifest == source.manifest);
        CHECK(*materials == source.materials);
        CHECK(*textureViews == source.textureViews);
        CHECK(*vertices == source.vertices);
        CHECK(indexSection->schemaVersion ==
            kCookedModelIndexSectionSchemaVersion);
        CHECK(indexSection->bytes.size() <
            serializeModelIndices(source.indices).size() + 128u);
        CHECK(*rtPositions == source.rtPositions);
        CHECK(*rtIndices == source.rtIndices);

        CookedArtifact artifact{
            .artifactType = first.artifactType,
            .artifactSchemaVersion = first.artifactSchemaVersion,
            .sections = first.sections,
        };
        const CookedModelReadResult decoded = readCookedModelProduct(artifact);
        CHECK(decoded.valid());
        CHECK(*decoded.data == source);

        CookedArtifact legacy = artifact;
        legacy.artifactSchemaVersion = kLegacyCookedModelSchemaVersion;
        CookedModelManifest legacyManifest = source.manifest;
        legacyManifest.schemaVersion = kLegacyCookedModelSchemaVersion;
        auto legacyManifestSection = std::ranges::find_if(legacy.sections,
            [](const CookSection& section) {
                return section.id == kCookedModelManifestSection;
            });
        auto legacyIndexSection = std::ranges::find_if(legacy.sections,
            [](const CookSection& section) {
                return section.id == kCookedModelIndexSection;
            });
        legacyManifestSection->schemaVersion = kLegacyCookedModelSchemaVersion;
        legacyManifestSection->bytes = serializeModelManifest(legacyManifest);
        legacyIndexSection->schemaVersion = 1;
        legacyIndexSection->bytes = serializeModelIndices(source.indices);
        const CookedModelReadResult migrated = readCookedModelProduct(legacy);
        CHECK(migrated.valid());
        CHECK(*migrated.data == source);

        CookedArtifact previous = artifact;
        previous.artifactSchemaVersion = kPreviousCookedModelSchemaVersion;
        CookedModelManifest previousManifest = source.manifest;
        previousManifest.schemaVersion = kPreviousCookedModelSchemaVersion;
        auto previousSection = std::ranges::find_if(previous.sections,
            [](const CookSection& section) {
                return section.id == kCookedModelManifestSection;
            });
        previousSection->schemaVersion = kPreviousCookedModelSchemaVersion;
        previousSection->bytes = serializeModelManifest(previousManifest);
        const auto migratedPrevious = readCookedModelProduct(previous);
        CHECK(migratedPrevious.valid());
        CHECK(*migratedPrevious.data == source);
        previousSection->bytes = serializeModelManifest(source.manifest);
        CHECK(!readCookedModelProduct(previous).valid());

        CookedArtifact incompatible = artifact;
        auto incompatibleIndex = std::ranges::find_if(incompatible.sections,
            [](const CookSection& section) {
                return section.id == kCookedModelIndexSection;
            });
        incompatibleIndex->bytes[8] = std::byte{ 99 };
        CHECK(!readCookedModelProduct(incompatible).valid());

        CHECK(manifest->primitives.size() == 2);
        CHECK(manifest->primitives[0].materialGuid !=
            manifest->primitives[1].materialGuid);
        CHECK(manifest->primitives[0].primitiveGuid !=
            manifest->primitives[1].primitiveGuid);
        CHECK(manifest->primitives[0].sourceKey !=
            manifest->primitives[1].sourceKey);
        CHECK(manifest->primitives[1].coverage ==
            ModelCoverage::Transparent);
        CHECK((manifest->primitives[1].flags &
            ModelPrimitiveMirroredTransform) != 0);
        return true;
    }

    bool testRtGeometryReconstructsCanonicalTriangles() {
        const CookedModelProductData data = fixture();
        for (const CookedModelPrimitive& primitive :
            data.manifest.primitives) {
            CHECK(primitive.indexCount == primitive.rtIndexCount);
            CHECK(primitive.vertexCount == primitive.rtPositionCount);
            for (uint64_t item = 0; item < primitive.indexCount; ++item) {
                const uint32_t rasterIndex = data.indices[
                    static_cast<size_t>(primitive.firstIndex + item)];
                const uint32_t rtIndex = data.rtIndices[
                    static_cast<size_t>(primitive.rtFirstIndex + item)];
                CHECK(rasterIndex - primitive.firstVertex == rtIndex);
                CHECK(data.vertices[static_cast<size_t>(rasterIndex)].position ==
                    data.rtPositions[
                    static_cast<size_t>(primitive.rtFirstPosition + rtIndex)]);
            }
        }
        return true;
    }

    bool testValidationRejectsSemanticLossAndBadRanges() {
        CookedModelProductData data = fixture();
        data.manifest.primitives[1].primitiveGuid =
            data.manifest.primitives[0].primitiveGuid;
        data.manifest.primitives[1].sourceKey =
            data.manifest.primitives[0].sourceKey;
        data.manifest.primitives[0].bounds.sphereRadius = 0.1f;
        data.indices[0] = 3;
        data.manifest.primitives[1].rtFlags =
            ModelRtBuildInput | ModelRtOpaque;
        const auto diagnostics = validateModelProduct(data);
        CHECK(hasCookErrors(diagnostics));
        const auto contains = [&diagnostics](const char* code) {
            return std::ranges::any_of(diagnostics,
                [code](const CookDiagnostic& diagnostic) {
                    return diagnostic.code == code;
                });
        };
        CHECK(contains("MODEL_PRIMITIVE_GUID"));
        CHECK(contains("MODEL_SOURCE_KEY"));
        CHECK(contains("MODEL_PRIMITIVE_BOUNDS"));
        CHECK(contains("MODEL_INDEX_VALUE"));
        CHECK(contains("MODEL_RT_COVERAGE"));
        CHECK(makeCookedModelProduct(data).sections.empty());
        return true;
    }

    bool testManifestRejectsCorruption() {
        const auto bytes = serializeModelManifest(fixture().manifest);
        std::vector<CookDiagnostic> diagnostics;
        CHECK(!readModelManifest(
            std::span<const std::byte>(bytes).first(40), diagnostics));
        CHECK(hasCookErrors(diagnostics));

        auto badSchema = bytes;
        badSchema[8] = std::byte{ 9 };
        diagnostics.clear();
        CHECK(!readModelManifest(badSchema, diagnostics));
        CHECK(hasCookErrors(diagnostics));

        auto badRecord = bytes;
        badRecord[72 + 112] = std::byte{ 99 };
        diagnostics.clear();
        CHECK(!readModelManifest(badRecord, diagnostics));
        CHECK(hasCookErrors(diagnostics));
        return true;
    }

    bool testClassifiedSortedSurfaceRuntimePipeline() {
        CookedModelProductData data = fixture();
        data.manifest.transparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        data.manifest.primitives[1].transparency =
            data.materials[1].compiled.transparency;

        const RuntimeMaterialFallbacks fallbacks{
            .white = {
                TextureHandle::fromParts(1, 1),
                SamplerHandle::fromParts(1, 1),
            },
            .normal = {
                TextureHandle::fromParts(2, 1),
                SamplerHandle::fromParts(2, 1),
            },
            .linearData = {
                TextureHandle::fromParts(3, 1),
                SamplerHandle::fromParts(3, 1),
            },
        };
        const RuntimeCanonicalMaterialResult materials =
            makeRuntimeCanonicalMaterials(data, {}, fallbacks);
        CHECK(materials.valid());
        CHECK(materials.materials.size() == 2);
        const CanonicalMaterialAsset& sorted = materials.materials[1].asset;
        CHECK(sorted.pipelineState.shaderProgram ==
            ShaderProgram::CanonicalComplexOpaqueForward);
        CHECK(sorted.pipelineState.renderPass ==
            RenderPassClass::Transparent);
        CHECK(sorted.pipelineState.blendMode ==
            BlendMode::PremultipliedAlpha);
        CHECK(sorted.pipelineState.depthTest);
        CHECK(!sorted.pipelineState.depthWrite);
        CHECK((sorted.packed.featureFlags &
            MaterialFeatureClassifiedTransparencyExecution) != 0);

        const RuntimeModelCpuResult runtime =
            makeRuntimeModelCpuData(data);
        CHECK(runtime.valid());
        CHECK(runtime.data->transparencyExecutionMode ==
            TransparencyExecutionMode::Classified);
        CHECK(runtime.data->primitives[1].sourcePrimitiveGuid ==
            data.manifest.primitives[1].sourcePrimitiveGuid);
        CHECK(runtime.data->primitives[1].transparency.resolvedClass ==
            TransparencyClass::SortedSurface);

        const RuntimeCanonicalMaterialResult forcedLegacyMaterials =
            makeRuntimeCanonicalMaterials(data, {}, fallbacks, true,
                TransparencyExecutionMode::LegacyTwoBucket);
        CHECK(forcedLegacyMaterials.valid());
        CHECK(forcedLegacyMaterials.materials.size() == 2);
        const CanonicalMaterialAsset& forcedLegacySorted =
            forcedLegacyMaterials.materials[1].asset;
        CHECK(forcedLegacySorted.pipelineState.shaderProgram ==
            ShaderProgram::CanonicalComplexForward);
        CHECK(forcedLegacySorted.pipelineState.renderPass ==
            RenderPassClass::Forward);
        CHECK(forcedLegacySorted.pipelineState.blendMode ==
            BlendMode::AlphaBlend);
        CHECK((forcedLegacySorted.packed.featureFlags &
            MaterialFeatureClassifiedTransparencyExecution) == 0);

        const RuntimeModelCpuResult forcedLegacyRuntime =
            makeRuntimeModelCpuData(data, true,
                TransparencyExecutionMode::LegacyTwoBucket);
        CHECK(forcedLegacyRuntime.valid());
        CHECK(forcedLegacyRuntime.data->transparencyExecutionMode ==
            TransparencyExecutionMode::LegacyTwoBucket);
        return true;
    }

    bool testClassifiedPrimitivePolicyMaterialVariants() {
        CookedModelProductData data = fixture();
        data.manifest.transparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        const AssetGuid sharedMaterial = data.materials[1].materialGuid;
        CompiledTransparencyPolicy thin =
            data.materials[1].compiled.transparency;
        thin.requestedClass = TransparencyClass::ThinGlass;
        thin.resolvedClass = TransparencyClass::ThinGlass;
        thin.flags = CompiledTransparencyExplicitClass;
        data.manifest.primitives[0].materialGuid = sharedMaterial;
        data.manifest.primitives[0].coverage =
            ModelCoverage::Transparent;
        data.manifest.primitives[0].flags =
            ModelPrimitiveDoubleSided;
        data.manifest.primitives[0].rtFlags =
            ModelRtBuildInput | ModelRtAllowAnyHit;
        data.manifest.primitives[0].transparency = thin;
        data.manifest.primitives[1].transparency =
            data.materials[1].compiled.transparency;
        CHECK(validateModelProduct(data).empty());

        const RuntimeMaterialFallbacks fallbacks{
            .white = {
                TextureHandle::fromParts(1, 1),
                SamplerHandle::fromParts(1, 1),
            },
            .normal = {
                TextureHandle::fromParts(2, 1),
                SamplerHandle::fromParts(2, 1),
            },
            .linearData = {
                TextureHandle::fromParts(3, 1),
                SamplerHandle::fromParts(3, 1),
            },
        };
        const RuntimeCanonicalMaterialResult canonical =
            makeRuntimeCanonicalMaterials(data, {}, fallbacks);
        CHECK(canonical.valid());
        CHECK(std::ranges::count_if(canonical.materials,
            [&](const RuntimeCanonicalMaterial& material) {
                return material.materialGuid == sharedMaterial;
            }) == 2);
        const auto thinMaterial = std::ranges::find_if(
            canonical.materials,
            [](const RuntimeCanonicalMaterial& material) {
                return material.transparency.resolvedClass ==
                    TransparencyClass::ThinGlass;
            });
        CHECK(thinMaterial != canonical.materials.end());
        CHECK(thinMaterial->asset.pipelineState.blendMode ==
            BlendMode::PremultipliedAlpha);
        CHECK((thinMaterial->asset.packed.featureFlags &
            MaterialFeatureClassifiedTransparencyExecution) != 0);

        std::vector<RuntimeMaterialBinding> bindings;
        for (size_t index = 0; index < canonical.materials.size(); ++index) {
            const RuntimeCanonicalMaterial& material =
                canonical.materials[index];
            bindings.push_back({
                .materialGuid = material.materialGuid,
                .transparency = material.transparency,
                .binding = {
                    .material = MaterialHandle::fromParts(
                        static_cast<uint32_t>(index + 1), 1),
                    .pipeline = PipelineHandle::fromParts(
                        static_cast<uint32_t>(index + 1), 1),
                    .renderQueue = RenderQueue::Transparent,
                },
            });
        }
        RuntimeModelCpuResult geometry = makeRuntimeModelCpuData(data);
        CHECK(geometry.valid());
        CHECK(geometry.data->geometryArena.uint16Indices ==
            std::vector<uint16_t>({ 0, 1, 2, 0, 1, 2 }));
        CHECK(geometry.data->geometryArena.uint32Indices.empty());
        CHECK(geometry.data->geometryArena.stats.savedIndexBytes == 12);
        ResolvedRuntimeModelCpuResult resolved =
            resolveRuntimeModelMaterials(
                std::move(*geometry.data), bindings);
        CHECK(resolved.valid());
        CHECK(resolved.data->geometry.primitives[0].materialIndex !=
            resolved.data->geometry.primitives[1].materialIndex);
        CHECK(resolved.data->materials.size() == 2);
        return true;
    }

    bool testOptionalLodSectionRoundTripAndValidation() {
        const CookedModelProductData source = lodFixture();
        CHECK(validateModelProduct(source).empty());
        const CookProduct product = makeCookedModelProduct(source);
        CHECK(!hasCookErrors(product.diagnostics));
        CHECK(product.sections.size() == 8);
        CHECK(hasCookErrors(makeCookedModelProduct(source,
            kPreviousCookedModelSchemaVersion).diagnostics));
        CHECK(hasCookErrors(makeCookedModelProduct(source, 999).diagnostics));
        const CookSection* lodSection =
            findSection(product, kCookedModelLodSection);
        CHECK(lodSection);
        CHECK(lodSection->schemaVersion ==
            kCookedModelLodSectionSchemaVersion);
        std::vector<CookDiagnostic> diagnostics;
        const auto chains = readModelLodChains(
            lodSection->bytes, diagnostics);
        CHECK(diagnostics.empty());
        CHECK(chains && *chains == source.lodChains);

        CookedArtifact artifact{
            .artifactType = product.artifactType,
            .artifactSchemaVersion = product.artifactSchemaVersion,
            .sections = product.sections,
        };
        const CookedModelReadResult decoded =
            readCookedModelProduct(artifact);
        CHECK(decoded.valid());
        CHECK(*decoded.data == source);
        const RuntimeModelCpuResult runtime =
            makeRuntimeModelCpuData(*decoded.data);
        CHECK(runtime.valid());
        CHECK(runtime.data->primitives.size() == 2);
        CHECK(runtime.data->geometryArena.primitives.size() == 3);
        CHECK(runtime.data->lodChains.size() == 1);
        CHECK(runtime.data->lodChains[0].levels.size() == 2);
        CHECK(runtime.data->lodChains[0].basePrimitiveGuid ==
            source.manifest.primitives[0].primitiveGuid);
        CHECK(runtime.data->lodChains[0].levels[1].subMesh.primitiveGuid ==
            source.manifest.primitives[2].primitiveGuid);
        CHECK(runtime.data->lodChains[0].levels[1].geometricError == 0.25f);
        RuntimeModelCpuData residentFallback = *runtime.data;
        const RuntimeModelLodResidencyStats residency =
            applyRuntimeModelLodResidencyFloor(residentFallback, 1u);
        CHECK(residency.requestedMinimumLevel == 1u);
        CHECK(residency.maximumAppliedLevel == 1u);
        CHECK(residency.fallbackChainCount == 1u);
        CHECK(residency.withheldPrimitiveRangeCount == 1u);
        CHECK(residency.residentIndexBytes < residency.originalIndexBytes);
        CHECK(residency.withheldIndexBytes() ==
            residency.originalIndexBytes - residency.residentIndexBytes);
        CHECK(residentFallback.geometryArena.primitives.size() == 2u);
        CHECK(residentFallback.lodChains[0].residentBaseLevel == 1u);
        CHECK(residentFallback.lodChains[0].levels.size() == 1u);
        CHECK(residentFallback.primitives[0].primitiveGuid ==
            source.manifest.primitives[0].primitiveGuid);
        CHECK(residentFallback.primitives[0].indexCount == 3u);
        CHECK(std::ranges::find(residentFallback.geometryArena.primitives,
            GeometryArenaPrimitiveIdentity{
                source.manifest.primitives[0].sourcePrimitiveGuid,
                source.manifest.primitives[0].primitiveGuid },
            &GeometryArenaPrimitiveRange::identity) !=
            residentFallback.geometryArena.primitives.end());
        CHECK(std::ranges::find(residentFallback.geometryArena.primitives,
            GeometryArenaPrimitiveIdentity{
                source.manifest.primitives[2].sourcePrimitiveGuid,
                source.manifest.primitives[2].primitiveGuid },
            &GeometryArenaPrimitiveRange::identity) ==
            residentFallback.geometryArena.primitives.end());
        RuntimeModelCpuData clampedFallback = *runtime.data;
        const RuntimeModelLodResidencyStats clampedResidency =
            applyRuntimeModelLodResidencyFloor(clampedFallback, 15u);
        CHECK(clampedResidency.requestedMinimumLevel == 15u);
        CHECK(clampedResidency.maximumAppliedLevel == 1u);
        CHECK(clampedResidency.withheldIndexBytes() ==
            residency.withheldIndexBytes());
        bool residencyThrew = false;
        try {
            (void)applyRuntimeModelLodResidencyFloor(
                residentFallback, MaximumGpuSceneLodLevels);
        } catch (const std::invalid_argument&) {
            residencyThrew = true;
        }
        CHECK(residencyThrew);
        std::vector<RuntimeMaterialBinding> bindings;
        for (size_t index = 0; index < source.materials.size(); ++index) {
            bindings.push_back({
                .materialGuid = source.materials[index].materialGuid,
                .transparency = source.manifest.primitives[index].transparency,
                .binding = {
                    .material = MaterialHandle::fromParts(
                        static_cast<uint32_t>(index + 1u), 1u),
                    .pipeline = PipelineHandle::fromParts(
                        static_cast<uint32_t>(index + 1u), 1u),
                },
            });
        }
        const auto resolved = resolveRuntimeModelMaterials(*runtime.data, bindings);
        CHECK(resolved.valid());
        CHECK(resolved.data->materials.size() == 2u);
        CHECK(resolved.data->geometry.lodChains[0].levels[1].subMesh.materialIndex ==
            resolved.data->geometry.primitives[0].materialIndex);
        auto mislabelled = artifact;
        mislabelled.artifactSchemaVersion = kPreviousCookedModelSchemaVersion;
        auto oldManifest = source.manifest;
        oldManifest.schemaVersion = kPreviousCookedModelSchemaVersion;
        for (CookSection& section : mislabelled.sections) {
            if (section.id == kCookedModelManifestSection) {
                section.schemaVersion = kPreviousCookedModelSchemaVersion;
                section.bytes = serializeModelManifest(oldManifest);
            }
        }
        CHECK(!readCookedModelProduct(mislabelled).valid());

        CookedModelProductData invalid = source;
        invalid.lodChains[0].levels[1].geometricError = -1.0f;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        invalid = source;
        invalid.manifest.primitives[2].materialGuid =
            invalid.manifest.primitives[1].materialGuid;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        invalid = source;
        invalid.lodChains[0].levels[1].primitiveIndex = 0;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        invalid = source;
        invalid.lodChains[0].levels[1].primitiveIndex = UINT32_MAX;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        CHECK(!makeRuntimeModelCpuData(invalid, false).valid());
        invalid = source;
        invalid.lodChains[0].basePrimitiveIndex = UINT32_MAX;
        CHECK(!makeRuntimeModelCpuData(invalid, false).valid());
        invalid = source;
        ++invalid.manifest.primitives[2].sourceNode;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        invalid = source;
        invalid.manifest.primitives[2].indexCount = 6;
        CHECK(hasCookErrors(validateModelProduct(invalid)));
        auto malformedBytes = lodSection->bytes;
        malformedBytes[8] = std::byte{ 99 };
        diagnostics.clear();
        CHECK(!readModelLodChains(malformedBytes, diagnostics));
        diagnostics.clear();
        CHECK(!readModelLodChains(std::span(lodSection->bytes).first(31),
            diagnostics));
        malformedBytes = lodSection->bytes;
        // First-level offset must start at zero and remain inside the table.
        malformedBytes[36] = std::byte{ 255 };
        diagnostics.clear();
        CHECK(!readModelLodChains(malformedBytes, diagnostics));

        artifact.sections.erase(std::remove_if(artifact.sections.begin(),
            artifact.sections.end(), [](const CookSection& section) {
                return section.id == kCookedModelLodSection;
            }), artifact.sections.end());
        CHECK(!readCookedModelProduct(artifact).valid());
        return true;
    }

    bool testDeterministicBoundedLodGeneration() {
        constexpr uint32_t side = 9;
        std::vector<CookedModelVertex> vertices;
        vertices.reserve(side * side);
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                vertices.push_back({
                    .position = { static_cast<float>(x),
                        static_cast<float>(y), 0.0f },
                    .normal = { 0.0f, 0.0f, 1.0f },
                    .texCoord0 = { static_cast<float>(x) / (side - 1u),
                        static_cast<float>(y) / (side - 1u) },
                    .tangent = { 1.0f, 0.0f, 0.0f, 1.0f },
                });
            }
        }
        std::vector<uint32_t> indices;
        for (uint32_t y = 0; y + 1u < side; ++y) {
            for (uint32_t x = 0; x + 1u < side; ++x) {
                const uint32_t a = y * side + x;
                const uint32_t b = a + 1u;
                const uint32_t c = a + side;
                const uint32_t d = c + 1u;
                indices.insert(indices.end(), { a, b, c, b, d, c });
            }
        }
        const ModelLodGenerationSettings settings{
            .maximumRelativeErrors = { 0.101f, 0.251f, 0.501f },
            .minimumSourceTriangles = 8,
            .maximumTexCoordDelta = 1.0f,
        };
        ModelLodGenerationStatistics firstStatistics;
        const auto first = generateModelLods(
            vertices, indices, settings, {}, &firstStatistics);
        const auto second = generateModelLods(vertices, indices, settings);
        CHECK(first == second);
        CHECK(!first.empty());
        CHECK(firstStatistics.canonicalPrimitiveCount == 1u);
        CHECK(firstStatistics.canonicalTriangleCount == indices.size() / 3u);
        CHECK(firstStatistics.attemptedPrimitiveCount == 1u);
        CHECK(firstStatistics.generatedChainCount == 1u);
        CHECK(firstStatistics.generatedLevelCount == first.size());
        CHECK(firstStatistics.acceptedMergeCount > 0u);
        uint64_t acceptedOutcomeCount = 0u;
        for (size_t budget = 0; budget < ModelLodBudgetCount; ++budget) {
            uint64_t budgetOutcomeCount = 0u;
            for (size_t outcome = 0;
                    outcome < ModelLodLevelOutcomeCount; ++outcome) {
                budgetOutcomeCount +=
                    firstStatistics.levelOutcomeCounts[budget][outcome];
                CHECK(firstStatistics
                    .levelOutcomeTriangleCounts[budget][outcome] ==
                    firstStatistics.levelOutcomeCounts[budget][outcome] *
                        (indices.size() / 3u));
            }
            CHECK(budgetOutcomeCount == 1u);
            acceptedOutcomeCount += firstStatistics.levelOutcomeCounts[budget][
                static_cast<size_t>(ModelLodLevelOutcome::Accepted)];
        }
        CHECK(acceptedOutcomeCount == firstStatistics.generatedLevelCount);
        size_t previousIndices = indices.size();
        float previousError = 0.0f;
        for (const GeneratedModelLod& level : first) {
            CHECK(!level.vertices.empty());
            CHECK(level.indices.size() % 3u == 0u);
            CHECK(level.indices.size() < previousIndices);
            CHECK(level.geometricError >= previousError);
            CHECK(std::ranges::all_of(level.indices,
                [&](uint32_t index) { return index < level.vertices.size(); }));
            for (uint32_t y = 0; y < side; ++y) {
                for (uint32_t x = 0; x < side; ++x) {
                    const CookedModelVertex& source = vertices[y * side + x];
                    if (x == 0u || y == 0u || x + 1u == side || y + 1u == side)
                        CHECK(std::ranges::find(level.vertices, source) !=
                            level.vertices.end());
                    float nearest = (std::numeric_limits<float>::max)();
                    for (const CookedModelVertex& reduced : level.vertices) {
                        float squared = 0.0f;
                        for (uint32_t axis = 0; axis < 3u; ++axis) {
                            const float delta = source.position[axis] -
                                reduced.position[axis];
                            squared += delta * delta;
                        }
                        nearest = (std::min)(nearest, std::sqrt(squared));
                    }
                    CHECK(nearest <= level.geometricError + 1.0e-6f);
                }
            }
            std::cout << "LOD grid: " << indices.size() / 3u << " -> "
                << level.indices.size() / 3u << " triangles; error "
                << level.geometricError << '\n';
            previousIndices = level.indices.size();
            previousError = level.geometricError;
        }

        CookedModelProductData product = fixture();
        product.vertices = vertices;
        product.indices = indices;
        product.rtPositions.clear();
        for (const auto& vertex : vertices)
            product.rtPositions.push_back(vertex.position);
        product.rtIndices = indices;
        product.materials.resize(1);
        product.manifest.primitives.resize(1);
        auto& primitive = product.manifest.primitives[0];
        primitive.vertexCount = vertices.size();
        primitive.indexCount = indices.size();
        primitive.rtPositionCount = vertices.size();
        primitive.rtIndexCount = indices.size();
        primitive.bounds = {
            .aabbMin = { 0.0f, 0.0f, 0.0f },
            .aabbMax = { 8.0f, 8.0f, 0.0f },
            .sphereCenter = { 4.0f, 4.0f, 0.0f },
            .sphereRadius = std::sqrt(32.0f),
        };
        product.manifest.vertexCount = vertices.size();
        product.manifest.indexCount = indices.size();
        product.manifest.rtPositionCount = vertices.size();
        product.manifest.rtIndexCount = indices.size();
        CHECK(validateModelProduct(product).empty());
        const auto canonicalVertices = product.vertices;
        const auto canonicalIndices = product.indices;
        const auto canonicalRtPositions = product.rtPositions;
        const auto canonicalRtIndices = product.rtIndices;
        const auto canonicalMaterials = product.materials;
        const CookedModelPrimitive canonicalPrimitive = primitive;
        auto repeated = product;
        ModelLodGenerationStatistics productStatistics;
        ModelLodGenerationStatistics repeatedStatistics;
        CHECK(appendGeneratedModelLods(
            product, settings, {}, &productStatistics) == 1u);
        CHECK(appendGeneratedModelLods(
            repeated, settings, {}, &repeatedStatistics) == 1u);
        CHECK(product == repeated);
        CHECK(productStatistics == repeatedStatistics);
        CHECK(productStatistics.canonicalPrimitiveCount == 1u);
        CHECK(productStatistics.generatedChainCount == 1u);
        CHECK(productStatistics.generatedBaseTriangleCount == indices.size() / 3u);
        CHECK(std::ranges::equal(canonicalVertices,
            std::span(product.vertices).first(canonicalVertices.size())));
        CHECK(std::ranges::equal(canonicalIndices,
            std::span(product.indices).first(canonicalIndices.size())));
        CHECK(product.rtPositions == canonicalRtPositions);
        CHECK(product.rtIndices == canonicalRtIndices);
        CHECK(product.materials == canonicalMaterials);
        CookedModelPrimitive linkedCanonicalPrimitive = canonicalPrimitive;
        linkedCanonicalPrimitive.lodSection = kCookedModelLodSection;
        CHECK(product.manifest.primitives[0] == linkedCanonicalPrimitive);
        CHECK(product.lodChains.size() == 1u);
        CHECK(product.lodChains[0].levels.size() == first.size() + 1u);
        CHECK(validateModelProduct(product).empty());
        const auto cooked = makeCookedModelProduct(product);
        CHECK(!hasCookErrors(cooked.diagnostics));
        const auto loaded = readCookedModelProduct({
            .artifactType = cooked.artifactType,
            .artifactSchemaVersion = cooked.artifactSchemaVersion,
            .sections = cooked.sections,
        });
        CHECK(loaded.valid());
        CHECK(*loaded.data == product);
        const auto runtime = makeRuntimeModelCpuData(*loaded.data);
        CHECK(runtime.valid());
        CHECK(runtime.data->primitives.size() == 1u);
        CHECK(runtime.data->lodChains.size() == 1u);
        CHECK(runtime.data->geometryArena.primitives.size() == first.size() + 1u);
        const auto childMask = makeCookedModelLodChildMask(product);
        CHECK(!childMask[0]);
        CHECK(std::ranges::count(childMask, true) == first.size());
        CHECK(appendGeneratedModelLods(product, settings) == 0u);
        CHECK(product == repeated);

        std::stop_source cancelled;
        cancelled.request_stop();
        bool cancelledThrew = false;
        try {
            (void)appendGeneratedModelLods(product, settings, cancelled.get_token());
        } catch (const std::runtime_error&) { cancelledThrew = true; }
        CHECK(cancelledThrew);
        CHECK(product == repeated);
        bool threw = false;
        try {
            (void)generateModelLods(vertices,
                std::span<const uint32_t>(indices).first(indices.size() - 1u),
                settings);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        auto lockedUv = settings;
        lockedUv.maximumTexCoordDelta = 0.0f;
        ModelLodGenerationStatistics lockedUvStatistics;
        CHECK(generateModelLods(vertices, indices, lockedUv, {},
            &lockedUvStatistics).empty());
        CHECK(lockedUvStatistics.noAcceptedLevelPrimitiveCount == 1u);
        CHECK(lockedUvStatistics.noAcceptedTerminalOutcomePrimitiveCounts[
            static_cast<size_t>(ModelLodLevelOutcome::NoReduction)] == 1u);
        auto boundedBoundary = settings;
        boundedBoundary.allowBoundaryEdgeCollapse = true;
        ModelLodGenerationStatistics boundaryFirstStatistics;
        ModelLodGenerationStatistics boundarySecondStatistics;
        const auto boundaryFirst = generateModelLods(vertices, indices,
            boundedBoundary, {}, &boundaryFirstStatistics);
        const auto boundarySecond = generateModelLods(vertices, indices,
            boundedBoundary, {}, &boundarySecondStatistics);
        CHECK(boundaryFirst == boundarySecond);
        CHECK(boundaryFirstStatistics == boundarySecondStatistics);
        CHECK(!boundaryFirst.empty());
        CHECK(boundaryFirst.front().indices.size() < first.front().indices.size());
        CHECK(boundaryFirst.front().geometricError <=
            boundedBoundary.maximumRelativeErrors.front() * std::sqrt(128.0f) +
                1.0e-6f);
        bool removedBoundaryVertex = false;
        for (uint32_t x = 0; x < side; ++x) {
            removedBoundaryVertex = removedBoundaryVertex ||
                std::ranges::find(boundaryFirst.front().vertices, vertices[x]) ==
                    boundaryFirst.front().vertices.end();
        }
        CHECK(removedBoundaryVertex);
        // Add a disconnected seven-vertex torus triangulation. Every vertex pair
        // is an edge, so each edge has common-link vertices beyond its two face
        // neighbors. Collapsing one would change genus even though the source is
        // a consistently oriented closed two-manifold. The planar component still
        // supplies useful reductions after those torus merges are rejected.
        auto topologyVertices = vertices;
        auto topologyIndices = indices;
        const uint32_t torusStart =
            static_cast<uint32_t>(topologyVertices.size());
        for (uint32_t vertex = 0; vertex < 7u; ++vertex) {
            auto value = vertices[0];
            const float parameter = static_cast<float>(vertex);
            value.position = { 20.0f + parameter * 0.1f,
                parameter * parameter * 0.01f,
                parameter * parameter * parameter * 0.001f };
            topologyVertices.push_back(value);
        }
        for (uint32_t vertex = 0; vertex < 7u; ++vertex) {
            const uint32_t one = (vertex + 1u) % 7u;
            const uint32_t two = (vertex + 2u) % 7u;
            const uint32_t three = (vertex + 3u) % 7u;
            topologyIndices.insert(topologyIndices.end(), {
                torusStart + vertex, torusStart + one, torusStart + three,
                torusStart + vertex, torusStart + three, torusStart + two,
            });
        }
        auto topologyGrid = boundedBoundary;
        topologyGrid.preventOrientationChangingMerges = true;
        topologyGrid.preventTopologyChangingMerges = true;
        ModelLodGenerationStatistics topologyGridFirstStatistics;
        ModelLodGenerationStatistics topologyGridSecondStatistics;
        const auto topologyGridFirst = generateModelLods(topologyVertices,
            topologyIndices, topologyGrid, {}, &topologyGridFirstStatistics);
        const auto topologyGridSecond = generateModelLods(topologyVertices,
            topologyIndices, topologyGrid, {}, &topologyGridSecondStatistics);
        CHECK(topologyGridFirst == topologyGridSecond);
        CHECK(topologyGridFirstStatistics == topologyGridSecondStatistics);
        CHECK(!topologyGridFirst.empty());
        CHECK(topologyGridFirstStatistics.topologyRejectedEdgeCount > 0u);
        CHECK(topologyGridFirstStatistics.topologyRejectedLevelCount == 0u);
        auto belowMinimum = settings;
        belowMinimum.minimumSourceTriangles =
            static_cast<uint32_t>(indices.size() / 3u + 1u);
        ModelLodGenerationStatistics belowMinimumStatistics;
        CHECK(generateModelLods(vertices, indices, belowMinimum, {},
            &belowMinimumStatistics).empty());
        CHECK(belowMinimumStatistics.belowMinimumPrimitiveCount == 1u);
        CHECK(belowMinimumStatistics.generatedChainCount == 0u);

        // Two otherwise-manifold boundary fans may meet at one authored vertex.
        // The opt-in normalization duplicates only that singular junction in the
        // generated child and then runs the unchanged topology/error proofs.
        auto bowTieVertices = vertices;
        auto bowTieIndices = indices;
        const uint32_t bowTieStart =
            static_cast<uint32_t>(bowTieVertices.size());
        auto bowTieA = vertices[0];
        bowTieA.position = { 20.0f, 0.0f, 0.0f };
        auto bowTieB = vertices[0];
        bowTieB.position = { 20.0f, 1.0f, 0.0f };
        bowTieVertices.push_back(bowTieA);
        bowTieVertices.push_back(bowTieB);
        bowTieIndices.insert(bowTieIndices.end(),
            { 0u, bowTieStart, bowTieStart + 1u });
        ModelLodGenerationStatistics bowTieRejectedStatistics;
        CHECK(generateModelLods(bowTieVertices, bowTieIndices, settings, {},
            &bowTieRejectedStatistics).empty());
        CHECK(bowTieRejectedStatistics.invalidSourceTopologyPrimitiveCount == 1u);
        CHECK(bowTieRejectedStatistics.sourceTopologyFailurePrimitiveCounts[
            static_cast<size_t>(ModelLodTopologyFailure::BoundaryDegree)] == 1u);
        auto splitFans = settings;
        splitFans.splitNonManifoldBoundaryFans = true;
        ModelLodGenerationStatistics splitFanStatistics;
        const auto splitFanLevels = generateModelLods(bowTieVertices,
            bowTieIndices, splitFans, {}, &splitFanStatistics);
        CHECK(!splitFanLevels.empty());
        CHECK(splitFanStatistics.invalidSourceTopologyPrimitiveCount == 0u);
        CHECK(splitFanStatistics.splitBoundaryFanVertexCount == 1u);
        CHECK(splitFanStatistics.duplicatedBoundaryFanVertexCount == 1u);
        CHECK(splitFanStatistics.generatedChainCount == 1u);
        auto transactional = splitFans;
        transactional.preventOrientationChangingMerges = true;
        ModelLodGenerationStatistics transactionalFirstStatistics;
        ModelLodGenerationStatistics transactionalSecondStatistics;
        const auto transactionalFirst = generateModelLods(bowTieVertices,
            bowTieIndices, transactional, {}, &transactionalFirstStatistics);
        const auto transactionalSecond = generateModelLods(bowTieVertices,
            bowTieIndices, transactional, {}, &transactionalSecondStatistics);
        CHECK(transactionalFirst == transactionalSecond);
        CHECK(transactionalFirstStatistics == transactionalSecondStatistics);
        CHECK(!transactionalFirst.empty());
        CHECK(transactionalFirstStatistics.orientationRejectedLevelCount == 0u);
        auto topologyTransactional = transactional;
        topologyTransactional.preventTopologyChangingMerges = true;
        ModelLodGenerationStatistics topologyFirstStatistics;
        ModelLodGenerationStatistics topologySecondStatistics;
        const auto topologyFirst = generateModelLods(bowTieVertices,
            bowTieIndices, topologyTransactional, {}, &topologyFirstStatistics);
        const auto topologySecond = generateModelLods(bowTieVertices,
            bowTieIndices, topologyTransactional, {}, &topologySecondStatistics);
        CHECK(topologyFirst == topologySecond);
        CHECK(topologyFirstStatistics == topologySecondStatistics);
        CHECK(!topologyFirst.empty());
        CHECK(topologyFirstStatistics.topologyRejectedLevelCount == 0u);

        auto invalidSettings = settings;
        invalidSettings.maximumRelativeErrors[1] = -1.0f;
        threw = false;
        try {
            (void)generateModelLods(vertices, indices, invalidSettings);
        } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        vertices[0].color[0] = std::numeric_limits<float>::quiet_NaN();
        threw = false;
        try {
            (void)generateModelLods(vertices, indices, settings);
        } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        vertices[0].color[0] = 1.0f;

        // A tiny disconnected surface must remain present in every generated
        // level even when its size is far below the grid's error budget.
        const uint32_t islandStart = static_cast<uint32_t>(vertices.size());
        for (const std::array<float, 3> position : {
                std::array{ 20.0f, 0.0f, 0.0f },
                std::array{ 20.01f, 0.0f, 0.0f },
                std::array{ 20.0f, 0.01f, 0.0f } }) {
            auto vertex = vertices[0];
            vertex.position = position;
            vertices.push_back(vertex);
        }
        indices.insert(indices.end(),
            { islandStart, islandStart + 1u, islandStart + 2u });
        const auto withIsland = generateModelLods(vertices, indices, settings);
        CHECK(!withIsland.empty());
        for (const auto& level : withIsland) {
            for (uint32_t index = islandStart; index < islandStart + 3u; ++index)
                CHECK(std::ranges::find(level.vertices, vertices[index]) !=
                    level.vertices.end());
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*function)();
    };
    const std::vector<TestCase> tests{
        { "deterministic round trip and primitive preservation",
            testDeterministicRoundTripAndPrimitivePreservation },
        { "RT reconstruction", testRtGeometryReconstructsCanonicalTriangles },
        { "semantic validation", testValidationRejectsSemanticLossAndBadRanges },
        { "manifest corruption", testManifestRejectsCorruption },
        { "classified SortedSurface runtime pipeline",
            testClassifiedSortedSurfaceRuntimePipeline },
        { "classified primitive policy material variants",
            testClassifiedPrimitivePolicyMaterialVariants },
        { "optional LOD section round trip and validation",
            testOptionalLodSectionRoundTripAndValidation },
        { "deterministic bounded LOD generation",
            testDeterministicBoundedLodGeneration },
    };

    for (const TestCase& test : tests) {
        if (!test.function()) {
            std::cerr << "FAILED: " << test.name << '\n';
            return 1;
        }
        std::cout << "PASSED: " << test.name << '\n';
    }
    return 0;
}
