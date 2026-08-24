#include "material/MaterialTangentGeneration.h"
#include "material/StandardMaterialShading.h"
#include "material/ComplexMaterialShading.h"
#include "material/TransparencyTransport.h"
#include "renderer/rhi/Mesh.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

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

    bool testMetricTransparencyTransport() {
        const DielectricFresnelResult normal = dielectricFresnel(
            1.0f, 1.5f, 1.0f);
        CHECK(!normal.totalInternalReflection);
        CHECK(!normal.sanitized);
        CHECK(near(normal.reflectance, 0.04f));

        const DielectricFresnelResult sixty = dielectricFresnel(
            1.0f, 1.5f, 0.5f);
        CHECK(!sixty.totalInternalReflection);
        CHECK(near(sixty.reflectance, 0.089186713f));
        const DielectricFresnelResult tir = dielectricFresnel(
            1.5f, 1.0f, std::cos(glm::radians(50.0f)));
        CHECK(tir.totalInternalReflection);
        CHECK(near(tir.reflectance, 1.0f));

        const BeerLambertResult zero = beerLambertTransmittance(
            { 0.8f, 0.5f, 0.25f }, 2.0f, 0.0f);
        CHECK(near(zero.transmittance, glm::vec3(1.0f)));
        const BeerLambertResult half = beerLambertTransmittance(
            { 0.8f, 0.5f, 0.25f }, 2.0f, 1.0f);
        CHECK(near(half.transmittance,
            { 0.89442719f, 0.70710678f, 0.5f }));
        const BeerLambertResult full = beerLambertTransmittance(
            { 0.8f, 0.5f, 0.25f }, 2.0f, 2.0f);
        CHECK(near(full.transmittance, { 0.8f, 0.5f, 0.25f }));
        const BeerLambertResult infinite = beerLambertTransmittance(
            glm::vec3(0.0f), std::numeric_limits<float>::infinity(), 1000.0f);
        CHECK(near(infinite.transmittance, glm::vec3(1.0f)));

        const ThinSheetPathResult zeroSheet = thinSheetPathLength(
            0.0f, 0.0f, 4.0f);
        CHECK(zeroSheet.pathLengthMeters == 0.0f);
        CHECK(!zeroSheet.grazingClampApplied);
        const ThinSheetPathResult oblique = thinSheetPathLength(
            0.01f, 0.5f, 2.0f);
        CHECK(near(oblique.pathLengthMeters, 0.04f));
        const ThinSheetPathResult grazing = thinSheetPathLength(
            0.01f, 0.0f);
        CHECK(grazing.grazingClampApplied);
        CHECK(near(grazing.pathLengthMeters, 0.2f));

        const DielectricFresnelResult invalidFresnel = dielectricFresnel(
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN());
        CHECK(invalidFresnel.sanitized);
        CHECK(std::isfinite(invalidFresnel.reflectance));
        const BeerLambertResult invalidBeer = beerLambertTransmittance(
            { -1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN() },
            -1.0f, std::numeric_limits<float>::infinity());
        CHECK(invalidBeer.sanitized);
        CHECK(finite(invalidBeer.transmittance));
        const ThinSheetPathResult invalidSheet = thinSheetPathLength(
            -1.0f, std::numeric_limits<float>::quiet_NaN(), -2.0f);
        CHECK(invalidSheet.sanitized);
        CHECK(invalidSheet.pathLengthMeters == 0.0f);

        const ThinGlassLocalCompositionWeights clearInterface =
            thinGlassLocalCompositionWeights(1.0f, 0.0f,
                glm::vec3(normal.reflectance));
        CHECK(near(clearInterface.effectiveTransmission, 1.0f));
        CHECK(near(clearInterface.interfaceOpacity, 0.04f));
        CHECK(near(clearInterface.destinationWeight, 0.96f));
        const ThinGlassLocalCompositionWeights opaqueInterface =
            thinGlassLocalCompositionWeights(0.0f, 0.0f,
                glm::vec3(0.04f));
        CHECK(near(opaqueInterface.destinationWeight, 0.0f));
        const ThinGlassLocalCompositionWeights metallicInterface =
            thinGlassLocalCompositionWeights(1.0f, 1.0f,
                glm::vec3(0.04f));
        CHECK(near(metallicInterface.effectiveTransmission, 0.0f));
        const ThinGlassLocalCompositionWeights invalidInterface =
            thinGlassLocalCompositionWeights(
                std::numeric_limits<float>::quiet_NaN(), -1.0f,
                { -1.0f, 2.0f,
                    std::numeric_limits<float>::infinity() });
        CHECK(invalidInterface.sanitized);
        CHECK(std::isfinite(invalidInterface.destinationWeight));

        for (float ior : { 1.0f, 1.1f, 1.5f, 2.5f, 10.0f }) {
            for (float cosine : { 0.0f, 0.05f, 0.25f, 0.75f, 1.0f }) {
                const auto sample = dielectricFresnel(1.0f, ior, cosine);
                CHECK(std::isfinite(sample.reflectance));
                CHECK(sample.reflectance >= 0.0f && sample.reflectance <= 1.0f);
                const auto path = thinSheetPathLength(0.01f, cosine);
                CHECK(std::isfinite(path.pathLengthMeters));
                CHECK(path.pathLengthMeters >= 0.01f);
            }
        }
        return true;
    }

    bool testActiveCameraDepthReconstruction() {
        const auto checkProjection = [](float nearPlane, float farPlane,
                glm::vec3 viewPosition) {
            glm::mat4 projection = glm::perspective(glm::radians(45.0f),
                16.0f / 9.0f, nearPlane, farPlane);
            projection[1][1] *= -1.0f;
            const ViewTransportRecord record = makeViewTransportRecord(
                glm::mat4(1.0f), projection, glm::vec3(0.0f),
                nearPlane, farPlane, { 3840u, 2160u });
            const glm::vec4 clip = projection * glm::vec4(viewPosition, 1.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const glm::vec2 screenUv = glm::vec2(ndc) * 0.5f + 0.5f;
            CHECK(near(reconstructViewPosition(record, screenUv, ndc.z),
                viewPosition, 0.0005f));
            CHECK(record.depthRange.x == nearPlane);
            CHECK(record.depthRange.y == farPlane);
            return true;
        };
        CHECK(checkProjection(0.1f, 100.0f, { 1.0f, -0.5f, -7.0f }));
        CHECK(checkProjection(0.01f, 1000.0f, { -0.25f, 0.125f, -3.0f }));
        return true;
    }

    bool testProjectedTransparencyPyramidTransport() {
        CHECK(transparencyPyramidMipCount(3840u, 2160u) == 12u);
        CHECK(transparencyPyramidMipCount(1u, 1u) == 1u);
        CHECK(transparencyPyramidMipCount(0u, 0u) == 0u);
        CHECK(transparencyPyramidTexelCount(4u, 4u) == 21u);
        CHECK(transparencyPyramidTexelCount(3u, 2u) == 7u);

        const RefractionDirectionResult normal = refractTransparencyRay(
            { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f }, 1.0f, 1.5f);
        CHECK(!normal.totalInternalReflection);
        CHECK(near(normal.direction, { 0.0f, 0.0f, -1.0f }));
        const RefractionDirectionResult bent = refractTransparencyRay(
            glm::normalize(glm::vec3(0.5f, 0.0f, -0.8660254f)),
            { 0.0f, 0.0f, 1.0f }, 1.0f, 1.5f);
        CHECK(!bent.totalInternalReflection);
        CHECK(std::abs(bent.direction.x) < 0.5f);
        const RefractionDirectionResult tir = refractTransparencyRay(
            glm::normalize(glm::vec3(0.7660444f, 0.0f, 0.6427876f)),
            { 0.0f, 0.0f, -1.0f }, 1.5f, 1.0f);
        CHECK(tir.totalInternalReflection);
        CHECK(finite(tir.direction));

        glm::mat4 projection = glm::perspective(glm::radians(60.0f),
            16.0f / 9.0f, 0.1f, 100.0f);
        projection[1][1] *= -1.0f;
        constexpr glm::uvec2 extent{ 3840u, 2160u };
        const uint32_t mips = transparencyPyramidMipCount(extent.x, extent.y);
        const RefractionProjectionResult zero = projectTransparencyRay(
            glm::mat4(1.0f), projection, { 0.0f, 0.0f, -5.0f },
            normal.direction, 0.0f, 1.0f, 1.0f, 1.0f / 1.5f,
            extent, mips);
        CHECK(zero.onScreen);
        CHECK(near(zero.sourceUv.x, zero.sampleUv.x));
        CHECK(near(zero.sourceUv.y, zero.sampleUv.y));
        CHECK(near(zero.footprintPixels, 1.0f));
        CHECK(near(zero.lod, 0.0f));

        const RefractionProjectionResult smooth = projectTransparencyRay(
            glm::mat4(1.0f), projection, { 0.0f, 0.0f, -5.0f },
            bent.direction, 0.2f, 1.0f, 0.0f, 1.0f / 1.5f,
            extent, mips);
        const RefractionProjectionResult rough = projectTransparencyRay(
            glm::mat4(1.0f), projection, { 0.0f, 0.0f, -5.0f },
            bent.direction, 0.2f, 1.0f, 0.8f, 1.0f / 1.5f,
            extent, mips);
        CHECK(smooth.onScreen && rough.onScreen);
        CHECK(glm::length(smooth.sampleUv - smooth.sourceUv) > 0.0f);
        CHECK(near(smooth.footprintPixels, 1.0f));
        CHECK(rough.footprintPixels > smooth.footprintPixels);
        CHECK(rough.lod > smooth.lod);
        CHECK(rough.lod <= static_cast<float>(mips - 1u));

        const RefractionProjectionResult edge = projectTransparencyRay(
            glm::mat4(1.0f), projection, { 4.9f, 0.0f, -5.0f },
            bent.direction, 1.0f, 1.0f, 0.8f, 1.0f / 1.5f,
            extent, mips);
        CHECK(!edge.onScreen || edge.edgeConfidence < rough.edgeConfidence);
        const RefractionProjectionResult invalid = projectTransparencyRay(
            glm::mat4(1.0f), projection,
            glm::vec3(std::numeric_limits<float>::quiet_NaN()),
            glm::vec3(0.0f), -1.0f, 0.0f,
            std::numeric_limits<float>::quiet_NaN(), -1.0f,
            { 0u, 0u }, 0u);
        CHECK(invalid.sanitized);
        CHECK(!invalid.onScreen);
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
        CHECK(complex.find("include/transparency_transport.glsl") !=
            std::string::npos);
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
        CHECK(complex.find("classifiedLocalInterface") !=
            std::string::npos);
        CHECK(complex.find("iridiumThinGlassLocalCompositionWeights") !=
            std::string::npos);
        CHECK(complex.find("linearizeDepth") == std::string::npos);
        CHECK(complex.find("measuredThickness") == std::string::npos);
        CHECK(complex.find("opticalPathMeters") != std::string::npos);
        CHECK(complex.find("iridiumDielectricFresnel") != std::string::npos);
        CHECK(complex.find("iridiumBeerLambert") != std::string::npos);
        CHECK(complex.find("refractionColorPyramid") !=
            std::string::npos);
        CHECK(complex.find("iridiumProjectTransparencyRay") !=
            std::string::npos);
        CHECK(complex.find("iridiumSampleTransmissionEnvironment") !=
            std::string::npos);
        CHECK(sizeof(ViewTransportRecord) == 320);
        CHECK(sizeof(UniformBufferObject) == 384);
        CHECK(offsetof(UniformBufferObject, inverseProjection) == 256);
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
        { "metric transparency transport", testMetricTransparencyTransport },
        { "active-camera depth reconstruction", testActiveCameraDepthReconstruction },
        { "projected transparency pyramid transport", testProjectedTransparencyPyramidTransport },
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
