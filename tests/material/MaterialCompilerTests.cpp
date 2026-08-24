#include "material/MaterialCompiler.h"

#include "renderer/color/SceneColor.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <string_view>

namespace {

    using namespace Iridium;
    using Json = nlohmann::json;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; return false; } } while (false)

    bool near(float lhs, float rhs, float tolerance = 0.00001f) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    bool near(glm::vec3 lhs, glm::vec3 rhs, float tolerance = 0.00001f) {
        return near(lhs.x, rhs.x, tolerance) && near(lhs.y, rhs.y, tolerance) &&
            near(lhs.z, rhs.z, tolerance);
    }

    glm::vec3 ap1(glm::vec3 linearSrgb) {
        const Color::Rgb value = Color::linearSrgbToAcesCg({
            linearSrgb.r, linearSrgb.g, linearSrgb.b });
        return { static_cast<float>(value.r), static_cast<float>(value.g),
            static_cast<float>(value.b) };
    }

    SourceMaterialExtension extension(std::string name, Json values = Json::object(),
        bool supported = true, bool required = false) {
        SourceMaterialExtension result{};
        result.name = std::move(name);
        result.required = required;
        result.supportedByM2 = supported;
        result.canonicalValues = values.dump();
        for (auto iterator = values.begin(); iterator != values.end(); ++iterator)
            result.properties.push_back({ iterator.key(), iterator.value().dump(),
                SourceValueOrigin::Authored });
        return result;
    }

    void addTexture(SourceMaterial& material, SourceTextureSemantic semantic) {
        SourceTextureUse texture{};
        texture.semantic = semantic;
        material.textures.push_back(std::move(texture));
    }

    bool hasDiagnostic(const MaterialCompileResult& result, std::string_view code) {
        for (const MaterialCompileDiagnostic& diagnostic : result.diagnostics)
            if (diagnostic.code == code) return true;
        return false;
    }

    bool hasLobe(const CompiledMaterial& material, ComplexLobeType type) {
        for (const ComplexLobeRecord& lobe : material.complexLobes)
            if (lobe.type == type) return true;
        return false;
    }

    SourceMaterial dielectric(glm::vec3 base, float roughness = 0.5f) {
        SourceMaterial material{};
        material.metallicRoughness.baseColorFactor.value = glm::vec4(base, 1.0f);
        material.metallicRoughness.metallicFactor.value = 0.0f;
        material.metallicRoughness.roughnessFactor.value = roughness;
        return material;
    }

    bool testMetallicRoughnessReferenceEquation() {
        SourceMaterial source = dielectric(glm::vec3(0.8f, 0.4f, 0.2f), 0.6f);
        source.metallicRoughness.metallicFactor.value = 0.5f;
        addTexture(source, SourceTextureSemantic::BaseColor);
        addTexture(source, SourceTextureSemantic::MetallicRoughness);
        const MaterialCompileResult compiled = compileSourceMaterial(source);
        CHECK(compiled.succeeded());
        CHECK(compiled.material->closureClass == MaterialClosureClass::StandardDeferred);

        MaterialSurfaceInputs inputs{};
        inputs.textures.sample(SourceTextureSemantic::BaseColor) = { 0.5f, 0.25f, 1.0f, 0.8f };
        inputs.textures.sample(SourceTextureSemantic::MetallicRoughness) = { 1.0f, 0.5f, 0.25f, 1.0f };
        inputs.vertexColor = { 0.5f, 1.0f, 0.25f, 0.5f };
        const CanonicalMaterialSurface surface = evaluateMaterialSurface(*compiled.material, inputs);
        const glm::vec3 decoded{
            static_cast<float>(Color::decodeSrgb(0.5)),
            static_cast<float>(Color::decodeSrgb(0.25)), 1.0f };
        const glm::vec3 base = glm::vec3(0.8f, 0.4f, 0.2f) * decoded *
            glm::vec3(inputs.vertexColor);
        const float metallic = 0.5f * 0.25f;
        CHECK(near(surface.diffuseAlbedo, ap1(base * (1.0f - metallic))));
        CHECK(near(surface.f0, ap1(glm::mix(glm::vec3(0.04f), base, metallic))));
        CHECK(near(surface.f90, glm::mix(1.0f, 1.0f, metallic)));
        CHECK(near(surface.perceptualRoughness, 0.3f));
        CHECK(near(surface.alpha, 0.4f));
        return true;
    }

    bool testIorSpecularF0AndF90() {
        SourceMaterial source = dielectric(glm::vec3(0.7f));
        source.extensions.push_back(extension("KHR_materials_ior", { { "ior", 1.5f } }));
        source.extensions.push_back(extension("KHR_materials_specular", {
            { "specularFactor", 0.5f }, { "specularColorFactor", { 0.5f, 1.0f, 1.0f } } }));
        addTexture(source, SourceTextureSemantic::Specular);
        addTexture(source, SourceTextureSemantic::SpecularColor);
        const MaterialCompileResult result = compileSourceMaterial(source);
        CHECK(result.succeeded());
        MaterialSurfaceInputs inputs{};
        inputs.textures.sample(SourceTextureSemantic::Specular).a = 0.25f;
        inputs.textures.sample(SourceTextureSemantic::SpecularColor) = { 0.5f, 1.0f, 0.5f, 1.0f };
        const CanonicalMaterialSurface surface = evaluateMaterialSurface(*result.material, inputs);
        const float half = static_cast<float>(Color::decodeSrgb(0.5));
        CHECK(near(surface.f0, ap1(glm::vec3(0.04f) *
            glm::vec3(0.5f * half, 1.0f, half) * 0.125f)));
        CHECK(near(surface.f90, 0.125f));
        CHECK((result.material->featureFlags & MaterialFeatureSpecular) != 0);
        return true;
    }

    bool testSpecGlossAndMrEquivalence() {
        SourceMaterial mr = dielectric(glm::vec3(0.48f, 0.24f, 0.096f), 0.3f);
        const MaterialCompileResult mrResult = compileSourceMaterial(mr);
        CHECK(mrResult.succeeded());

        SourceMaterial sg{};
        sg.extensions.push_back(extension("KHR_materials_pbrSpecularGlossiness", {
            { "diffuseFactor", { 0.5f, 0.25f, 0.1f, 1.0f } },
            { "specularFactor", { 0.04f, 0.04f, 0.04f } },
            { "glossinessFactor", 0.7f } }));
        const MaterialCompileResult sgResult = compileSourceMaterial(sg);
        CHECK(sgResult.succeeded());
        CHECK(sgResult.material->workflow == MaterialWorkflow::SpecularGlossiness);
        CHECK(hasDiagnostic(sgResult, "MATERIAL_SPEC_GLOSS_PRECEDENCE"));
        const CanonicalMaterialSurface mrSurface = evaluateMaterialSurface(*mrResult.material);
        const CanonicalMaterialSurface sgSurface = evaluateMaterialSurface(*sgResult.material);
        CHECK(near(mrSurface.diffuseAlbedo, sgSurface.diffuseAlbedo));
        CHECK(near(mrSurface.f0, sgSurface.f0));
        CHECK(near(mrSurface.f90, sgSurface.f90));
        CHECK(near(mrSurface.perceptualRoughness, sgSurface.perceptualRoughness));
        SourceMaterial zeroRoughness = dielectric(glm::vec3(0.5f), 0.0f);
        const MaterialCompileResult zero = compileSourceMaterial(zeroRoughness);
        CHECK(zero.succeeded());
        CHECK(evaluateMaterialSurface(*zero.material).perceptualRoughness == 0.0f);
        return true;
    }

    bool testAlphaNormalAoAndEmissive() {
        SourceMaterial source = dielectric(glm::vec3(1.0f), 0.4f);
        source.metallicRoughness.baseColorFactor.value.a = 0.8f;
        source.alphaMode.value = SourceAlphaMode::Mask;
        source.alphaCutoff.value = 0.3f;
        source.normalScale.value = 0.5f;
        source.occlusionStrength.value = 0.5f;
        source.emissiveFactor.value = { 0.5f, 0.25f, 0.0f };
        source.emissiveStrength.value = 2.0f;
        addTexture(source, SourceTextureSemantic::BaseColor);
        addTexture(source, SourceTextureSemantic::Normal);
        addTexture(source, SourceTextureSemantic::Occlusion);
        addTexture(source, SourceTextureSemantic::Emissive);
        const MaterialCompileResult result = compileSourceMaterial(source);
        CHECK(result.succeeded());
        MaterialSurfaceInputs inputs{};
        inputs.textures.sample(SourceTextureSemantic::BaseColor) = glm::vec4(1.0f, 1.0f, 1.0f, 0.5f);
        inputs.textures.sample(SourceTextureSemantic::Normal) = glm::vec4(0.75f, 0.5f, 1.0f, 1.0f);
        inputs.textures.sample(SourceTextureSemantic::Occlusion).r = 0.2f;
        inputs.textures.sample(SourceTextureSemantic::Emissive) = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        inputs.vertexColor.a = 0.5f;
        const CanonicalMaterialSurface surface = evaluateMaterialSurface(*result.material, inputs);
        CHECK(near(surface.alpha, 0.2f));
        CHECK(!surface.alphaPasses);
        CHECK(near(surface.ao, 0.6f));
        CHECK(near(surface.shadingNormal, glm::normalize(glm::vec3(0.25f, 0.0f, 1.0f))));
        const float decoded = static_cast<float>(Color::decodeSrgb(0.5));
        CHECK(near(surface.emissive, ap1(glm::vec3(decoded, decoded * 0.5f, 0.0f))));
        return true;
    }

    bool testTwoSidedAndUnlitConvention() {
        SourceMaterial source = dielectric(glm::vec3(0.25f, 0.5f, 1.0f));
        source.doubleSided.value = true;
        source.extensions.push_back(extension("KHR_materials_unlit"));
        source.extensions.push_back(extension("KHR_materials_clearcoat", { { "clearcoatFactor", 1.0f } }));
        const MaterialCompileResult result = compileSourceMaterial(source);
        CHECK(result.succeeded());
        CHECK(result.material->closureClass == MaterialClosureClass::Unlit);
        CHECK((result.material->featureFlags & MaterialFeatureUnlit) != 0);
        CHECK(hasDiagnostic(result, "MATERIAL_UNLIT_INPUT_IGNORED"));
        MaterialSurfaceInputs inputs{};
        inputs.frontFacing = false;
        const CanonicalMaterialSurface surface = evaluateMaterialSurface(*result.material, inputs);
        CHECK(near(surface.shadingNormal, glm::vec3(0.0f, 0.0f, -1.0f)));
        CHECK(near(surface.unlitRadiance, ap1(glm::vec3(0.25f, 0.5f, 1.0f))));
        CHECK(near(surface.emissive, glm::vec3(0.0f)));
        return true;
    }

    bool testClassificationAndDormantLobes() {
        SourceMaterial dormant = dielectric(glm::vec3(0.5f));
        dormant.extensions.push_back(extension("KHR_materials_clearcoat", { { "clearcoatFactor", 0.0f } }));
        MaterialCompileResult result = compileSourceMaterial(dormant);
        CHECK(result.succeeded());
        CHECK(result.material->closureClass == MaterialClosureClass::StandardDeferred);
        CHECK(hasDiagnostic(result, "MATERIAL_DORMANT_EXTENSION"));
        CHECK((result.material->featureFlags & MaterialFeatureClearcoat) == 0);

        SourceMaterial complex = dielectric(glm::vec3(0.5f));
        complex.extensions.push_back(extension("KHR_materials_clearcoat", {
            { "clearcoatFactor", 1.0f }, { "clearcoatRoughnessFactor", 0.2f } }));
        complex.extensions.push_back(extension("KHR_materials_sheen", {
            { "sheenColorFactor", { 0.2f, 0.1f, 0.0f } }, { "sheenRoughnessFactor", 0.4f } }));
        complex.extensions.push_back(extension("KHR_materials_anisotropy", {
            { "anisotropyStrength", 0.7f }, { "anisotropyRotation", 0.3f } }));
        complex.extensions.push_back(extension("KHR_materials_iridescence", {
            { "iridescenceFactor", 0.8f }, { "iridescenceIor", 1.4f } }));
        complex.extensions.push_back(extension("KHR_materials_transmission", { { "transmissionFactor", 1.0f } }));
        complex.extensions.push_back(extension("KHR_materials_volume", {
            { "thicknessFactor", 0.1f }, { "attenuationDistance", 2.0f },
            { "attenuationColor", { 0.9f, 0.8f, 0.7f } } }));
        complex.extensions.push_back(extension("KHR_materials_dispersion", { { "dispersion", 0.05f } }));
        complex.extensions.push_back(extension("KHR_materials_diffuse_transmission", {
            { "diffuseTransmissionFactor", 0.3f },
            { "diffuseTransmissionColorFactor", { 1.0f, 0.5f, 0.25f } } }));
        result = compileSourceMaterial(complex);
        CHECK(result.succeeded());
        CHECK(result.material->closureClass == MaterialClosureClass::ComplexForward);
        for (const ComplexLobeType type : { ComplexLobeType::Clearcoat, ComplexLobeType::Sheen,
            ComplexLobeType::Anisotropy, ComplexLobeType::Iridescence,
            ComplexLobeType::ThinTransmission, ComplexLobeType::VolumeTransmission,
            ComplexLobeType::Dispersion, ComplexLobeType::DiffuseTransmission }) CHECK(hasLobe(*result.material, type));
        CHECK((result.material->featureFlags & MaterialFeatureClearcoat) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureSheen) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureAnisotropy) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureIridescence) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureTransmission) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureVolume) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureDispersion) != 0);
        CHECK((result.material->featureFlags & MaterialFeatureDiffuseTransmission) != 0);
        CHECK(hasDiagnostic(result, "MATERIAL_IRIDESCENCE_M2_APPROXIMATION"));
        CHECK(hasDiagnostic(result, "MATERIAL_DISPERSION_TRANSPORT_DEFERRED_M6"));
        const ClearcoatLobe& coat = std::get<ClearcoatLobe>(result.material->complexLobes[0].data);
        CHECK(near(coat.factor, 1.0f));
        CHECK(near(coat.roughnessFactor, 0.2f));
        CHECK(hasDiagnostic(result, "MATERIAL_TRANSPORT_DEFERRED_M6"));

        SourceMaterial blend = dielectric(glm::vec3(0.5f));
        blend.alphaMode.value = SourceAlphaMode::Blend;
        result = compileSourceMaterial(blend);
        CHECK(result.succeeded());
        CHECK(result.material->closureClass == MaterialClosureClass::StandardForward);
        return true;
    }

    bool testConflictAndUnsupportedPolicy() {
        SourceMaterial volume = dielectric(glm::vec3(0.5f));
        volume.extensions.push_back(extension("KHR_materials_volume", { { "thicknessFactor", 1.0f } }));
        MaterialCompileResult result = compileSourceMaterial(volume);
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, "MATERIAL_VOLUME_WITHOUT_TRANSMISSION"));

        SourceMaterial unknown = dielectric(glm::vec3(0.5f));
        unknown.extensions.push_back(extension("VENDOR_unknown", { { "value", 1 } }, false));
        result = compileSourceMaterial(unknown, MaterialCompilePolicy::Strict);
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, "MATERIAL_OPTIONAL_EXTENSION_STRICT"));
        result = compileSourceMaterial(unknown, MaterialCompilePolicy::PermissiveEditor);
        CHECK(result.succeeded());
        CHECK(hasDiagnostic(result, "MATERIAL_OPTIONAL_EXTENSION_FALLBACK"));
        CHECK((result.material->featureFlags & MaterialFeaturePermissiveFallback) != 0);

        SourceMaterial conflicting = dielectric(glm::vec3(0.5f));
        conflicting.extensions.push_back(extension("KHR_materials_pbrSpecularGlossiness"));
        conflicting.extensions.push_back(extension("KHR_materials_ior", { { "ior", 1.4f } }));
        result = compileSourceMaterial(conflicting);
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, "MATERIAL_SPEC_GLOSS_EXTENSION_CONFLICT"));

        SourceMaterial invalidIridescence = dielectric(glm::vec3(0.5f));
        invalidIridescence.extensions.push_back(extension("KHR_materials_iridescence", {
            { "iridescenceFactor", 1.0f }, { "iridescenceThicknessMinimum", 500.0f },
            { "iridescenceThicknessMaximum", 100.0f } }));
        result = compileSourceMaterial(invalidIridescence);
        CHECK(!result.succeeded());
        CHECK(hasDiagnostic(result, "MATERIAL_COMPLEX_PARAMETER_INVALID"));
        return true;
    }

    bool testInvalidAndDeterministicHash() {
        SourceMaterial invalid = dielectric(glm::vec3(0.5f));
        invalid.metallicRoughness.roughnessFactor.value = std::numeric_limits<float>::quiet_NaN();
        CHECK(!compileSourceMaterial(invalid).succeeded());
        SourceMaterial invalidLobe = dielectric(glm::vec3(0.5f));
        invalidLobe.extensions.push_back(extension("KHR_materials_clearcoat", {
            { "clearcoatFactor", -0.5f } }));
        CHECK(!compileSourceMaterial(invalidLobe).succeeded());

        SourceMaterial first = dielectric(glm::vec3(0.2f, 0.4f, 0.8f));
        addTexture(first, SourceTextureSemantic::BaseColor);
        first.textures[0].texCoord = { 1u, SourceValueOrigin::Authored };
        first.textures[0].sampler.wrapS = { 33071, SourceValueOrigin::Authored };
        first.localIndex = 3;
        first.name = "first";
        SourceMaterial second = first;
        second.localIndex = 99;
        second.name = "renamed";
        const MaterialCompileResult a = compileSourceMaterial(first);
        const MaterialCompileResult b = compileSourceMaterial(second);
        CHECK(a.succeeded() && b.succeeded());
        CHECK(a.material->contentHash == b.material->contentHash);
        CHECK(a.material->textureOperations.size() == 1);
        CHECK(a.material->textureOperations[0].texCoord == 1);
        CHECK(a.material->textureOperations[0].sampler.wrapS.value == 33071);
        second.textures[0].transform.offset = { glm::vec2(0.25f, 0.0f),
            SourceValueOrigin::Authored };
        const MaterialCompileResult transformed = compileSourceMaterial(second);
        CHECK(transformed.succeeded());
        CHECK(transformed.material->contentHash != a.material->contentHash);
        second.metallicRoughness.roughnessFactor.value = 0.75f;
        const MaterialCompileResult changed = compileSourceMaterial(second);
        CHECK(changed.succeeded());
        CHECK(changed.material->contentHash != a.material->contentHash);
        return true;
    }

    bool testVersionedTransparencyPolicyResolution() {
        SourceMaterial masked = dielectric(glm::vec3(0.5f));
        masked.alphaMode.value = SourceAlphaMode::Mask;
        MaterialCompileResult alphaClip = compileSourceMaterial(masked);
        CHECK(alphaClip.succeeded());
        CHECK(alphaClip.material->schemaVersion == 2);
        CHECK(alphaClip.material->transparency.resolvedClass ==
            TransparencyClass::AlphaClip);

        SourceMaterial blended = dielectric(glm::vec3(0.5f));
        blended.alphaMode.value = SourceAlphaMode::Blend;
        MaterialCompileResult sorted = compileSourceMaterial(blended);
        CHECK(sorted.succeeded());
        CHECK(sorted.material->transparency.resolvedClass ==
            TransparencyClass::SortedSurface);

        SourceMaterial thin = dielectric(glm::vec3(0.5f));
        thin.extensions.push_back(extension(
            "KHR_materials_transmission", {
                { "transmissionFactor", 1.0f },
            }));
        thin.transparencyPolicy = {
            .requestedClass = TransparencyClass::ThinGlass,
            .quality = TransparencyQuality::Hero4,
            .priority = 23,
            .thinSheetThicknessMeters = 0.0075f,
        };
        MaterialCompileResult thinResult = compileSourceMaterial(thin);
        CHECK(thinResult.succeeded());
        CHECK(thinResult.material->transparency.resolvedClass ==
            TransparencyClass::ThinGlass);
        CHECK(thinResult.material->transparency.quality ==
            TransparencyQuality::Hero4);
        CHECK(thinResult.material->transparency.priority == 23);
        CHECK(near(thinResult.material->transparency
            .thinSheetThicknessMeters, 0.0075f));

        SourceMaterial volume = thin;
        volume.transparencyPolicy.requestedClass =
            TransparencyClass::LayeredGlass;
        volume.extensions.push_back(extension(
            "KHR_materials_volume", {
                { "thicknessFactor", 0.1f },
                { "attenuationDistance", 2.0f },
                { "attenuationColor", { 0.9f, 0.8f, 0.7f } },
            }));
        MaterialCompileResult unresolvedLayered =
            compileSourceMaterial(volume);
        CHECK(unresolvedLayered.succeeded());
        CHECK(unresolvedLayered.material->transparency.resolvedClass ==
            TransparencyClass::ThinGlass);
        CHECK((unresolvedLayered.material->transparency.flags &
            CompiledTransparencyTopologyRequired) != 0);
        CHECK((unresolvedLayered.material->transparency.flags &
            CompiledTransparencyFallbackApplied) != 0);
        MaterialCompileResult layered = applyCompiledTransparencyPolicy(
            *unresolvedLayered.material, volume.transparencyPolicy,
            TransparencyTopology::ValidClosed);
        CHECK(layered.succeeded());
        CHECK(layered.material->transparency.resolvedClass ==
            TransparencyClass::LayeredGlass);
        CHECK((layered.material->transparency.flags &
            CompiledTransparencyFallbackApplied) == 0);

        SourceMaterial oit = blended;
        oit.transparencyPolicy.requestedClass =
            TransparencyClass::WeightedOit;
        MaterialCompileResult oitResult = compileSourceMaterial(oit);
        CHECK(oitResult.succeeded());
        CHECK(oitResult.material->transparency.resolvedClass ==
            TransparencyClass::WeightedOit);

        TransparencyPolicyV1 incompatibleOit =
            volume.transparencyPolicy;
        incompatibleOit.requestedClass =
            TransparencyClass::WeightedOit;
        MaterialCompileResult oitFallback =
            applyCompiledTransparencyPolicy(
                *unresolvedLayered.material, incompatibleOit);
        CHECK(oitFallback.succeeded());
        CHECK(oitFallback.material->transparency.resolvedClass ==
            TransparencyClass::SortedSurface);
        CHECK(hasDiagnostic(oitFallback,
            "MATERIAL_TRANSPARENCY_OIT_INCOMPATIBLE"));

        SourceMaterial invalid = blended;
        invalid.transparencyPolicy.thinSheetThicknessMeters =
            std::numeric_limits<float>::quiet_NaN();
        MaterialCompileResult sanitized =
            compileSourceMaterial(invalid);
        CHECK(sanitized.succeeded());
        CHECK(sanitized.material->transparency
            .thinSheetThicknessMeters == 0.0f);
        CHECK((sanitized.material->transparency.flags &
            CompiledTransparencyPolicySanitized) != 0);
        CHECK(hasDiagnostic(sanitized,
            "MATERIAL_TRANSPARENCY_THICKNESS_INVALID"));

        CHECK(alphaClip.material->contentHash !=
            sorted.material->contentHash);
        CHECK(thinResult.material->contentHash !=
            unresolvedLayered.material->contentHash);
        return true;
    }

    bool testOptionalCarClassificationSnapshot() {
        const std::filesystem::path path = std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "models" / "alfa_romeo" / "scene.gltf";
        if (!std::filesystem::exists(path)) {
            std::cout << "  optional Alfa classification skipped: licensed asset absent\n";
            return true;
        }
        const SourceMaterialDocument source = importGltfSourceMaterials(path);
        const MaterialCompileDocumentResult compiled = compileSourceMaterialDocument(source);
        CHECK(compiled.succeeded());
        CHECK(compiled.materials.size() == 87);
        CHECK(compiled.materials[0].material->closureClass == MaterialClosureClass::ComplexForward);
        CHECK(compiled.materials[36].material->closureClass == MaterialClosureClass::ComplexForward);
        CHECK(compiled.materials[6].material->closureClass == MaterialClosureClass::StandardForward);
        CHECK(compiled.materials[3].material->closureClass == MaterialClosureClass::ComplexForward);
        CHECK(compiled.materials[4].material->closureClass == MaterialClosureClass::ComplexForward);
        CHECK(hasLobe(*compiled.materials[0].material, ComplexLobeType::Clearcoat));
        CHECK(hasLobe(*compiled.materials[3].material, ComplexLobeType::ThinTransmission));
        for (const size_t lensIndex : { size_t{ 3 }, size_t{ 4 } }) {
            CHECK(std::ranges::any_of(source.materials()[lensIndex].textures,
                [](const SourceTextureUse& use) {
                    return use.semantic == SourceTextureSemantic::Normal;
                }));
            CHECK(std::ranges::any_of(compiled.materials[lensIndex].material->textureOperations,
                [](const CompiledTextureOperation& operation) {
                    return operation.semantic == SourceTextureSemantic::Normal;
                }));
        }
        for (const MaterialCompileResult& material : compiled.materials)
            CHECK(material.material->standard.emissiveFactor == glm::vec3(0.0f));
        return true;
    }

    bool testTrackedFixtureClassification() {
        struct Fixture {
            const char* path;
            size_t standardDeferred;
            size_t complexForward;
        };
        constexpr Fixture fixtures[]{
            { "assets/benchmarks/m0/material_lab.gltf", 3, 2 },
            { "assets/benchmarks/m0/opaque_emissive_range.gltf", 6, 0 },
            { "assets/benchmarks/m1/color_volume_transparency.gltf", 12, 1 },
            { "assets/benchmarks/m2/complex_closure_lab.gltf", 2, 6 },
        };
        for (const Fixture& fixture : fixtures) {
            const SourceMaterialDocument source = importGltfSourceMaterials(
                std::filesystem::path(PROJECT_ROOT_DIR) / fixture.path);
            const MaterialCompileDocumentResult compiled = compileSourceMaterialDocument(source);
            CHECK(compiled.succeeded());
            size_t standardDeferred = 0;
            size_t complexForward = 0;
            for (const MaterialCompileResult& result : compiled.materials) {
                standardDeferred += result.material->closureClass ==
                    MaterialClosureClass::StandardDeferred ? 1 : 0;
                complexForward += result.material->closureClass ==
                    MaterialClosureClass::ComplexForward ? 1 : 0;
            }
            CHECK(standardDeferred == fixture.standardDeferred);
            CHECK(complexForward == fixture.complexForward);
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[]{
        { "metallic/roughness reference equation", testMetallicRoughnessReferenceEquation },
        { "IOR and specular F0/F90", testIorSpecularF0AndF90 },
        { "specular/glossiness and MR equivalence", testSpecGlossAndMrEquivalence },
        { "alpha, normal, AO, and emissive", testAlphaNormalAoAndEmissive },
        { "two-sided and unlit convention", testTwoSidedAndUnlitConvention },
        { "classification and dormant lobes", testClassificationAndDormantLobes },
        { "conflict and unsupported policy", testConflictAndUnsupportedPolicy },
        { "invalid input and deterministic hash", testInvalidAndDeterministicHash },
        { "versioned transparency policy resolution", testVersionedTransparencyPolicyResolution },
        { "tracked fixture classification", testTrackedFixtureClassification },
        { "optional Alfa classification snapshot", testOptionalCarClassificationSnapshot },
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
    std::cout << std::size(tests) - failures << '/' << std::size(tests) << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
