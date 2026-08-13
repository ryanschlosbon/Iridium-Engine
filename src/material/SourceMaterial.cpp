#include "material/SourceMaterial.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Iridium {

    namespace {

        using Json = nlohmann::json;

        constexpr uint32_t GlbMagic = 0x46546C67u;
        constexpr uint32_t GlbVersion = 2u;
        constexpr uint32_t GlbJsonChunk = 0x4E4F534Au;

        struct TextureSlot {
            const char* key;
            SourceTextureSemantic semantic;
            const char* channels;
            SourceTextureTransfer transfer;
            const char* scalarKey;
            float scalarDefault;
        };

        constexpr std::array corePbrTextures{
            TextureSlot{ "baseColorTexture", SourceTextureSemantic::BaseColor,
                "RGBA", SourceTextureTransfer::Srgb, nullptr, 1.0f },
            TextureSlot{ "metallicRoughnessTexture", SourceTextureSemantic::MetallicRoughness,
                "G=roughness, B=metallic", SourceTextureTransfer::Linear, nullptr, 1.0f },
        };

        constexpr std::array coreMaterialTextures{
            TextureSlot{ "normalTexture", SourceTextureSemantic::Normal,
                "RGB", SourceTextureTransfer::Linear, "scale", 1.0f },
            TextureSlot{ "occlusionTexture", SourceTextureSemantic::Occlusion,
                "R", SourceTextureTransfer::Linear, "strength", 1.0f },
            TextureSlot{ "emissiveTexture", SourceTextureSemantic::Emissive,
                "RGB", SourceTextureTransfer::Srgb, nullptr, 1.0f },
        };

        struct ExtensionSpec {
            std::string name;
            std::vector<std::pair<std::string, std::string>> defaults;
            std::vector<TextureSlot> textures;
        };

        const std::vector<ExtensionSpec>& extensionSpecs() {
            static const std::vector<ExtensionSpec> specs{
                { "KHR_materials_emissive_strength", { { "emissiveStrength", "1.0" } }, {} },
                { "KHR_materials_ior", { { "ior", "1.5" } }, {} },
                { "KHR_materials_specular", {
                    { "specularFactor", "1.0" }, { "specularColorFactor", "[1.0,1.0,1.0]" } }, {
                    { "specularTexture", SourceTextureSemantic::Specular, "A", SourceTextureTransfer::Linear, nullptr, 1.0f },
                    { "specularColorTexture", SourceTextureSemantic::SpecularColor, "RGB", SourceTextureTransfer::Srgb, nullptr, 1.0f } } },
                { "KHR_materials_clearcoat", {
                    { "clearcoatFactor", "0.0" }, { "clearcoatRoughnessFactor", "0.0" } }, {
                    { "clearcoatTexture", SourceTextureSemantic::Clearcoat, "R", SourceTextureTransfer::Linear, nullptr, 1.0f },
                    { "clearcoatRoughnessTexture", SourceTextureSemantic::ClearcoatRoughness, "G", SourceTextureTransfer::Linear, nullptr, 1.0f },
                    { "clearcoatNormalTexture", SourceTextureSemantic::ClearcoatNormal, "RGB", SourceTextureTransfer::Linear, "scale", 1.0f } } },
                { "KHR_materials_sheen", {
                    { "sheenColorFactor", "[0.0,0.0,0.0]" }, { "sheenRoughnessFactor", "0.0" } }, {
                    { "sheenColorTexture", SourceTextureSemantic::SheenColor, "RGB", SourceTextureTransfer::Srgb, nullptr, 1.0f },
                    { "sheenRoughnessTexture", SourceTextureSemantic::SheenRoughness, "A", SourceTextureTransfer::Linear, nullptr, 1.0f } } },
                { "KHR_materials_anisotropy", {
                    { "anisotropyStrength", "0.0" }, { "anisotropyRotation", "0.0" } }, {
                    { "anisotropyTexture", SourceTextureSemantic::Anisotropy, "RG=direction, B=strength", SourceTextureTransfer::Linear, nullptr, 1.0f } } },
                { "KHR_materials_iridescence", {
                    { "iridescenceFactor", "0.0" }, { "iridescenceIor", "1.3" },
                    { "iridescenceThicknessMinimum", "100.0" }, { "iridescenceThicknessMaximum", "400.0" } }, {
                    { "iridescenceTexture", SourceTextureSemantic::Iridescence, "R", SourceTextureTransfer::Linear, nullptr, 1.0f },
                    { "iridescenceThicknessTexture", SourceTextureSemantic::IridescenceThickness, "G", SourceTextureTransfer::Linear, nullptr, 1.0f } } },
                { "KHR_materials_transmission", { { "transmissionFactor", "0.0" } }, {
                    { "transmissionTexture", SourceTextureSemantic::Transmission, "R", SourceTextureTransfer::Linear, nullptr, 1.0f } } },
                { "KHR_materials_volume", {
                    { "thicknessFactor", "0.0" }, { "attenuationDistance", "infinity" },
                    { "attenuationColor", "[1.0,1.0,1.0]" } }, {
                    { "thicknessTexture", SourceTextureSemantic::Thickness, "G", SourceTextureTransfer::Linear, nullptr, 1.0f } } },
                { "KHR_materials_dispersion", { { "dispersion", "0.0" } }, {} },
                { "KHR_materials_diffuse_transmission", {
                    { "diffuseTransmissionFactor", "0.0" },
                    { "diffuseTransmissionColorFactor", "[1.0,1.0,1.0]" } }, {
                    { "diffuseTransmissionTexture", SourceTextureSemantic::DiffuseTransmission, "A", SourceTextureTransfer::Linear, nullptr, 1.0f },
                    { "diffuseTransmissionColorTexture", SourceTextureSemantic::DiffuseTransmissionColor, "RGB", SourceTextureTransfer::Srgb, nullptr, 1.0f } } },
                { "KHR_materials_unlit", {}, {} },
                { "KHR_materials_pbrSpecularGlossiness", {
                    { "diffuseFactor", "[1.0,1.0,1.0,1.0]" },
                    { "specularFactor", "[1.0,1.0,1.0]" }, { "glossinessFactor", "1.0" } }, {
                    { "diffuseTexture", SourceTextureSemantic::Diffuse, "RGBA", SourceTextureTransfer::Srgb, nullptr, 1.0f },
                    { "specularGlossinessTexture", SourceTextureSemantic::SpecularGlossiness, "RGB=specular, A=glossiness", SourceTextureTransfer::Srgb, nullptr, 1.0f } } },
            };
            return specs;
        }

        const ExtensionSpec* findExtensionSpec(std::string_view name) {
            const auto& specs = extensionSpecs();
            const auto found = std::find_if(specs.begin(), specs.end(),
                [name](const ExtensionSpec& spec) { return name == spec.name; });
            return found == specs.end() ? nullptr : &*found;
        }

        void diagnostic(std::vector<SourceMaterialDiagnostic>& diagnostics,
            SourceDiagnosticSeverity severity, std::string code, std::string path,
            std::string message) {
            diagnostics.push_back({ severity, std::move(code), std::move(path),
                std::move(message) });
        }

        uint32_t readU32(const std::vector<char>& bytes, size_t offset) {
            if (offset + sizeof(uint32_t) > bytes.size()) {
                throw std::runtime_error("truncated 32-bit GLB field");
            }
            uint32_t value = 0;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        Json readRoot(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("unable to open source file");
            std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
            if (bytes.empty()) throw std::runtime_error("source file is empty");

            if (bytes.size() >= 4 && readU32(bytes, 0) == GlbMagic) {
                if (bytes.size() < 20) throw std::runtime_error("truncated GLB header");
                if (readU32(bytes, 4) != GlbVersion) throw std::runtime_error("GLB version must be 2");
                const uint32_t declaredLength = readU32(bytes, 8);
                if (declaredLength != bytes.size()) throw std::runtime_error("GLB length does not match file size");
                const uint32_t chunkLength = readU32(bytes, 12);
                if (readU32(bytes, 16) != GlbJsonChunk || 20ull + chunkLength > bytes.size()) {
                    throw std::runtime_error("GLB first chunk must be a valid JSON chunk");
                }
                const char* begin = bytes.data() + 20;
                return Json::parse(begin, begin + chunkLength);
            }

            const char* begin = bytes.data();
            return Json::parse(begin, begin + bytes.size());
        }

        template <size_t N>
        glm::vec<N, float> vectorValue(const Json& object, const char* key,
            const glm::vec<N, float>& defaultValue, SourceValueOrigin& origin) {
            if (!object.contains(key)) {
                origin = SourceValueOrigin::FormatDefault;
                return defaultValue;
            }
            const Json& values = object.at(key);
            if (!values.is_array() || values.size() != N) {
                throw std::runtime_error(std::string(key) + " must contain " +
                    std::to_string(N) + " numbers");
            }
            glm::vec<N, float> result{};
            for (size_t index = 0; index < N; ++index) {
                result[index] = values.at(index).get<float>();
                if (!std::isfinite(result[index])) throw std::runtime_error(std::string(key) + " must be finite");
            }
            origin = SourceValueOrigin::Authored;
            return result;
        }

        template <typename T>
        SourceValue<T> scalarValue(const Json& object, const char* key, T defaultValue) {
            if (!object.contains(key)) return { std::move(defaultValue), SourceValueOrigin::FormatDefault };
            T value = object.at(key).get<T>();
            if constexpr (std::is_floating_point_v<T>) {
                if (!std::isfinite(value)) throw std::runtime_error(std::string(key) + " must be finite");
            }
            return { std::move(value), SourceValueOrigin::Authored };
        }

        Json objectOrEmpty(const Json& parent, const char* key) {
            if (!parent.contains(key)) return Json::object();
            if (!parent.at(key).is_object()) throw std::runtime_error(std::string(key) + " must be an object");
            return parent.at(key);
        }

        std::string imageIdentity(const Json& root, uint32_t imageIndex) {
            if (!root.contains("images") || !root.at("images").is_array() ||
                imageIndex >= root.at("images").size()) return {};
            const Json& image = root.at("images").at(imageIndex);
            if (image.contains("uri")) {
                const std::string uri = image.at("uri").get<std::string>();
                return uri.starts_with("data:") ? "data-uri#" + std::to_string(imageIndex) : uri;
            }
            if (image.contains("bufferView")) return "bufferView#" + std::to_string(image.at("bufferView").get<uint32_t>());
            return image.value("name", "image#" + std::to_string(imageIndex));
        }

        SourceSampler parseSampler(const Json& root, const Json& texture,
            std::vector<SourceMaterialDiagnostic>& diagnostics, const std::string& path) {
            SourceSampler result{};
            if (!texture.contains("sampler")) return result;
            const uint32_t index = texture.at("sampler").get<uint32_t>();
            result.sourceIndex = index;
            if (!root.contains("samplers") || !root.at("samplers").is_array() ||
                index >= root.at("samplers").size()) {
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_SAMPLER_INDEX",
                    path + "/sampler", "sampler index is out of range");
                return result;
            }
            const Json& sampler = root.at("samplers").at(index);
            if (sampler.contains("magFilter")) result.magFilter = {
                sampler.at("magFilter").get<int32_t>(), SourceValueOrigin::Authored };
            if (sampler.contains("minFilter")) result.minFilter = {
                sampler.at("minFilter").get<int32_t>(), SourceValueOrigin::Authored };
            result.wrapS = scalarValue<int32_t>(sampler, "wrapS", 10497);
            result.wrapT = scalarValue<int32_t>(sampler, "wrapT", 10497);
            constexpr std::array validMag{ 9728, 9729 };
            constexpr std::array validMin{ 9728, 9729, 9984, 9985, 9986, 9987 };
            constexpr std::array validWrap{ 33071, 33648, 10497 };
            const auto valid = [](const auto& values, int32_t value) {
                return std::find(values.begin(), values.end(), value) != values.end();
            };
            if (result.magFilter.value && !valid(validMag, *result.magFilter.value))
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_SAMPLER_FILTER", path, "invalid magFilter");
            if (result.minFilter.value && !valid(validMin, *result.minFilter.value))
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_SAMPLER_FILTER", path, "invalid minFilter");
            if (!valid(validWrap, result.wrapS.value) || !valid(validWrap, result.wrapT.value))
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_SAMPLER_WRAP", path, "invalid sampler wrap mode");
            return result;
        }

        void parseTextureUse(SourceMaterial& material, const Json& root,
            const Json& owner, const TextureSlot& slot,
            std::vector<SourceMaterialDiagnostic>& diagnostics, const std::string& ownerPath) {
            if (!owner.contains(slot.key)) return;
            const Json& info = owner.at(slot.key);
            const std::string path = ownerPath + "/" + slot.key;
            if (!info.is_object() || !info.contains("index")) {
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_TEXTURE_INFO", path,
                    "texture info must be an object with an index");
                return;
            }
            SourceTextureUse use{};
            use.semantic = slot.semantic;
            use.channels = slot.channels;
            use.transfer = slot.transfer;
            use.textureIndex = info.at("index").get<uint32_t>();
            use.texCoord = scalarValue<uint32_t>(info, "texCoord", 0);
            if (slot.scalarKey) use.scalar = scalarValue<float>(info, slot.scalarKey, slot.scalarDefault);

            if (!root.contains("textures") || !root.at("textures").is_array() ||
                use.textureIndex >= root.at("textures").size()) {
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_TEXTURE_INDEX", path + "/index",
                    "texture index is out of range");
            }
            else {
                const Json& texture = root.at("textures").at(use.textureIndex);
                use.sampler = parseSampler(root, texture, diagnostics,
                    "/textures/" + std::to_string(use.textureIndex));
                if (texture.contains("source")) {
                    use.imageIndex = texture.at("source").get<uint32_t>();
                    use.imageIdentity = imageIdentity(root, *use.imageIndex);
                    if (use.imageIdentity.empty()) diagnostic(diagnostics, SourceDiagnosticSeverity::Error,
                        "GLTF_IMAGE_INDEX", "/textures/" + std::to_string(use.textureIndex) + "/source",
                        "image index is out of range");
                }
                else {
                    diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_TEXTURE_SOURCE",
                        "/textures/" + std::to_string(use.textureIndex),
                        "texture has no core image source");
                }
            }

            const Json extensions = objectOrEmpty(info, "extensions");
            if (extensions.contains("KHR_texture_transform")) {
                const Json& transform = extensions.at("KHR_texture_transform");
                use.transform.offset.value = vectorValue<2>(transform, "offset", glm::vec2(0.0f), use.transform.offset.origin);
                use.transform.rotation = scalarValue<float>(transform, "rotation", 0.0f);
                use.transform.scale.value = vectorValue<2>(transform, "scale", glm::vec2(1.0f), use.transform.scale.origin);
                if (transform.contains("texCoord")) use.transform.texCoordOverride = transform.at("texCoord").get<uint32_t>();
            }
            material.textures.push_back(std::move(use));
        }

        SourceAlphaMode parseAlpha(std::string_view value) {
            if (value == "OPAQUE") return SourceAlphaMode::Opaque;
            if (value == "MASK") return SourceAlphaMode::Mask;
            if (value == "BLEND") return SourceAlphaMode::Blend;
            throw std::runtime_error("alphaMode must be OPAQUE, MASK, or BLEND");
        }

        std::vector<std::string> stringArray(const Json& root, const char* key,
            std::vector<SourceMaterialDiagnostic>& diagnostics) {
            std::vector<std::string> result;
            if (!root.contains(key)) return result;
            if (!root.at(key).is_array()) {
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_EXTENSION_LIST",
                    std::string("/") + key, "extension list must be an array");
                return result;
            }
            for (const Json& entry : root.at(key)) result.push_back(entry.get<std::string>());
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        bool finiteInRange(float value, float minimum, float maximum) noexcept {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        void validateMaterialValues(const SourceMaterial& material,
            std::vector<SourceMaterialDiagnostic>& diagnostics, const std::string& path) {
            const auto unit = [&](float value, std::string_view field) {
                if (!finiteInRange(value, 0.0f, 1.0f)) diagnostic(diagnostics,
                    SourceDiagnosticSeverity::Error, "GLTF_FACTOR_RANGE",
                    path + "/" + std::string(field), "factor must be finite and in [0, 1]");
            };
            for (size_t component = 0; component < 4; ++component)
                unit(material.metallicRoughness.baseColorFactor.value[component],
                    "pbrMetallicRoughness/baseColorFactor/" + std::to_string(component));
            unit(material.metallicRoughness.metallicFactor.value, "pbrMetallicRoughness/metallicFactor");
            unit(material.metallicRoughness.roughnessFactor.value, "pbrMetallicRoughness/roughnessFactor");
            unit(material.alphaCutoff.value, "alphaCutoff");
            unit(material.occlusionStrength.value, "occlusionTexture/strength");
            for (size_t component = 0; component < 3; ++component)
                unit(material.emissiveFactor.value[component],
                    "emissiveFactor/" + std::to_string(component));
            for (const SourceMaterialExtension& extension : material.extensions) {
                for (const SourceExtensionProperty& property : extension.properties) {
                    if (property.origin != SourceValueOrigin::Authored) continue;
                    const Json value = Json::parse(property.canonicalValue);
                    constexpr std::array unitScalars{
                        "specularFactor", "clearcoatFactor", "clearcoatRoughnessFactor",
                        "sheenRoughnessFactor", "anisotropyStrength", "iridescenceFactor",
                        "transmissionFactor", "diffuseTransmissionFactor",
                        "glossinessFactor" };
                    if (value.is_number() && std::find(unitScalars.begin(), unitScalars.end(),
                        property.name) != unitScalars.end()) unit(value.get<float>(),
                            "extensions/" + extension.name + "/" + property.name);
                    constexpr std::array nonnegativeScalars{
                        "emissiveStrength", "dispersion", "attenuationDistance",
                        "iridescenceThicknessMinimum", "iridescenceThicknessMaximum",
                        "thicknessFactor" };
                    if (value.is_number() && std::find(nonnegativeScalars.begin(), nonnegativeScalars.end(),
                        property.name) != nonnegativeScalars.end() && value.get<float>() < 0.0f)
                        diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_FACTOR_RANGE",
                            path + "/extensions/" + extension.name + "/" + property.name,
                            "factor must be finite and nonnegative");
                    if (value.is_number() && (property.name == "ior" || property.name == "iridescenceIor") &&
                        value.get<float>() < 1.0f)
                        diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_FACTOR_RANGE",
                            path + "/extensions/" + extension.name + "/" + property.name,
                            "index of refraction must be at least 1");
                    constexpr std::array unitVectors{ "specularColorFactor", "sheenColorFactor",
                        "diffuseTransmissionColorFactor", "diffuseFactor", "specularFactor" };
                    if (value.is_array() && std::find(unitVectors.begin(), unitVectors.end(),
                        property.name) != unitVectors.end())
                        for (size_t component = 0; component < value.size(); ++component)
                            unit(value.at(component).get<float>(), "extensions/" + extension.name +
                                "/" + property.name + "/" + std::to_string(component));
                }
            }
        }

        void parseVariantsAndValidateMeshes(const Json& root,
            const std::vector<SourceMaterial>& materials,
            std::vector<SourceMaterialVariant>& variants,
            std::vector<SourceMaterialVariantMapping>& mappings,
            std::vector<SourceMaterialDiagnostic>& diagnostics) {
            const Json rootExtensions = objectOrEmpty(root, "extensions");
            if (rootExtensions.contains("KHR_materials_variants")) {
                const Json extension = rootExtensions.at("KHR_materials_variants");
                const Json sourceVariants = extension.value("variants", Json::array());
                if (!sourceVariants.is_array())
                    diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_VARIANTS_INVALID",
                        "/extensions/KHR_materials_variants/variants", "variants must be an array");
                else for (size_t index = 0; index < sourceVariants.size(); ++index)
                    variants.push_back({ static_cast<uint32_t>(index),
                        sourceVariants.at(index).value("name", std::string{}) });
            }
            const Json meshes = root.value("meshes", Json::array());
            if (!meshes.is_array()) return;
            for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
                const Json primitives = meshes.at(meshIndex).value("primitives", Json::array());
                for (size_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
                    const Json& primitive = primitives.at(primitiveIndex);
                    const std::string path = "/meshes/" + std::to_string(meshIndex) +
                        "/primitives/" + std::to_string(primitiveIndex);
                    if (primitive.contains("material")) {
                        const uint32_t materialIndex = primitive.at("material").get<uint32_t>();
                        if (materialIndex >= materials.size()) diagnostic(diagnostics,
                            SourceDiagnosticSeverity::Error, "GLTF_MATERIAL_INDEX", path + "/material",
                            "material index is out of range");
                        else {
                            const Json attributes = primitive.value("attributes", Json::object());
                            for (const SourceTextureUse& use : materials[materialIndex].textures) {
                                const uint32_t uv = use.transform.texCoordOverride.value_or(use.texCoord.value);
                                const std::string attribute = "TEXCOORD_" + std::to_string(uv);
                                if (!attributes.contains(attribute)) diagnostic(diagnostics,
                                    SourceDiagnosticSeverity::Error, "GLTF_TEXCOORD_MISSING", path + "/attributes",
                                    "material " + std::to_string(materialIndex) + " requires " + attribute);
                            }
                        }
                    }
                    const Json extensions = objectOrEmpty(primitive, "extensions");
                    if (!extensions.contains("KHR_materials_variants")) continue;
                    const Json sourceMappings = extensions.at("KHR_materials_variants").value("mappings", Json::array());
                    for (size_t mappingIndex = 0; mappingIndex < sourceMappings.size(); ++mappingIndex) {
                        const Json& sourceMapping = sourceMappings.at(mappingIndex);
                        SourceMaterialVariantMapping mapping{};
                        mapping.meshIndex = static_cast<uint32_t>(meshIndex);
                        mapping.primitiveIndex = static_cast<uint32_t>(primitiveIndex);
                        mapping.materialIndex = sourceMapping.value("material", std::numeric_limits<uint32_t>::max());
                        if (mapping.materialIndex >= materials.size()) diagnostic(diagnostics,
                            SourceDiagnosticSeverity::Error, "GLTF_VARIANT_MATERIAL_INDEX", path,
                            "variant material index is out of range");
                        for (const Json& variant : sourceMapping.value("variants", Json::array())) {
                            const uint32_t variantIndex = variant.get<uint32_t>();
                            mapping.variantIndices.push_back(variantIndex);
                            if (variantIndex >= variants.size()) diagnostic(diagnostics,
                                SourceDiagnosticSeverity::Error, "GLTF_VARIANT_INDEX", path,
                                "variant index is out of range");
                        }
                        mappings.push_back(std::move(mapping));
                    }
                }
            }
        }

    } // namespace

    SourceMaterialDocument::SourceMaterialDocument(std::filesystem::path sourcePath,
        std::vector<SourceMaterial> materials,
        std::vector<SourceMaterialDiagnostic> diagnostics,
        std::vector<std::string> extensionsUsed,
        std::vector<std::string> extensionsRequired,
        std::vector<SourceMaterialVariant> variants,
        std::vector<SourceMaterialVariantMapping> variantMappings)
        : m_sourcePath(std::move(sourcePath)), m_materials(std::move(materials)),
          m_diagnostics(std::move(diagnostics)), m_extensionsUsed(std::move(extensionsUsed)),
          m_extensionsRequired(std::move(extensionsRequired)), m_variants(std::move(variants)),
          m_variantMappings(std::move(variantMappings)) {}

    const std::filesystem::path& SourceMaterialDocument::sourcePath() const noexcept { return m_sourcePath; }
    std::span<const SourceMaterial> SourceMaterialDocument::materials() const noexcept { return m_materials; }
    std::span<const SourceMaterialDiagnostic> SourceMaterialDocument::diagnostics() const noexcept { return m_diagnostics; }
    std::span<const std::string> SourceMaterialDocument::extensionsUsed() const noexcept { return m_extensionsUsed; }
    std::span<const std::string> SourceMaterialDocument::extensionsRequired() const noexcept { return m_extensionsRequired; }
    std::span<const SourceMaterialVariant> SourceMaterialDocument::variants() const noexcept { return m_variants; }
    std::span<const SourceMaterialVariantMapping> SourceMaterialDocument::variantMappings() const noexcept { return m_variantMappings; }
    bool SourceMaterialDocument::hasErrors() const noexcept {
        return std::any_of(m_diagnostics.begin(), m_diagnostics.end(), [](const auto& item) {
            return item.severity == SourceDiagnosticSeverity::Error;
        });
    }

    SourceMaterialDocument importGltfSourceMaterials(const std::filesystem::path& path) {
        std::vector<SourceMaterial> materials;
        std::vector<SourceMaterialDiagnostic> diagnostics;
        std::vector<std::string> used;
        std::vector<std::string> required;
        std::vector<SourceMaterialVariant> variants;
        std::vector<SourceMaterialVariantMapping> variantMappings;
        try {
            const Json root = readRoot(path);
            if (!root.is_object()) throw std::runtime_error("glTF root must be an object");
            if (!root.contains("asset") || root.at("asset").value("version", "") != "2.0")
                diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_VERSION", "/asset/version",
                    "material ingestion requires glTF 2.0");
            used = stringArray(root, "extensionsUsed", diagnostics);
            required = stringArray(root, "extensionsRequired", diagnostics);
            const std::set<std::string> usedSet(used.begin(), used.end());
            const std::set<std::string> requiredSet(required.begin(), required.end());
            for (const std::string& name : required) {
                if (!usedSet.contains(name)) diagnostic(diagnostics, SourceDiagnosticSeverity::Error,
                    "GLTF_REQUIRED_NOT_USED", "/extensionsRequired", name + " is required but not listed as used");
                if (findExtensionSpec(name) == nullptr && name != "KHR_texture_transform" &&
                    name != "KHR_materials_variants")
                    diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_REQUIRED_UNSUPPORTED",
                        "/extensionsRequired", "unsupported required extension: " + name);
            }
            for (const std::string& name : used) {
                if (findExtensionSpec(name) == nullptr && name != "KHR_texture_transform" &&
                    name != "KHR_materials_variants")
                    diagnostic(diagnostics, SourceDiagnosticSeverity::Warning, "GLTF_OPTIONAL_UNSUPPORTED",
                        "/extensionsUsed", "unsupported optional extension preserved but not interpreted: " + name);
            }

            const Json sourceMaterials = root.value("materials", Json::array());
            if (!sourceMaterials.is_array()) throw std::runtime_error("materials must be an array");
            materials.reserve(sourceMaterials.size());
            for (size_t index = 0; index < sourceMaterials.size(); ++index) {
                const Json& source = sourceMaterials.at(index);
                const std::string basePath = "/materials/" + std::to_string(index);
                try {
                    if (!source.is_object()) throw std::runtime_error("material must be an object");
                    SourceMaterial material{};
                    material.localIndex = static_cast<uint32_t>(index);
                    material.name = source.value("name", std::string{});
                    const Json pbr = objectOrEmpty(source, "pbrMetallicRoughness");
                    material.metallicRoughness.baseColorFactor.value = vectorValue<4>(pbr,
                        "baseColorFactor", glm::vec4(1.0f), material.metallicRoughness.baseColorFactor.origin);
                    material.metallicRoughness.metallicFactor = scalarValue<float>(pbr, "metallicFactor", 1.0f);
                    material.metallicRoughness.roughnessFactor = scalarValue<float>(pbr, "roughnessFactor", 1.0f);
                    material.emissiveFactor.value = vectorValue<3>(source, "emissiveFactor",
                        glm::vec3(0.0f), material.emissiveFactor.origin);
                    material.doubleSided = scalarValue<bool>(source, "doubleSided", false);
                    material.alphaCutoff = scalarValue<float>(source, "alphaCutoff", 0.5f);
                    if (source.contains("alphaMode")) material.alphaMode = {
                        parseAlpha(source.at("alphaMode").get<std::string>()), SourceValueOrigin::Authored };

                    for (const TextureSlot& slot : corePbrTextures)
                        parseTextureUse(material, root, pbr, slot, diagnostics, basePath + "/pbrMetallicRoughness");
                    for (const TextureSlot& slot : coreMaterialTextures)
                        parseTextureUse(material, root, source, slot, diagnostics, basePath);
                    if (const SourceTextureUse* normal = findSourceTexture(material, SourceTextureSemantic::Normal))
                        material.normalScale = normal->scalar;
                    if (const SourceTextureUse* ao = findSourceTexture(material, SourceTextureSemantic::Occlusion))
                        material.occlusionStrength = ao->scalar;

                    const Json extensions = objectOrEmpty(source, "extensions");
                    for (auto iterator = extensions.begin(); iterator != extensions.end(); ++iterator) {
                        SourceMaterialExtension extension{};
                        extension.name = iterator.key();
                        extension.required = requiredSet.contains(extension.name);
                        extension.canonicalValues = iterator.value().dump();
                        const ExtensionSpec* spec = findExtensionSpec(extension.name);
                        extension.supportedByM2 = spec != nullptr;
                        if (spec != nullptr) {
                            if (!iterator.value().is_object()) throw std::runtime_error(extension.name + " must be an object");
                            for (const auto& [key, defaultValue] : spec->defaults) {
                                const bool authored = iterator.value().contains(key);
                                extension.properties.push_back({ key,
                                    authored ? iterator.value().at(key).dump() : defaultValue,
                                    authored ? SourceValueOrigin::Authored : SourceValueOrigin::FormatDefault });
                            }
                            for (const TextureSlot& slot : spec->textures)
                                parseTextureUse(material, root, iterator.value(), slot, diagnostics,
                                    basePath + "/extensions/" + extension.name);
                        }
                        else {
                            diagnostic(diagnostics, extension.required ? SourceDiagnosticSeverity::Error : SourceDiagnosticSeverity::Warning,
                                extension.required ? "GLTF_REQUIRED_UNSUPPORTED" : "GLTF_MATERIAL_EXTENSION_PRESERVED",
                                basePath + "/extensions/" + extension.name,
                                extension.required ? "required material extension is unsupported" :
                                    "unknown optional material extension is preserved but not interpreted");
                        }
                        material.extensions.push_back(std::move(extension));
                    }
                    if (const SourceMaterialExtension* emissive = findSourceExtension(material,
                        "KHR_materials_emissive_strength")) {
                        const auto property = std::find_if(emissive->properties.begin(), emissive->properties.end(),
                            [](const auto& value) { return value.name == "emissiveStrength"; });
                        material.emissiveStrength = { std::stof(property->canonicalValue), property->origin };
                    }
                    if (findSourceExtension(material, "KHR_materials_unlit") && material.extensions.size() > 1)
                        diagnostic(diagnostics, SourceDiagnosticSeverity::Warning, "GLTF_UNLIT_IGNORES_LOBES",
                            basePath + "/extensions", "unlit material also declares extensions that do not affect unlit shading");
                    validateMaterialValues(material, diagnostics, basePath);
                    materials.push_back(std::move(material));
                }
                catch (const std::exception& exception) {
                    diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_MATERIAL_INVALID",
                        basePath, exception.what());
                }
            }
            parseVariantsAndValidateMeshes(root, materials, variants, variantMappings, diagnostics);
        }
        catch (const std::exception& exception) {
            diagnostic(diagnostics, SourceDiagnosticSeverity::Error, "GLTF_SOURCE_INVALID", "/", exception.what());
        }
        return SourceMaterialDocument(path, std::move(materials), std::move(diagnostics),
            std::move(used), std::move(required), std::move(variants), std::move(variantMappings));
    }

    const SourceTextureUse* findSourceTexture(const SourceMaterial& material,
        SourceTextureSemantic semantic) noexcept {
        const auto found = std::find_if(material.textures.begin(), material.textures.end(),
            [semantic](const SourceTextureUse& use) { return use.semantic == semantic; });
        return found == material.textures.end() ? nullptr : &*found;
    }

    const SourceMaterialExtension* findSourceExtension(const SourceMaterial& material,
        std::string_view name) noexcept {
        const auto found = std::find_if(material.extensions.begin(), material.extensions.end(),
            [name](const SourceMaterialExtension& extension) { return extension.name == name; });
        return found == material.extensions.end() ? nullptr : &*found;
    }

    const char* sourceValueOriginName(SourceValueOrigin origin) noexcept {
        return origin == SourceValueOrigin::Authored ? "authored" : "format-default";
    }
    const char* sourceDiagnosticSeverityName(SourceDiagnosticSeverity severity) noexcept {
        switch (severity) {
        case SourceDiagnosticSeverity::Info: return "info";
        case SourceDiagnosticSeverity::Warning: return "warning";
        case SourceDiagnosticSeverity::Error: return "error";
        }
        return "unknown";
    }
    const char* sourceAlphaModeName(SourceAlphaMode mode) noexcept {
        switch (mode) {
        case SourceAlphaMode::Opaque: return "OPAQUE";
        case SourceAlphaMode::Mask: return "MASK";
        case SourceAlphaMode::Blend: return "BLEND";
        }
        return "OPAQUE";
    }
    const char* sourceTextureSemanticName(SourceTextureSemantic semantic) noexcept {
        constexpr std::array names{ "base-color", "metallic-roughness", "normal", "occlusion", "emissive",
            "diffuse", "specular-glossiness", "clearcoat", "clearcoat-roughness", "clearcoat-normal",
            "sheen-color", "sheen-roughness", "specular", "specular-color", "anisotropy", "iridescence",
            "iridescence-thickness", "transmission", "thickness", "diffuse-transmission",
            "diffuse-transmission-color" };
        return names.at(static_cast<size_t>(semantic));
    }

} // namespace Iridium
