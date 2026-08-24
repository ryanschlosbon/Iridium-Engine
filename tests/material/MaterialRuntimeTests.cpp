#include "material/MaterialRuntime.h"
#include "material/MaterialDiagnosticEvaluation.h"

#include "renderer/color/SceneColor.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

    using namespace Iridium;
    using Json = nlohmann::json;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; return false; } } while (false)

    bool near(float lhs, float rhs, float tolerance = 0.001f) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    SourceMaterialExtension extension(std::string name,
        std::initializer_list<SourceExtensionProperty> properties = {}) {
        SourceMaterialExtension result{};
        result.name = std::move(name);
        result.supportedByM2 = true;
        result.canonicalValues = "{}";
        result.properties.assign(properties.begin(), properties.end());
        return result;
    }

    SourceTextureUse texture(SourceTextureSemantic semantic, uint32_t sourceIndex) {
        SourceTextureUse result{};
        result.semantic = semantic;
        result.textureIndex = sourceIndex;
        result.imageIndex = sourceIndex;
        result.imageIdentity = "image-" + std::to_string(sourceIndex);
        result.channels = semantic == SourceTextureSemantic::BaseColor ? "rgba" : "rgb";
        result.transfer = semantic == SourceTextureSemantic::BaseColor
            ? SourceTextureTransfer::Srgb : SourceTextureTransfer::Linear;
        return result;
    }

    SourceMaterial standardSource(bool textured = false) {
        SourceMaterial source{};
        source.localIndex = 7;
        source.name = "runtime-test";
        source.metallicRoughness.baseColorFactor = {
            glm::vec4(0.8f, 0.4f, 0.2f, 1.0f), SourceValueOrigin::Authored };
        source.metallicRoughness.metallicFactor = { 0.25f, SourceValueOrigin::Authored };
        source.metallicRoughness.roughnessFactor = { 0.6f, SourceValueOrigin::Authored };
        if (textured) {
            source.textures.push_back(texture(SourceTextureSemantic::BaseColor, 0));
            source.textures.push_back(texture(SourceTextureSemantic::Normal, 1));
            source.textures.push_back(texture(SourceTextureSemantic::Occlusion, 2));
            source.textures[0].transform.offset.value = { 0.25f, -0.5f };
            source.textures[0].transform.scale.value = { 2.0f, 0.5f };
            source.textures[0].transform.rotation.value = 0.125f;
            source.textures[0].transform.texCoordOverride = 1;
            source.textures[0].sampler.sourceIndex = 4;
            source.textures[1].scalar.value = 0.75f;
            source.textures[2].scalar.value = 0.375f;
        }
        return source;
    }

    std::shared_ptr<const CompiledMaterial> compile(const SourceMaterial& source,
        MaterialCompileResult* retainedResult = nullptr) {
        MaterialCompileResult result = compileSourceMaterial(source);
        if (!result.succeeded()) throw std::runtime_error("test material did not compile");
        const auto material = result.material;
        if (retainedResult) *retainedResult = std::move(result);
        return material;
    }

    bool testImmutableCompiledAssetAndOverrides() {
        using Element = typename decltype(MaterialCompileResult{}.material)::element_type;
        static_assert(std::is_const_v<Element>);

        MaterialInstance instance(compile(standardSource()));
        const std::string compiledHash = instance.compiled().contentHash;
        CHECK(instance.revision() == 1);
        CHECK(instance.setMetallic(0.5f) == MaterialOverrideStatus::Applied);
        CHECK(instance.setMetallic(0.5f) == MaterialOverrideStatus::Unchanged);
        CHECK(instance.setRoughness(-0.1f) == MaterialOverrideStatus::InvalidValue);
        CHECK(instance.setBaseColor({ 0.2f, 0.3f, 0.4f, 1.0f }) == MaterialOverrideStatus::Applied);
        CHECK(instance.isOverridden(MaterialOverrideMetallic));
        CHECK(instance.isOverridden(MaterialOverrideBaseColor));
        CHECK(instance.setNormalScale(0.5f) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setOcclusionStrength(0.5f) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setAlphaCutoff(0.4f) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setAlphaMode(SourceAlphaMode::Mask) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setDoubleSided(true) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setTextureBinding(SourceTextureSemantic::BaseColor,
            MaterialTextureBinding{ TextureHandle::fromParts(1, 1),
                SamplerHandle::fromParts(1, 1) }) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setEmissive({ 1.0f, 0.0f, 0.0f }, 1.0f) ==
            MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.setDiffuseFactor(glm::vec4(0.5f)) ==
            MaterialOverrideStatus::RequiresRecompile);
        CHECK(instance.compiled().contentHash == compiledHash);

        SourceMaterial emissive = standardSource();
        emissive.emissiveFactor.value = { 1.0f, 0.25f, 0.0f };
        MaterialInstance emissiveInstance(compile(emissive));
        CHECK(emissiveInstance.setEmissive({ 0.5f, 0.5f, 0.0f }, 2.0f) ==
            MaterialOverrideStatus::Applied);
        CHECK(emissiveInstance.setEmissive(glm::vec3(0.0f), 0.0f) ==
            MaterialOverrideStatus::RequiresRecompile);

        SourceMaterial specGloss{};
        specGloss.extensions.push_back(extension("KHR_materials_pbrSpecularGlossiness", {
            { "diffuseFactor", "[0.5,0.4,0.3,1.0]", SourceValueOrigin::Authored },
            { "specularFactor", "[0.04,0.04,0.04]", SourceValueOrigin::Authored },
            { "glossinessFactor", "0.7", SourceValueOrigin::Authored } }));
        MaterialInstance specGlossInstance(compile(specGloss));
        CHECK(specGlossInstance.setMetallic(0.5f) == MaterialOverrideStatus::RequiresRecompile);
        CHECK(specGlossInstance.setDiffuseFactor({ 0.4f, 0.3f, 0.2f, 1.0f }) ==
            MaterialOverrideStatus::Applied);
        CHECK(specGlossInstance.setSpecularGlossiness({ 0.08f, 0.06f, 0.04f }, 0.5f) ==
            MaterialOverrideStatus::Applied);

        SourceMaterial masked = standardSource();
        masked.alphaMode.value = SourceAlphaMode::Mask;
        MaterialInstance maskedInstance(compile(masked));
        CHECK(maskedInstance.setAlphaCutoff(0.25f) == MaterialOverrideStatus::Applied);
        return true;
    }

    bool testPackedLayoutAndRoundTrip() {
        static_assert(offsetof(PackedGpuMaterial, baseColorFactor) == 32);
        static_assert(offsetof(PackedGpuMaterial, complexLobes) == 144);
        static_assert(offsetof(PackedGpuMaterial, textureUses) == 400);
        static_assert(offsetof(PackedGpuMaterial, textureIndices) == 736);
        static_assert(offsetof(PackedGpuMaterial, transparencyPolicy) == 820);
        static_assert(offsetof(PackedGpuMaterial, transparencyPriority) == 824);
        static_assert(offsetof(PackedGpuMaterial, thinSheetThicknessMeters) == 828);
        static_assert(offsetof(PackedGpuTextureUse, samplerIndex) == 12);

        SourceMaterial source = standardSource(true);
        source.transparencyPolicy = {
            .requestedClass = TransparencyClass::ThinGlass,
            .quality = TransparencyQuality::Hero4,
            .priority = -9,
            .thinSheetThicknessMeters = 0.03125f,
        };
        const auto compiled = compile(source);
        const std::array bindings{ MaterialTextureBinding{ TextureHandle::fromParts(3, 2),
                SamplerHandle::fromParts(4, 3) },
            MaterialTextureBinding{ TextureHandle::fromParts(5, 4),
                SamplerHandle::fromParts(4, 3), true },
            MaterialTextureBinding{ TextureHandle::fromParts(7, 6), SamplerHandle::fromParts(6, 5) } };
        MaterialInstance instance(compiled, bindings);
        CHECK(instance.setRoughness(0.35f) == MaterialOverrideStatus::Applied);
        const MaterialTextureBinding replacement{ TextureHandle::fromParts(8, 7),
            SamplerHandle::fromParts(4, 3) };
        CHECK(instance.setTextureBinding(SourceTextureSemantic::BaseColor, replacement) ==
            MaterialOverrideStatus::Applied);
        CHECK(instance.setTextureBinding(SourceTextureSemantic::BaseColor, replacement) ==
            MaterialOverrideStatus::Unchanged);
        std::array<uint32_t, 9> generations{};
        generations[3] = 2; generations[5] = 4; generations[7] = 6;
        generations[8] = 7;
        std::array<uint32_t, 7> samplerGenerations{};
        samplerGenerations[4] = 3; samplerGenerations[6] = 5;
        const MaterialPackResult first = packMaterialInstance(instance,
            { generations, samplerGenerations });
        const MaterialPackResult second = packMaterialInstance(instance,
            { generations, samplerGenerations });
        CHECK(first.succeeded());
        CHECK(second.succeeded());
        CHECK(std::memcmp(&*first.material, &*second.material, sizeof(PackedGpuMaterial)) == 0);
        CHECK(first.material->schemaVersion == PackedGpuMaterial::SchemaVersion);
        CHECK(first.material->featureFlags ==
            (compiled->featureFlags |
                MaterialFeaturePackedNormalReconstructZ));
        CHECK(first.material->textureMask == compiled->standard.textureMask);
        CHECK(first.material->textureIndices[0] == 8);
        CHECK(first.material->textureIndices[2] == 5);
        CHECK(first.material->textureIndices[3] == 7);
        CHECK(packedTextureReconstructNormalZMask(
            *first.material) == (1u << 2u));
        CHECK(first.material->transparencyPolicy ==
            packTransparencyPolicyWord(compiled->transparency));
        CHECK(first.material->transparencyPriority == -9);
        CHECK(first.material->thinSheetThicknessMeters == 0.03125f);
        CHECK(first.material->textureUses[0].samplerIndex == 4);
        CHECK(first.material->textureUses[0].texCoord == 1);

        const UnpackedGpuMaterial unpacked = unpackGpuMaterial(*first.material);
        CHECK(unpacked.values.baseColorFactor == instance.values().baseColorFactor);
        CHECK(unpacked.values.roughnessFactor == 0.35f);
        CHECK(unpacked.transparency == compiled->transparency);
        CHECK(near(unpacked.textureUses[0].offset.x, 0.25f));
        CHECK(near(unpacked.textureUses[0].offset.y, -0.5f));
        CHECK(near(unpacked.textureUses[0].scale.x, 2.0f));
        CHECK(near(unpacked.textureUses[0].rotation, 0.125f));
        CHECK(near(unpacked.textureUses[2].scalar, 0.75f));
        CHECK(near(unpacked.textureUses[3].scalar, 0.375f));
        return true;
    }

    bool testTransparencyClassesSurviveGpuPacking() {
        struct PolicyCase {
            TransparencyClass requestedClass;
            TransparencyTopology topology;
            SourceAlphaMode alphaMode;
            bool volume;
        };
        constexpr std::array cases{
            PolicyCase{ TransparencyClass::AlphaClip,
                TransparencyTopology::Unknown, SourceAlphaMode::Mask, false },
            PolicyCase{ TransparencyClass::SortedSurface,
                TransparencyTopology::Unknown, SourceAlphaMode::Blend, false },
            PolicyCase{ TransparencyClass::ThinGlass,
                TransparencyTopology::Unknown, SourceAlphaMode::Blend, false },
            PolicyCase{ TransparencyClass::LayeredGlass,
                TransparencyTopology::ValidClosed, SourceAlphaMode::Blend, true },
            PolicyCase{ TransparencyClass::WeightedOit,
                TransparencyTopology::Unknown, SourceAlphaMode::Blend, false },
        };

        int32_t priority = -17;
        for (const PolicyCase& policyCase : cases) {
            SourceMaterial source = standardSource();
            source.alphaMode.value = policyCase.alphaMode;
            source.transparencyPolicy = {
                .requestedClass = policyCase.requestedClass,
                .quality = TransparencyQuality::Cinematic8,
                .priority = priority++,
                .thinSheetThicknessMeters = 0.00625f,
            };
            if (policyCase.volume) {
                source.extensions.push_back(extension(
                    "KHR_materials_transmission", {
                        { "transmissionFactor", "1.0",
                            SourceValueOrigin::Authored },
                    }));
                source.extensions.push_back(extension(
                    "KHR_materials_volume", {
                        { "thicknessFactor", "0.1",
                            SourceValueOrigin::Authored },
                    }));
            }

            MaterialCompileResult compiled = compileSourceMaterial(source);
            CHECK(compiled.succeeded());
            if (policyCase.topology == TransparencyTopology::ValidClosed) {
                compiled = applyCompiledTransparencyPolicy(*compiled.material,
                    source.transparencyPolicy, policyCase.topology);
                CHECK(compiled.succeeded());
            }
            CHECK(compiled.material->transparency.resolvedClass ==
                policyCase.requestedClass);

            const MaterialInstance instance(compiled.material);
            const MaterialPackResult packed = packMaterialInstance(instance, {});
            CHECK(packed.succeeded());
            CHECK(packed.material->transparencyPolicy ==
                packTransparencyPolicyWord(compiled.material->transparency));
            CHECK(packed.material->transparencyPriority ==
                compiled.material->transparency.priority);
            CHECK(packed.material->thinSheetThicknessMeters ==
                compiled.material->transparency.thinSheetThicknessMeters);

            const UnpackedGpuMaterial unpacked =
                unpackGpuMaterial(*packed.material);
            CHECK(unpacked.transparency == compiled.material->transparency);
        }
        return true;
    }

    bool testHalfBoundariesAndPackingFailures() {
        CHECK(floatToHalf(0.0f) == 0x0000u);
        CHECK(floatToHalf(-0.0f) == 0x8000u);
        CHECK(floatToHalf(1.0f) == 0x3c00u);
        CHECK(floatToHalf(0.5f) == 0x3800u);
        CHECK(floatToHalf(65504.0f) == 0x7bffu);
        CHECK(floatToHalf(std::ldexp(1.0f, -14)) == 0x0400u);
        CHECK(floatToHalf(std::ldexp(1.0f, -24)) == 0x0001u);
        CHECK((floatToHalf(std::numeric_limits<float>::infinity()) & 0x7c00u) == 0x7c00u);
        CHECK((floatToHalf(std::numeric_limits<float>::quiet_NaN()) & 0x7c00u) == 0x7c00u);

        SourceMaterial source = standardSource(true);
        source.textures[0].transform.scale.value.x = 70000.0f;
        const auto compiled = compile(source);
        const std::array bindings{ MaterialTextureBinding{ TextureHandle::fromParts(1, 1),
                SamplerHandle::fromParts(1, 1) },
            MaterialTextureBinding{ TextureHandle::fromParts(2, 1), SamplerHandle::fromParts(1, 1) },
            MaterialTextureBinding{ TextureHandle::fromParts(3, 1), SamplerHandle::fromParts(1, 1) } };
        MaterialInstance instance(compiled, bindings);
        std::array<uint32_t, 4> generations{ 0, 1, 1, 1 };
        std::array<uint32_t, 2> samplerGenerations{ 0, 1 };
        const MaterialPackResult result = packMaterialInstance(instance,
            { generations, samplerGenerations });
        CHECK(!result.succeeded());
        CHECK(!result.diagnostics.empty());
        CHECK(result.diagnostics[0].error == MaterialPackError::HalfOverflow);

        generations[2] = 2;
        const MaterialPackResult stale = packMaterialInstance(instance,
            { generations, samplerGenerations });
        CHECK(!stale.succeeded());
        bool foundStale = false;
        for (const auto& diagnostic : stale.diagnostics)
            foundStale |= diagnostic.error == MaterialPackError::StaleTextureHandle;
        CHECK(foundStale);
        generations[2] = 1;
        samplerGenerations[1] = 2;
        const MaterialPackResult staleSampler = packMaterialInstance(instance,
            { generations, samplerGenerations });
        CHECK(!staleSampler.succeeded());
        bool foundStaleSampler = false;
        for (const auto& diagnostic : staleSampler.diagnostics)
            foundStaleSampler |= diagnostic.error == MaterialPackError::StaleSamplerHandle;
        CHECK(foundStaleSampler);
        return true;
    }

    bool testComplexLobePacking() {
        SourceMaterial source = standardSource();
        source.extensions.push_back(extension("KHR_materials_clearcoat", {
            { "clearcoatFactor", "0.8", SourceValueOrigin::Authored },
            { "clearcoatRoughnessFactor", "0.2", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_sheen", {
            { "sheenColorFactor", "[0.2,0.1,0.05]", SourceValueOrigin::Authored },
            { "sheenRoughnessFactor", "0.4", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_anisotropy", {
            { "anisotropyStrength", "0.7", SourceValueOrigin::Authored },
            { "anisotropyRotation", "0.3", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_iridescence", {
            { "iridescenceFactor", "0.8", SourceValueOrigin::Authored },
            { "iridescenceIor", "1.4", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_transmission", {
            { "transmissionFactor", "1.0", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_volume", {
            { "thicknessFactor", "0.1", SourceValueOrigin::Authored },
            { "attenuationDistance", "2.0", SourceValueOrigin::Authored },
            { "attenuationColor", "[0.9,0.8,0.7]", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_dispersion", {
            { "dispersion", "0.05", SourceValueOrigin::Authored } }));
        source.extensions.push_back(extension("KHR_materials_diffuse_transmission", {
            { "diffuseTransmissionFactor", "0.3", SourceValueOrigin::Authored },
            { "diffuseTransmissionColorFactor", "[1.0,0.5,0.25]", SourceValueOrigin::Authored } }));
        const auto compiled = compile(source);
        CHECK(compiled->closureClass == MaterialClosureClass::ComplexForward);
        MaterialInstance instance(compiled);
        const MaterialPackResult packed = packMaterialInstance(instance, {});
        CHECK(packed.succeeded());
        CHECK(packed.material->complexLobeCount == 8);
        CHECK(packed.material->complexLobes[0].type ==
            static_cast<uint32_t>(ComplexLobeType::Clearcoat));
        CHECK(near(packed.material->complexLobes[0].parameters[0], 0.8f));
        CHECK(near(packed.material->complexLobes[0].parameters[1], 0.2f));
        for (uint32_t index = 0; index < packed.material->complexLobeCount; ++index)
            CHECK(packed.material->complexLobes[index].type == index);
        CHECK(near(packed.material->complexLobes[2].parameters[0], 0.7f));
        CHECK(near(packed.material->complexLobes[3].parameters[1], 1.4f));
        CHECK(near(packed.material->complexLobes[4].parameters[0], 1.0f));
        CHECK(near(packed.material->complexLobes[5].parameters[1], 2.0f));
        CHECK(near(packed.material->complexLobes[6].parameters[0], 0.05f));
        CHECK(near(packed.material->complexLobes[7].parameters[0], 0.3f));
        return true;
    }

    bool testGenerationalStoreAndChangedOnlyUpload() {
        const auto compiled = compile(standardSource());
        MaterialInstanceStore store(2);
        const MaterialInstanceHandle first = store.create(compiled);
        const MaterialInstanceHandle second = store.create(compiled);
        CHECK(store.activeCount() == 2);
        std::vector<MaterialUpload> uploads;
        uploads.reserve(2);
        MaterialUploadStats stats = store.collectChanged({}, uploads);
        CHECK(stats.activeInstances == 2);
        CHECK(stats.changedInstances == 2);
        CHECK(stats.uploadedBytes == 2 * sizeof(PackedGpuMaterial));
        CHECK(uploads.size() == 2);
        stats = store.collectChanged({}, uploads);
        CHECK(stats.changedInstances == 0);
        CHECK(uploads.empty());
        CHECK(store.get(second)->setRoughness(0.2f) == MaterialOverrideStatus::Applied);
        stats = store.collectChanged({}, uploads);
        CHECK(stats.changedInstances == 1);
        CHECK(stats.uploadedBytes == sizeof(PackedGpuMaterial));
        CHECK(uploads[0].instance == second);

        const uint32_t oldIndex = first.getIndex();
        store.destroy(first);
        CHECK(store.get(first) == nullptr);
        const MaterialInstanceHandle replacement = store.create(compiled);
        CHECK(replacement.getIndex() == oldIndex);
        CHECK(replacement.getGeneration() != first.getGeneration());
        CHECK(store.get(first) == nullptr);
        CHECK(store.get(replacement) != nullptr);
        return true;
    }

    bool testDiagnosticSnapshot() {
        SourceMaterial source = standardSource(true);
        SourceMaterialExtension unsupported{};
        unsupported.name = "VENDOR_materials_diagnostic_test";
        unsupported.canonicalValues = "{\"value\":1}";
        unsupported.properties.push_back({ "value", "1", SourceValueOrigin::Authored });
        source.extensions.push_back(std::move(unsupported));
        MaterialCompileResult compiledResult = compileSourceMaterial(
            source, MaterialCompilePolicy::PermissiveEditor);
        CHECK(compiledResult.succeeded());
        const auto compiled = compiledResult.material;
        const std::array bindings{ MaterialTextureBinding{ TextureHandle::fromParts(3, 2),
                SamplerHandle::fromParts(4, 3) },
            MaterialTextureBinding{ TextureHandle::fromParts(5, 4), SamplerHandle::fromParts(4, 3) },
            MaterialTextureBinding{ TextureHandle::fromParts(7, 6), SamplerHandle::fromParts(6, 5) } };
        MaterialInstance instance(compiled, bindings);
        CHECK(instance.setMetallic(0.75f) == MaterialOverrideStatus::Applied);
        std::array<uint32_t, 8> generations{};
        generations[3] = 2; generations[5] = 4; generations[7] = 6;
        std::array<uint32_t, 7> samplerGenerations{};
        samplerGenerations[4] = 3; samplerGenerations[6] = 5;
        const MaterialPackResult packed = packMaterialInstance(instance,
            { generations, samplerGenerations });
        CHECK(packed.succeeded());
        const MaterialDiagnosticSnapshot first = buildMaterialDiagnosticSnapshot(
            source, compiledResult, instance, packed);
        const MaterialDiagnosticSnapshot second = buildMaterialDiagnosticSnapshot(
            source, compiledResult, instance, packed);
        CHECK(first.sha256 == second.sha256);
        CHECK(first.json == second.json);
        const Json document = Json::parse(first.json);
        CHECK(document.at("schema_version") == 2);
        CHECK(document.at("source").at("transparency_policy")
            .at("requested_class") == "auto");
        CHECK(document.at("source").at("base_color_origin") == "authored");
        CHECK(document.at("source").at("textures").size() == 3);
        CHECK(document.at("source").at("textures").at(0).contains("wrap_s"));
        CHECK(document.at("source").at("extensions").size() == 1);
        CHECK(document.at("compiled").at("hash") == compiled->contentHash);
        CHECK(document.at("compiled").at("closure") == "standard-deferred");
        CHECK(document.at("compiled").at("texture_operations").size() == 3);
        CHECK(document.at("compiled").at("transparency")
            .at("resolved_class") == "none");
        CHECK(document.at("instance").at("overrides").at(0) == "metallic");
        CHECK(document.at("instance").at("texture_bindings").at(0).at("sampler_index") == 4);
        CHECK(document.at("packed").at("byte_size") == sizeof(PackedGpuMaterial));
        CHECK(document.at("packed").at("texture_half_bits").size() == 3);
        CHECK(document.at("packed").contains("transparency_policy"));
        CHECK(!document.at("diagnostics").empty());
        CHECK(document.at("diagnostics").at(0).at("stage") == "compile");
        return true;
    }

    bool testInvalidBindingsAndSchema() {
        const auto compiled = compile(standardSource(true));
        bool threw = false;
        try { MaterialInstance invalid(compiled); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        PackedGpuMaterial packed{};
        packed.schemaVersion = 99;
        threw = false;
        try { (void)unpackGpuMaterial(packed); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        return true;
    }

    bool testPerFrameUploadRevisionRetirement() {
        uint64_t frame0 = 0;
        uint64_t frame1 = 0;
        CHECK(consumeMaterialUploadRevision(1, frame0));
        CHECK(frame0 == 1 && frame1 == 0);
        CHECK(!consumeMaterialUploadRevision(1, frame0));
        CHECK(consumeMaterialUploadRevision(1, frame1));
        CHECK(frame0 == 1 && frame1 == 1);
        CHECK(consumeMaterialUploadRevision(2, frame0));
        CHECK(frame0 == 2 && frame1 == 1);
        CHECK(!consumeMaterialUploadRevision(0, frame1));
        return true;
    }

    bool testDiagnosticUvEvaluation() {
        const glm::vec2 translated = evaluateMaterialTextureUv(
            { 0.25f, 0.5f }, { 0.1f, 0.2f }, { 2.0f, 0.5f }, 0.0f);
        CHECK(near(translated.x, 0.6f));
        CHECK(near(translated.y, 0.45f));
        const glm::vec2 rotated = evaluateMaterialTextureUv(
            { 1.0f, 0.0f }, { 0.0f, 0.0f }, { 2.0f, 1.0f },
            1.57079632679f);
        CHECK(near(rotated.x, 0.0f));
        CHECK(near(rotated.y, 2.0f));
        return true;
    }

} // namespace

int main() {
    const struct { const char* name; bool (*test)(); } tests[] = {
        { "immutable compiled asset and constrained overrides", testImmutableCompiledAssetAndOverrides },
        { "packed ABI and deterministic round trip", testPackedLayoutAndRoundTrip },
        { "five transparency classes survive GPU packing", testTransparencyClassesSurviveGpuPacking },
        { "FP16 boundaries and explicit packing failures", testHalfBoundariesAndPackingFailures },
        { "typed complex lobe packing", testComplexLobePacking },
        { "generational handles and changed-only upload", testGenerationalStoreAndChangedOnlyUpload },
        { "deterministic diagnostic snapshot", testDiagnosticSnapshot },
        { "invalid binding and schema rejection", testInvalidBindingsAndSchema },
        { "per-frame upload revision retirement", testPerFrameUploadRevisionRetirement },
        { "diagnostic UV evaluation", testDiagnosticUvEvaluation },
    };
    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name << '\n';
        try {
            if (!test.test()) return 1;
        }
        catch (const std::exception& exception) {
            std::cerr << "  unexpected exception: " << exception.what() << '\n';
            return 1;
        }
        std::cout << "[       OK ] " << test.name << '\n';
    }
    return 0;
}
