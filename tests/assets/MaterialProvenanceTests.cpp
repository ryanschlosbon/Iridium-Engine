#include "assets/MaterialProvenance.h"

#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    bool near(float lhs, float rhs) {
        return std::abs(lhs - rhs) < 0.00001f;
    }

    const MaterialTextureProvenance& texture(
        const MaterialProvenance& material, MaterialTextureSemantic semantic) {
        for (const MaterialTextureProvenance& candidate : material.textures) {
            if (candidate.semantic == semantic) return candidate;
        }
        throw std::runtime_error("missing texture semantic");
    }

    const MaterialExtensionProvenance& extension(
        const MaterialProvenance& material, std::string_view name) {
        for (const MaterialExtensionProvenance& candidate : material.extensions) {
            if (candidate.name == name) return candidate;
        }
        throw std::runtime_error("missing extension");
    }

    std::vector<MaterialProvenance> loadFixture() {
        return inspectGltfMaterialSources(std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets" / "material_provenance_fixture.gltf");
    }

    bool testFormatDefaultsRemainDistinguishable() {
        const auto materials = loadFixture();
        CHECK(materials.size() == 2);
        const MaterialProvenance& defaults = materials[0];
        CHECK(defaults.baseColor.origin == MaterialValueOrigin::FormatDefault);
        CHECK(defaults.baseColor.value == glm::vec4(1.0f));
        CHECK(defaults.metallic.origin == MaterialValueOrigin::FormatDefault);
        CHECK(near(defaults.metallic.value, 1.0f));
        CHECK(defaults.roughness.origin == MaterialValueOrigin::FormatDefault);
        CHECK(near(defaults.roughness.value, 1.0f));
        CHECK(defaults.alphaMode.value == "OPAQUE");
        CHECK(defaults.alphaCutoff.origin == MaterialValueOrigin::FormatDefault);
        CHECK(near(defaults.alphaCutoff.value, 0.5f));
        CHECK(!texture(defaults, MaterialTextureSemantic::BaseColor).present);

        const MaterialProvenance& explicitValues = materials[1];
        CHECK(explicitValues.metallic.origin == MaterialValueOrigin::Explicit);
        CHECK(near(explicitValues.metallic.value, 1.0f));
        CHECK(explicitValues.baseColor.origin == MaterialValueOrigin::Explicit);
        CHECK(near(explicitValues.baseColor.value.r, 0.8f));
        CHECK(explicitValues.alphaMode.origin == MaterialValueOrigin::Explicit);
        CHECK(explicitValues.alphaMode.value == "BLEND");
        CHECK(explicitValues.doubleSided.value);
        return true;
    }

    bool testTextureSemanticsAndSampler() {
        const auto materials = loadFixture();
        const MaterialProvenance& material = materials[1];
        const auto& base = texture(material, MaterialTextureSemantic::BaseColor);
        CHECK(base.present);
        CHECK(base.textureIndex == 0);
        CHECK(base.imageIndex == 0);
        CHECK(base.imageIdentity == "base.png");
        CHECK(base.channels == "RGBA");
        CHECK(base.colorInterpretation == TextureColorInterpretation::SRGB);
        CHECK(base.uvSet.value == 1);
        CHECK(base.uvSet.origin == MaterialValueOrigin::Explicit);
        CHECK(base.sampler.samplerObjectExplicit);
        CHECK(base.sampler.magFilter == 9729);
        CHECK(base.sampler.minFilter == 9987);
        CHECK(base.sampler.wrapS == 33071);
        CHECK(base.sampler.wrapT == 10497);
        CHECK(!base.sampler.wrapTExplicit);

        const auto& mr = texture(material,
            MaterialTextureSemantic::MetallicRoughness);
        CHECK(mr.channels == "G=roughness, B=metallic");
        CHECK(mr.colorInterpretation == TextureColorInterpretation::Linear);
        CHECK(mr.uvSet.value == 0);
        CHECK(mr.uvSet.origin == MaterialValueOrigin::FormatDefault);
        const auto& occlusion = texture(material, MaterialTextureSemantic::Occlusion);
        CHECK(occlusion.present);
        CHECK(occlusion.channels == "R");
        CHECK(occlusion.uvSet.value == 1);
        CHECK(!occlusion.consumedByRuntime);
        CHECK(texture(material, MaterialTextureSemantic::Emissive).
            colorInterpretation == TextureColorInterpretation::SRGB);
        CHECK(texture(material, MaterialTextureSemantic::Transmission).channels == "R");
        return true;
    }

    bool testExtensionDispositionAndWarnings() {
        const auto materials = loadFixture();
        const MaterialProvenance& material = materials[1];
        CHECK(extension(material, "KHR_materials_transmission").disposition ==
            MaterialExtensionDisposition::Applied);
        CHECK(extension(material, "KHR_materials_clearcoat").disposition ==
            MaterialExtensionDisposition::ParsedNotConsumed);
        CHECK(extension(material, "KHR_materials_specular").disposition ==
            MaterialExtensionDisposition::ParsedNotConsumed);
        CHECK(extension(material, "IRIDIUM_unknown_material_extension").disposition ==
            MaterialExtensionDisposition::UnsupportedIgnored);
        CHECK(material.warnings.size() >= 5);
        CHECK(near(material.transmission.value, 0.65f));
        return true;
    }

    bool testRuntimeAndGpuPackingSnapshot() {
        MaterialProvenance material = loadFixture()[1];
        MaterialAsset runtime{};
        runtime.baseColor = glm::vec4(0.8f, 0.2f, 0.1f, 0.75f);
        runtime.emissiveFactor = glm::vec4(2.0f, 1.0f, 0.5f, 0.0f);
        runtime.metallic = 1.0f;
        runtime.roughness = 0.35f;
        runtime.normalScale = 0.6f;
        runtime.alphaCutoff = 0.0f;
        runtime.transmissionFactor = 0.65f;
        runtime.albedoMap = TextureHandle{ 11 };
        runtime.normalMap = TextureHandle{ 12 };
        runtime.pbrMap = TextureHandle{ 13 };
        runtime.emissiveMap = TextureHandle{ 14 };
        runtime.transmissionMap = TextureHandle{ 15 };
        MaterialBinding binding{};
        binding.material = MaterialHandle{ 21 };
        binding.pipeline = PipelineHandle{ 22 };

        attachRuntimeMaterial(material, runtime, binding);
        CHECK(material.gpuBinding.material.id == 21);
        CHECK(material.gpuBinding.pipeline.id == 22);
        CHECK(near(material.pushConstantInputs.roughness, 0.35f));
        CHECK(near(material.pushConstantInputs.transmissionFactor, 0.65f));
        CHECK(texture(material, MaterialTextureSemantic::BaseColor).
            runtimeTexture.id == 11);
        CHECK(texture(material, MaterialTextureSemantic::Transmission).
            runtimeTexture.id == 15);
        const size_t warningsBeforeFallback = material.warnings.size();
        markRuntimeTextureFallbacks(material, {
            TextureHandle{ 11 }, TextureHandle{ 99 }, TextureHandle{ 98 },
            TextureHandle{ 97 }, TextureHandle{ 96 }
        });
        CHECK(texture(material, MaterialTextureSemantic::BaseColor).usedEngineFallback);
        CHECK(material.warnings.size() == warningsBeforeFallback + 1);
        return true;
    }

    bool testOptionalAlfaDiagnostic() {
        const std::filesystem::path path = std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "models" / "alfa_romeo" / "alfa_romeo.gltf";
        if (!std::filesystem::exists(path)) {
            std::cout << "  optional Alfa diagnostic skipped: local licensed asset absent\n";
            return true;
        }
        const auto materials = inspectGltfMaterialSources(path);
        CHECK(materials.size() > 36);
        for (const size_t paintIndex : { size_t{ 0 }, size_t{ 36 } }) {
            CHECK(materials[paintIndex].metallic.origin ==
                MaterialValueOrigin::FormatDefault);
            CHECK(near(materials[paintIndex].metallic.value, 1.0f));
            CHECK(extension(materials[paintIndex], "KHR_materials_clearcoat").disposition ==
                MaterialExtensionDisposition::ParsedNotConsumed);
        }
        CHECK(materials[6].alphaMode.value == "BLEND");
        CHECK(materials[6].metallic.origin == MaterialValueOrigin::FormatDefault);
        CHECK(near(materials[6].transmission.value, 0.0f));
        for (const size_t lensIndex : { size_t{ 3 }, size_t{ 4 } }) {
            CHECK(materials[lensIndex].alphaMode.value == "BLEND");
            CHECK(materials[lensIndex].transmission.origin ==
                MaterialValueOrigin::Explicit);
            CHECK(near(materials[lensIndex].transmission.value, 1.0f));
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[] = {
        { "glTF defaults versus explicit values", testFormatDefaultsRemainDistinguishable },
        { "texture semantics and sampler", testTextureSemanticsAndSampler },
        { "extension disposition and warnings", testExtensionDispositionAndWarnings },
        { "runtime and GPU packing snapshot", testRuntimeAndGpuPackingSnapshot },
        { "optional Alfa source diagnostic", testOptionalAlfaDiagnostic },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
