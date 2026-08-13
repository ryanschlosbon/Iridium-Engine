#include "material/MaterialTangentGeneration.h"
#include "material/StandardMaterialShading.h"
#include "material/ComplexMaterialShading.h"
#include "renderer/rhi/Mesh.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

    using namespace Iridium;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; return false; } } while (false)

    bool near(float lhs, float rhs, float tolerance = 0.00001f) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    bool near(glm::vec3 lhs, glm::vec3 rhs, float tolerance = 0.00001f) {
        return near(lhs.x, rhs.x, tolerance) && near(lhs.y, rhs.y, tolerance) &&
            near(lhs.z, rhs.z, tolerance);
    }

    bool finite(glm::vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }

    bool testFresnelAndGgxBoundaries() {
        const glm::vec3 f0(0.04f, 0.25f, 0.8f);
        CHECK(near(materialFresnelSchlick(f0, glm::vec3(1.0f), 1.0f), f0));
        CHECK(near(materialFresnelSchlick(f0, glm::vec3(1.0f), 0.0f),
            glm::vec3(1.0f)));
        CHECK(near(materialGgxAlpha(0.0f), 0.0016f));
        CHECK(near(materialGgxAlpha(1.0f), 1.0f));
        for (float roughness : { 0.0f, 0.04f, 1.0f }) {
            for (float cosine : { 0.0f, 0.5f, 1.0f }) {
                CHECK(std::isfinite(materialDistributionGgx(cosine, roughness)));
                CHECK(std::isfinite(materialGeometrySmith(cosine, cosine,
                    roughness)));
            }
        }
        CHECK(near(materialDiffuseWeight(glm::vec3(0.25f), 1.0f),
            glm::vec3(0.0f)));
        CHECK(near(materialDiffuseWeight(glm::vec3(0.25f), 0.0f),
            glm::vec3(0.75f)));
        const glm::vec3 brdf = materialEvaluateStandardBrdf(
            { 0.5f, 0.25f, 0.125f }, f0, glm::vec3(1.0f), 0.0f, 0.5f,
            { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f },
            glm::normalize(glm::vec3(1.0f)));
        CHECK(finite(brdf));
        CHECK(glm::all(glm::greaterThanEqual(brdf, glm::vec3(0.0f))));
        return true;
    }

    bool testTangentFrameEdgeCases() {
        const StandardTangentFrame front = buildStandardTangentFrame(
            { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, 1.0f, true, true);
        const StandardTangentFrame back = buildStandardTangentFrame(
            { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, 1.0f, true, false);
        CHECK(near(front.normal, { 0.0f, 0.0f, 1.0f }));
        CHECK(near(back.normal, { 0.0f, 0.0f, -1.0f }));
        CHECK(near(glm::dot(front.normal, front.tangent), 0.0f));
        CHECK(near(glm::dot(front.normal, front.bitangent), 0.0f));

        const StandardTangentFrame mirrored = buildStandardTangentFrame(
            { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, -1.0f);
        CHECK(near(mirrored.bitangent, -front.bitangent));
        const glm::vec3 positiveY = applyStandardTangentNormal(front,
            { 0.5f, 1.0f, 0.5f }, 1.0f);
        const glm::vec3 mirroredY = applyStandardTangentNormal(mirrored,
            { 0.5f, 1.0f, 0.5f }, 1.0f);
        CHECK(near(positiveY, -mirroredY));

        const StandardTangentFrame fallback = buildStandardTangentFrame(
            { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, 1.0f);
        CHECK(finite(fallback.tangent));
        CHECK(near(glm::length(fallback.tangent), 1.0f));
        return true;
    }

    std::array<Vertex, 3> triangle(glm::vec2 uv1, glm::vec2 uv2) {
        std::array<Vertex, 3> vertices{};
        vertices[0].pos = { 0.0f, 0.0f, 0.0f };
        vertices[1].pos = { 1.0f, 0.0f, 0.0f };
        vertices[2].pos = { 0.0f, 1.0f, 0.0f };
        for (Vertex& vertex : vertices) vertex.normal = { 0.0f, 0.0f, 1.0f };
        vertices[0].uv0 = { 0.0f, 0.0f };
        vertices[1].uv0 = uv1;
        vertices[2].uv0 = uv2;
        return vertices;
    }

    bool testGeneratedTangentsAndHandedness() {
        constexpr std::array<uint32_t, 3> indices{ 0, 1, 2 };
        auto regular = triangle({ 1.0f, 0.0f }, { 0.0f, 1.0f });
        TangentGenerationStats regularStats = generateMikkCompatibleTangents<Vertex>(
            regular, indices);
        CHECK(regularStats.degenerateUvTriangleCount == 0);
        CHECK(regularStats.fallbackVertexCount == 0);
        for (const Vertex& vertex : regular) {
            CHECK(near(glm::vec3(vertex.tangent), { 1.0f, 0.0f, 0.0f }));
            CHECK(vertex.tangent.w == 1.0f);
        }

        auto mirrored = triangle({ 0.0f, 1.0f }, { 1.0f, 0.0f });
        TangentGenerationStats mirroredStats = generateMikkCompatibleTangents<Vertex>(
            mirrored, indices);
        CHECK(mirroredStats.degenerateUvTriangleCount == 0);
        for (const Vertex& vertex : mirrored) CHECK(vertex.tangent.w == -1.0f);

        auto degenerate = triangle({ 0.0f, 0.0f }, { 0.0f, 0.0f });
        TangentGenerationStats degenerateStats = generateMikkCompatibleTangents<Vertex>(
            degenerate, indices);
        CHECK(degenerateStats.degenerateUvTriangleCount == 1);
        CHECK(degenerateStats.fallbackVertexCount == 3);
        for (const Vertex& vertex : degenerate) CHECK(finite(glm::vec3(vertex.tangent)));
        return true;
    }

    bool testRasterShadersUseSharedConventions() {
        const std::filesystem::path shaderRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "shaders";
        const std::string deferred = readText(
            shaderRoot / "include/canonical_lighting_body.glsl");
        const std::string complex = readText(
            shaderRoot / "include/complex_material_body.glsl");
        const std::string gbuffer = readText(
            shaderRoot / "include/canonical_gbuffer_body.glsl");
        CHECK(deferred.find("include/material_bsdf.glsl") != std::string::npos);
        CHECK(complex.find("include/material_complex.glsl") != std::string::npos);
        CHECK(complex.find("include/material_normal.glsl") != std::string::npos);
        CHECK(gbuffer.find("include/material_normal.glsl") != std::string::npos);
        CHECK(deferred.find("materialEvaluateCanonicalBrdf") != std::string::npos);
        CHECK(complex.find("materialEvaluateStandardBrdf") != std::string::npos);
        CHECK(deferred.find("float DistributionGGX") == std::string::npos);
        CHECK(deferred.find("vec3 FresnelSchlick") == std::string::npos);
        CHECK(complex.find("vec3 FresnelSchlick") == std::string::npos);
        CHECK(complex.find("materialEvaluateStandardBrdf") != std::string::npos);
        CHECK(complex.find("MATERIAL_SCHEMA_VERSION") != std::string::npos);
        CHECK(complex.find("binding = 21") != std::string::npos);
        CHECK(complex.find("lobe.type == 0u") != std::string::npos);
        CHECK(complex.find("lobe.type == 7u") != std::string::npos);
        CHECK(complex.find("outputAlpha = max(alpha, transmission)") !=
            std::string::npos);
        return true;
    }

    bool testComplexLobesAreExplicitAndEffective() {
        ComplexShadingInputs inputs{};
        inputs.baseColor = { 0.3f, 0.15f, 0.05f };
        inputs.f0 = { 0.04f, 0.04f, 0.04f };
        inputs.perceptualRoughness = 0.4f;
        const ComplexShadingResult base = evaluateComplexMaterial(inputs, {});

        const ComplexLobeRecord coat{ ComplexLobeType::Clearcoat,
            "KHR_materials_clearcoat", ClearcoatLobe{ 1.0f, 0.15f, 1.0f, 0u } };
        const ComplexShadingResult coated = evaluateComplexMaterial(inputs,
            std::span<const ComplexLobeRecord>(&coat, 1));
        CHECK(!near(base.reflectionBrdf, coated.reflectionBrdf));

        const ComplexLobeRecord iridescence{ ComplexLobeType::Iridescence,
            "KHR_materials_iridescence", IridescenceLobe{ 1.0f, 1.3f,
                200.0f, 500.0f, 0u } };
        const ComplexShadingResult film = evaluateComplexMaterial(inputs,
            std::span<const ComplexLobeRecord>(&iridescence, 1));
        CHECK(!near(base.reflectionBrdf, film.reflectionBrdf));

        const std::array<ComplexLobeRecord, 4> transport{
            ComplexLobeRecord{ ComplexLobeType::ThinTransmission,
                "KHR_materials_transmission", ThinTransmissionLobe{
                    0.75f, 1.45f, 1.0f, glm::vec3(1.0f), 0u } },
            ComplexLobeRecord{ ComplexLobeType::VolumeTransmission,
                "KHR_materials_volume", VolumeTransmissionLobe{
                    0.2f, 3.0f, glm::vec3(0.8f, 0.9f, 1.0f), 0u } },
            ComplexLobeRecord{ ComplexLobeType::Dispersion,
                "KHR_materials_dispersion", DispersionLobe{ 0.05f } },
            ComplexLobeRecord{ ComplexLobeType::DiffuseTransmission,
                "KHR_materials_diffuse_transmission", DiffuseTransmissionLobe{
                    0.3f, glm::vec3(1.0f, 0.5f, 0.25f), 0u } },
        };
        const ComplexShadingResult transported = evaluateComplexMaterial(inputs,
            transport);
        CHECK(near(transported.transmission, 0.75f));
        CHECK(near(transported.transmissionIor, 1.45f));
        CHECK(near(transported.volumeThickness, 0.2f));
        CHECK(near(transported.attenuationColor, { 0.8f, 0.9f, 1.0f }));
        CHECK(near(transported.dispersion, 0.05f));
        CHECK(near(transported.diffuseTransmission, 0.3f));
        return true;
    }

}

int main() {
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        { "Fresnel and GGX boundaries", testFresnelAndGgxBoundaries },
        { "tangent-frame edge cases", testTangentFrameEdgeCases },
        { "generated tangent handedness", testGeneratedTangentsAndHandedness },
        { "shared raster shader conventions", testRasterShadersUseSharedConventions },
        { "complex lobe evaluation", testComplexLobesAreExplicitAndEffective },
    };
    for (const Test& test : tests) {
        std::cout << test.name << "\n";
        if (!test.run()) return 1;
    }
    std::cout << "Standard material shading tests passed\n";
    return 0;
}
