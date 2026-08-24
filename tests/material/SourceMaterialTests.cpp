#include "material/SourceMaterial.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;
    using Json = nlohmann::json;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; return false; } } while (false)

    std::filesystem::path tempPath(std::string_view suffix) {
        return std::filesystem::temp_directory_path() /
            ("iridium_source_material_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + std::string(suffix));
    }

    class TemporaryFile {
    public:
        explicit TemporaryFile(std::filesystem::path path) : m_path(std::move(path)) {}
        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;
        TemporaryFile(TemporaryFile&& other) noexcept : m_path(std::move(other.m_path)) {
            other.m_path.clear();
        }
        ~TemporaryFile() {
            if (!m_path.empty()) {
                std::error_code error;
                std::filesystem::remove(m_path, error);
            }
        }
        const std::filesystem::path& path() const { return m_path; }
    private:
        std::filesystem::path m_path;
    };

    TemporaryFile writeJson(const Json& root) {
        TemporaryFile file(tempPath(".gltf"));
        std::ofstream output(file.path(), std::ios::binary);
        output << root.dump();
        return file;
    }

    void appendU32(std::vector<char>& bytes, uint32_t value) {
        for (uint32_t shift : { 0u, 8u, 16u, 24u })
            bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
    }

    TemporaryFile writeGlb(Json root) {
        std::string payload = root.dump();
        while (payload.size() % 4 != 0) payload.push_back(' ');
        std::vector<char> bytes;
        appendU32(bytes, 0x46546C67u);
        appendU32(bytes, 2u);
        appendU32(bytes, static_cast<uint32_t>(20 + payload.size()));
        appendU32(bytes, static_cast<uint32_t>(payload.size()));
        appendU32(bytes, 0x4E4F534Au);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        TemporaryFile file(tempPath(".glb"));
        std::ofstream output(file.path(), std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file;
    }

    Json baseAsset() {
        return { { "asset", { { "version", "2.0" } } }, { "materials", Json::array() } };
    }

    bool testCoreDefaultsAndExplicitness() {
        const auto document = importGltfSourceMaterials(std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets" / "material_provenance_fixture.gltf");
        CHECK(document.materials().size() == 2);
        const SourceMaterial& defaults = document.materials()[0];
        CHECK(defaults.metallicRoughness.baseColorFactor.origin == SourceValueOrigin::FormatDefault);
        CHECK(defaults.metallicRoughness.metallicFactor.value == 1.0f);
        CHECK(defaults.alphaMode.value == SourceAlphaMode::Opaque);
        CHECK(defaults.transparencyPolicy == TransparencyPolicyV1{});
        CHECK(defaults.alphaCutoff.value == 0.5f);
        CHECK(!defaults.doubleSided.value);
        const SourceMaterial& explicitMaterial = document.materials()[1];
        CHECK(explicitMaterial.metallicRoughness.baseColorFactor.origin == SourceValueOrigin::Authored);
        CHECK(explicitMaterial.metallicRoughness.metallicFactor.origin == SourceValueOrigin::Authored);
        CHECK(explicitMaterial.alphaMode.value == SourceAlphaMode::Blend);
        CHECK(explicitMaterial.doubleSided.value);
        return true;
    }

    bool testTextureSemanticsSamplerAndTransform() {
        const auto document = importGltfSourceMaterials(std::filesystem::path(PROJECT_ROOT_DIR) /
            "tests" / "assets" / "material_provenance_fixture.gltf");
        const SourceMaterial& material = document.materials()[1];
        const SourceTextureUse* base = findSourceTexture(material, SourceTextureSemantic::BaseColor);
        CHECK(base != nullptr);
        CHECK(base->transfer == SourceTextureTransfer::Srgb);
        CHECK(base->channels == "RGBA");
        CHECK(base->texCoord.value == 1);
        CHECK(base->transform.offset.origin == SourceValueOrigin::Authored);
        CHECK(base->transform.offset.value == glm::vec2(0.25f, 0.5f));
        CHECK(base->sampler.magFilter.value == 9729);
        CHECK(base->sampler.minFilter.value == 9987);
        CHECK(base->sampler.wrapS.value == 33071);
        CHECK(base->sampler.wrapT.value == 10497);
        CHECK(base->sampler.wrapT.origin == SourceValueOrigin::FormatDefault);
        const SourceTextureUse* mr = findSourceTexture(material, SourceTextureSemantic::MetallicRoughness);
        CHECK(mr && mr->transfer == SourceTextureTransfer::Linear);
        CHECK(mr->channels == "G=roughness, B=metallic");
        return true;
    }

    bool testImageMayHaveDifferentTransferRoles() {
        Json root = baseAsset();
        root["images"] = Json::array({ { { "uri", "shared.png" } } });
        root["textures"] = Json::array({ { { "source", 0 } } });
        root["materials"].push_back({ { "pbrMetallicRoughness", {
            { "baseColorTexture", { { "index", 0 } } },
            { "metallicRoughnessTexture", { { "index", 0 } } } } } });
        const TemporaryFile file = writeJson(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(!document.hasErrors());
        const SourceTextureUse* color = findSourceTexture(document.materials()[0], SourceTextureSemantic::BaseColor);
        const SourceTextureUse* data = findSourceTexture(document.materials()[0], SourceTextureSemantic::MetallicRoughness);
        CHECK(color && data && color->imageIndex == data->imageIndex);
        CHECK(color->transfer == SourceTextureTransfer::Srgb);
        CHECK(data->transfer == SourceTextureTransfer::Linear);
        return true;
    }

    bool testSupportedExtensionInventory() {
        constexpr std::array names{
            "KHR_materials_emissive_strength", "KHR_materials_ior", "KHR_materials_specular",
            "KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_anisotropy",
            "KHR_materials_iridescence", "KHR_materials_transmission", "KHR_materials_volume",
            "KHR_materials_dispersion", "KHR_materials_diffuse_transmission", "KHR_materials_unlit",
            "KHR_materials_pbrSpecularGlossiness" };
        Json root = baseAsset();
        root["extensionsUsed"] = names;
        Json extensions = Json::object();
        for (const char* name : names) extensions[name] = Json::object();
        root["materials"].push_back({ { "extensions", extensions } });
        const TemporaryFile file = writeJson(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(!document.hasErrors());
        CHECK(document.materials()[0].extensions.size() == names.size());
        for (const char* name : names) {
            const SourceMaterialExtension* extension = findSourceExtension(document.materials()[0], name);
            CHECK(extension && extension->supportedByM2);
            for (const SourceExtensionProperty& property : extension->properties)
                CHECK(property.origin == SourceValueOrigin::FormatDefault);
        }
        CHECK(document.materials()[0].emissiveStrength.value == 1.0f);
        return true;
    }

    bool testRequiredAndInvalidInputsFailWithStableCodes() {
        Json root = baseAsset();
        root["extensionsUsed"] = { "VENDOR_required" };
        root["extensionsRequired"] = { "VENDOR_required" };
        root["materials"].push_back({ { "pbrMetallicRoughness", {
            { "baseColorTexture", { { "index", 4 } } } } } });
        const TemporaryFile file = writeJson(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(document.hasErrors());
        bool required = false;
        bool texture = false;
        for (const SourceMaterialDiagnostic& item : document.diagnostics()) {
            required |= item.code == "GLTF_REQUIRED_UNSUPPORTED";
            texture |= item.code == "GLTF_TEXTURE_INDEX";
        }
        CHECK(required);
        CHECK(texture);
        return true;
    }

    bool testGlbUsesSameMaterialPath() {
        Json root = baseAsset();
        root["materials"].push_back({ { "name", "GLB material" },
            { "pbrMetallicRoughness", { { "roughnessFactor", 0.25 } } } });
        const TemporaryFile file = writeGlb(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(!document.hasErrors());
        CHECK(document.materials().size() == 1);
        CHECK(document.materials()[0].name == "GLB material");
        CHECK(std::abs(document.materials()[0].metallicRoughness.roughnessFactor.value - 0.25f) < 0.0001f);
        CHECK(document.materials()[0].metallicRoughness.roughnessFactor.origin == SourceValueOrigin::Authored);
        return true;
    }

    bool testVariantsAndMissingUvDiagnostics() {
        Json root = baseAsset();
        root["extensionsUsed"] = { "KHR_materials_variants" };
        root["extensions"] = { { "KHR_materials_variants", {
            { "variants", Json::array({ { { "name", "Sport" } } }) } } } };
        root["images"] = Json::array({ { { "uri", "color.png" } } });
        root["textures"] = Json::array({ { { "source", 0 } } });
        root["materials"] = Json::array({
            { { "pbrMetallicRoughness", { { "baseColorTexture", {
                { "index", 0 }, { "texCoord", 1 } } } } } },
            Json::object()
        });
        root["meshes"] = Json::array({ { { "primitives", Json::array({ {
            { "attributes", { { "POSITION", 0 }, { "TEXCOORD_0", 1 } } },
            { "material", 0 },
            { "extensions", { { "KHR_materials_variants", {
                { "mappings", Json::array({ { { "material", 1 }, { "variants", { 0 } } } }) } } } } }
        } }) } } });
        const TemporaryFile file = writeJson(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(document.variants().size() == 1);
        CHECK(document.variants()[0].name == "Sport");
        CHECK(document.variantMappings().size() == 1);
        CHECK(document.variantMappings()[0].materialIndex == 1);
        bool missingUv = false;
        for (const SourceMaterialDiagnostic& item : document.diagnostics())
            missingUv |= item.code == "GLTF_TEXCOORD_MISSING";
        CHECK(missingUv);
        CHECK(document.hasErrors());
        return true;
    }

    bool testFactorRangeValidation() {
        Json root = baseAsset();
        root["materials"].push_back({ { "pbrMetallicRoughness", {
            { "metallicFactor", 1.1 }, { "roughnessFactor", -0.1 } } } });
        const TemporaryFile file = writeJson(root);
        const SourceMaterialDocument document = importGltfSourceMaterials(file.path());
        CHECK(document.hasErrors());
        size_t rangeErrors = 0;
        for (const SourceMaterialDiagnostic& item : document.diagnostics())
            rangeErrors += item.code == "GLTF_FACTOR_RANGE" ? 1 : 0;
        CHECK(rangeErrors == 2);
        return true;
    }

    bool testOptionalCarSnapshot() {
        const std::filesystem::path path = std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "models" / "alfa_romeo" / "alfa_romeo.gltf";
        if (!std::filesystem::exists(path)) {
            std::cout << "  optional Alfa snapshot skipped: licensed asset absent\n";
            return true;
        }
        const SourceMaterialDocument document = importGltfSourceMaterials(path);
        CHECK(!document.hasErrors());
        CHECK(document.materials().size() == 87);
        size_t defaultMetallic = 0;
        size_t explicitZeroMetallic = 0;
        size_t blend = 0;
        size_t normalTextures = 0;
        size_t metallicRoughnessTextures = 0;
        size_t occlusionTextures = 0;
        size_t emissiveTextures = 0;
        size_t nonzeroEmissive = 0;
        for (const SourceMaterial& material : document.materials()) {
            defaultMetallic += material.metallicRoughness.metallicFactor.origin ==
                SourceValueOrigin::FormatDefault ? 1 : 0;
            explicitZeroMetallic += material.metallicRoughness.metallicFactor.origin ==
                SourceValueOrigin::Authored && material.metallicRoughness.metallicFactor.value == 0.0f ? 1 : 0;
            blend += material.alphaMode.value == SourceAlphaMode::Blend ? 1 : 0;
            normalTextures += findSourceTexture(material, SourceTextureSemantic::Normal) ? 1 : 0;
            metallicRoughnessTextures += findSourceTexture(material,
                SourceTextureSemantic::MetallicRoughness) ? 1 : 0;
            occlusionTextures += findSourceTexture(material, SourceTextureSemantic::Occlusion) ? 1 : 0;
            emissiveTextures += findSourceTexture(material, SourceTextureSemantic::Emissive) ? 1 : 0;
            nonzeroEmissive += glm::any(glm::notEqual(material.emissiveFactor.value,
                glm::vec3(0.0f))) ? 1 : 0;
        }
        CHECK(defaultMetallic == 23);
        CHECK(explicitZeroMetallic == 63);
        CHECK(blend == 5);
        CHECK(normalTextures == 72);
        CHECK(metallicRoughnessTextures == 2);
        CHECK(occlusionTextures == 1);
        CHECK(emissiveTextures == 0);
        CHECK(nonzeroEmissive == 0);
        CHECK(document.materials()[0].metallicRoughness.metallicFactor.origin == SourceValueOrigin::FormatDefault);
        CHECK(findSourceExtension(document.materials()[0], "KHR_materials_clearcoat") != nullptr);
        CHECK(document.materials()[6].alphaMode.value == SourceAlphaMode::Blend);
        CHECK(findSourceExtension(document.materials()[3], "KHR_materials_transmission") != nullptr);
        CHECK(findSourceExtension(document.materials()[4], "KHR_materials_transmission") != nullptr);
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[]{
        { "core defaults and explicitness", testCoreDefaultsAndExplicitness },
        { "texture semantics, sampler, and transform", testTextureSemanticsSamplerAndTransform },
        { "one image in sRGB and linear roles", testImageMayHaveDifferentTransferRoles },
        { "supported extension inventory", testSupportedExtensionInventory },
        { "required extension and invalid index diagnostics", testRequiredAndInvalidInputsFailWithStableCodes },
        { "GLB material ingestion", testGlbUsesSameMaterialPath },
        { "variants and missing UV diagnostics", testVariantsAndMissingUvDiagnostics },
        { "factor range validation", testFactorRangeValidation },
        { "optional Alfa source snapshot", testOptionalCarSnapshot },
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
