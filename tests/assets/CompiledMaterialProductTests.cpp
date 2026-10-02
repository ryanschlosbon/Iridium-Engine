#include "assets/material/CompiledMaterialProduct.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>

namespace {

    using namespace Iridium;
    using Json = nlohmann::json;

    #define CHECK(condition)                                                \
        do {                                                                \
            if (!(condition)) {                                             \
                std::cerr << __FILE__ << ':' << __LINE__                    \
                          << ": CHECK failed: " #condition << '\n';         \
                return false;                                               \
            }                                                               \
        } while (false)

    SourceMaterialExtension extension(
        std::string name, Json values) {
        SourceMaterialExtension result;
        result.name = std::move(name);
        result.supportedByM2 = true;
        result.canonicalValues = values.dump();
        for (auto iterator = values.begin();
            iterator != values.end(); ++iterator) {
            result.properties.push_back({
                .name = iterator.key(),
                .canonicalValue = iterator.value().dump(),
                .origin = SourceValueOrigin::Authored,
            });
        }
        return result;
    }

    std::shared_ptr<const CompiledMaterial> comprehensiveMaterial() {
        SourceMaterial source;
        source.localIndex = 84;
        source.name = "ID04_plastic_textured_001_rtint_colors_001_diff_6_79";
        source.metallicRoughness.baseColorFactor = {
            glm::vec4(0.1f, 0.2f, 0.3f, 0.9f),
            SourceValueOrigin::Authored,
        };
        source.metallicRoughness.metallicFactor = {
            0.9798125f, SourceValueOrigin::Authored,
        };
        source.metallicRoughness.roughnessFactor = {
            0.540954f, SourceValueOrigin::Authored,
        };
        source.emissiveFactor = {
            glm::vec3(0.01f, 0.02f, 0.03f),
            SourceValueOrigin::Authored,
        };
        source.emissiveStrength = {
            2.0f, SourceValueOrigin::Authored,
        };
        source.normalScale = {
            0.75f, SourceValueOrigin::Authored,
        };
        source.occlusionStrength = {
            0.6f, SourceValueOrigin::Authored,
        };
        source.alphaMode = {
            SourceAlphaMode::Opaque, SourceValueOrigin::FormatDefault,
        };
        source.alphaCutoff = {
            0.42f, SourceValueOrigin::Authored,
        };
        source.doubleSided = {
            true, SourceValueOrigin::Authored,
        };
        source.transparencyPolicy = {
            .requestedClass = TransparencyClass::ThinGlass,
            .quality = TransparencyQuality::Hero4,
            .priority = 17,
            .thinSheetThicknessMeters = 0.0125f,
        };

        SourceTextureUse texture;
        texture.semantic = SourceTextureSemantic::BaseColor;
        texture.textureIndex = 17;
        texture.imageIndex = 4;
        texture.imageIdentity = "images/4";
        texture.channels = "rgba";
        texture.transfer = SourceTextureTransfer::Srgb;
        texture.texCoord = { 1, SourceValueOrigin::Authored };
        texture.transform.offset = {
            glm::vec2(0.25f, 0.5f), SourceValueOrigin::Authored,
        };
        texture.transform.rotation = {
            0.125f, SourceValueOrigin::Authored,
        };
        texture.transform.scale = {
            glm::vec2(2.0f, 3.0f), SourceValueOrigin::Authored,
        };
        texture.transform.texCoordOverride = 2;
        texture.sampler.sourceIndex = 3;
        texture.sampler.magFilter = {
            9729, SourceValueOrigin::Authored,
        };
        texture.sampler.minFilter = {
            9987, SourceValueOrigin::Authored,
        };
        texture.sampler.wrapS = {
            33071, SourceValueOrigin::Authored,
        };
        texture.sampler.wrapT = {
            33648, SourceValueOrigin::Authored,
        };
        texture.scalar = {
            0.8f, SourceValueOrigin::Authored,
        };
        source.textures.push_back(texture);

        source.extensions = {
            extension("KHR_materials_ior", { { "ior", 1.6f } }),
            extension("KHR_materials_specular", {
                { "specularFactor", 0.7f },
                { "specularColorFactor", { 0.8f, 0.9f, 1.0f } },
            }),
            extension("KHR_materials_clearcoat", {
                { "clearcoatFactor", 1.0f },
                { "clearcoatRoughnessFactor", 0.2f },
            }),
            extension("KHR_materials_sheen", {
                { "sheenColorFactor", { 0.2f, 0.1f, 0.05f } },
                { "sheenRoughnessFactor", 0.4f },
            }),
            extension("KHR_materials_anisotropy", {
                { "anisotropyStrength", 0.7f },
                { "anisotropyRotation", 0.3f },
            }),
            extension("KHR_materials_iridescence", {
                { "iridescenceFactor", 0.8f },
                { "iridescenceIor", 1.4f },
                { "iridescenceThicknessMinimum", 120.0f },
                { "iridescenceThicknessMaximum", 360.0f },
            }),
            extension("KHR_materials_transmission", {
                { "transmissionFactor", 0.9f },
            }),
            extension("KHR_materials_volume", {
                { "thicknessFactor", 0.1f },
                { "attenuationDistance", 2.0f },
                { "attenuationColor", { 0.9f, 0.8f, 0.7f } },
            }),
            extension("KHR_materials_dispersion", {
                { "dispersion", 0.05f },
            }),
            extension("KHR_materials_diffuse_transmission", {
                { "diffuseTransmissionFactor", 0.3f },
                { "diffuseTransmissionColorFactor",
                    { 1.0f, 0.5f, 0.25f } },
            }),
        };

        const MaterialCompileResult result =
            compileSourceMaterial(source);
        if (!result.succeeded()) return {};
        return result.material;
    }

    bool testLosslessDeterministicRoundTrip() {
        const auto material = comprehensiveMaterial();
        CHECK(material);
        CHECK(material->complexLobes.size() == 8);
        CHECK(material->textureOperations.size() == 1);

        const std::vector<std::byte> first =
            serializeCompiledMaterial(*material);
        const std::vector<std::byte> second =
            serializeCompiledMaterial(*material);
        CHECK(first == second);

        const CompiledMaterialReadResult decoded =
            readCompiledMaterial(first);
        CHECK(decoded.valid());
        CHECK(decoded.material->sourceMaterialIndex == 84);
        CHECK(decoded.material->sourceName == material->sourceName);
        CHECK(decoded.material->contentHash == material->contentHash);
        CHECK(decoded.material->standard.baseColorFactor ==
            material->standard.baseColorFactor);
        CHECK(decoded.material->standard.doubleSided);
        CHECK(decoded.material->transparency.requestedClass ==
            TransparencyClass::ThinGlass);
        CHECK(decoded.material->transparency.resolvedClass ==
            TransparencyClass::ThinGlass);
        CHECK(decoded.material->transparency.quality ==
            TransparencyQuality::Hero4);
        CHECK(decoded.material->transparency.priority == 17);
        CHECK(decoded.material->transparency.thinSheetThicknessMeters ==
            0.0125f);
        CHECK(decoded.material->textureOperations.size() == 1);
        const auto& texture = decoded.material->textureOperations.front();
        CHECK(texture.semantic == SourceTextureSemantic::BaseColor);
        CHECK(texture.sourceTextureIndex == 17);
        CHECK(texture.sourceImageIndex == 4);
        CHECK(texture.imageIdentity == "images/4");
        CHECK(texture.transform.texCoordOverride == 2);
        CHECK(texture.sampler.sourceIndex == 3);
        CHECK(texture.sampler.magFilter.value == 9729);
        CHECK(texture.sampler.minFilter.value == 9987);
        CHECK(texture.sampler.wrapS.value == 33071);
        CHECK(texture.sampler.wrapT.value == 33648);
        CHECK(decoded.material->complexLobes.size() == 8);
        CHECK(serializeCompiledMaterial(*decoded.material) == first);
        return true;
    }

    bool testCorruptionAndVariantMismatchRejected() {
        const auto material = comprehensiveMaterial();
        CHECK(material);
        std::vector<std::byte> bytes =
            serializeCompiledMaterial(*material);
        bytes.back() ^= std::byte{ 0x01 };
        const CompiledMaterialReadResult corrupt =
            readCompiledMaterial(bytes);
        CHECK(!corrupt.valid());
        CHECK(corrupt.diagnostics.front().code ==
            "MATERIAL_PRODUCT_CHECKSUM");

        bytes.pop_back();
        CHECK(!readCompiledMaterial(bytes).valid());

        CompiledMaterial mismatch = *material;
        mismatch.complexLobes.front().data = SheenLobe{};
        mismatch.contentHash =
            calculateCompiledMaterialHash(mismatch);
        bool threw = false;
        try {
            (void)serializeCompiledMaterial(mismatch);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        return true;
    }

} // namespace

int main() {
    if (!testLosslessDeterministicRoundTrip()) return 1;
    if (!testCorruptionAndVariantMismatchRejected()) return 1;
    std::cout << "Compiled material product tests passed.\n";
    return 0;
}
