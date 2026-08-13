#include "assets/model/ModelProduct.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
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
                    .bounds = {
                        .aabbMin = { 0.0f, 0.0f, 0.0f },
                        .aabbMax = { 1.0f, 1.0f, 0.0f },
                        .sphereCenter = { 0.5f, 0.5f, 0.0f },
                        .sphereRadius = 0.707107f,
                    },
                },
                {
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
        const auto indices =
            readModelIndices(indexSection->bytes, diagnostics);
        const auto rtPositions =
            readModelRtPositions(rtPositionSection->bytes, diagnostics);
        const auto rtIndices = readModelIndices(
            rtIndexSection->bytes, diagnostics, "/rt_indices");
        CHECK(diagnostics.empty());
        CHECK(manifest && materials && textureViews &&
            vertices && indices &&
            rtPositions && rtIndices);
        CHECK(*manifest == source.manifest);
        CHECK(*materials == source.materials);
        CHECK(*textureViews == source.textureViews);
        CHECK(*vertices == source.vertices);
        CHECK(*indices == source.indices);
        CHECK(*rtPositions == source.rtPositions);
        CHECK(*rtIndices == source.rtIndices);

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
