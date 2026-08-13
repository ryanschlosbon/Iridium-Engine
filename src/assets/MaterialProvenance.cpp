#include "assets/MaterialProvenance.h"

#include "material/SourceMaterial.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Iridium {

    namespace {

        MaterialValueOrigin provenanceOrigin(SourceValueOrigin origin) noexcept {
            return origin == SourceValueOrigin::Authored
                ? MaterialValueOrigin::Explicit : MaterialValueOrigin::FormatDefault;
        }

        std::optional<MaterialTextureSemantic> provenanceSemantic(
            SourceTextureSemantic semantic) noexcept {
            switch (semantic) {
            case SourceTextureSemantic::BaseColor: return MaterialTextureSemantic::BaseColor;
            case SourceTextureSemantic::Normal: return MaterialTextureSemantic::Normal;
            case SourceTextureSemantic::MetallicRoughness: return MaterialTextureSemantic::MetallicRoughness;
            case SourceTextureSemantic::Occlusion: return MaterialTextureSemantic::Occlusion;
            case SourceTextureSemantic::Emissive: return MaterialTextureSemantic::Emissive;
            case SourceTextureSemantic::Transmission: return MaterialTextureSemantic::Transmission;
            default: return std::nullopt;
            }
        }

        bool runtimeApplies(std::string_view name) noexcept {
            return name == "KHR_materials_emissive_strength" ||
                name == "KHR_materials_transmission";
        }

        float extensionFloat(const SourceMaterialExtension* extension,
            std::string_view propertyName, float fallback) {
            if (extension == nullptr) return fallback;
            const auto found = std::find_if(extension->properties.begin(), extension->properties.end(),
                [propertyName](const SourceExtensionProperty& property) {
                    return property.name == propertyName;
                });
            return found == extension->properties.end() ? fallback : std::stof(found->canonicalValue);
        }

        void addMissingTextureSlots(MaterialProvenance& result) {
            constexpr std::array slots{
                std::pair{ MaterialTextureSemantic::BaseColor, TextureColorInterpretation::SRGB },
                std::pair{ MaterialTextureSemantic::Normal, TextureColorInterpretation::Linear },
                std::pair{ MaterialTextureSemantic::MetallicRoughness, TextureColorInterpretation::Linear },
                std::pair{ MaterialTextureSemantic::Occlusion, TextureColorInterpretation::Linear },
                std::pair{ MaterialTextureSemantic::Emissive, TextureColorInterpretation::SRGB },
                std::pair{ MaterialTextureSemantic::Transmission, TextureColorInterpretation::Linear },
            };
            constexpr std::array channels{ "RGBA", "RGB", "G=roughness, B=metallic", "R", "RGB", "R" };
            for (size_t index = 0; index < slots.size(); ++index) {
                const bool present = std::any_of(result.textures.begin(), result.textures.end(),
                    [semantic = slots[index].first](const auto& texture) { return texture.semantic == semantic; });
                if (!present) result.textures.push_back({
                    .semantic = slots[index].first,
                    .channels = channels[index],
                    .colorInterpretation = slots[index].second,
                    .consumedByRuntime = slots[index].first != MaterialTextureSemantic::Occlusion,
                });
            }
        }

    } // namespace

    std::vector<MaterialProvenance> inspectGltfMaterialSources(
        const std::filesystem::path& path) {
        const SourceMaterialDocument document = importGltfSourceMaterials(path);
        if (document.hasErrors()) {
            std::string message = "Failed to inspect glTF material source";
            bool fatal = false;
            for (const SourceMaterialDiagnostic& item : document.diagnostics()) {
                if (item.severity == SourceDiagnosticSeverity::Error &&
                    item.code != "GLTF_FACTOR_RANGE") {
                    fatal = true;
                    message += "; " + item.code + " " + item.path + ": " + item.message;
                }
            }
            // The legacy M0 sidecar remains readable for out-of-schema factors so
            // accepted diagnostic fixtures are not made unloadable. The M2 source
            // document still reports those values as errors and the compiler will
            // reject them in strict mode.
            if (fatal) throw std::runtime_error(message);
        }

        std::vector<MaterialProvenance> results;
        results.reserve(document.materials().size());
        for (const SourceMaterial& source : document.materials()) {
            MaterialProvenance result{};
            result.sourceAsset = path.generic_string();
            result.sourceMaterialIndex = source.localIndex;
            result.sourceName = source.name;
            result.baseColor = { source.metallicRoughness.baseColorFactor.value,
                provenanceOrigin(source.metallicRoughness.baseColorFactor.origin) };
            result.metallic = { source.metallicRoughness.metallicFactor.value,
                provenanceOrigin(source.metallicRoughness.metallicFactor.origin) };
            result.roughness = { source.metallicRoughness.roughnessFactor.value,
                provenanceOrigin(source.metallicRoughness.roughnessFactor.origin) };
            result.emissive = { source.emissiveFactor.value, provenanceOrigin(source.emissiveFactor.origin) };
            result.emissiveStrength = { source.emissiveStrength.value,
                provenanceOrigin(source.emissiveStrength.origin) };
            result.normalScale = { source.normalScale.value, provenanceOrigin(source.normalScale.origin) };
            result.alphaMode = { sourceAlphaModeName(source.alphaMode.value),
                provenanceOrigin(source.alphaMode.origin) };
            result.alphaCutoff = { source.alphaCutoff.value, provenanceOrigin(source.alphaCutoff.origin) };
            result.doubleSided = { source.doubleSided.value, provenanceOrigin(source.doubleSided.origin) };
            const SourceMaterialExtension* transmission = findSourceExtension(source,
                "KHR_materials_transmission");
            result.transmission = { extensionFloat(transmission, "transmissionFactor", 0.0f),
                transmission ? provenanceOrigin(transmission->properties.front().origin) :
                    MaterialValueOrigin::FormatDefault };

            for (const SourceTextureUse& use : source.textures) {
                const std::optional<MaterialTextureSemantic> semantic = provenanceSemantic(use.semantic);
                if (!semantic) continue;
                MaterialTextureProvenance texture{};
                texture.semantic = *semantic;
                texture.present = true;
                texture.textureIndex = use.textureIndex;
                texture.imageIndex = use.imageIndex;
                texture.imageIdentity = use.imageIdentity;
                texture.channels = use.channels;
                texture.colorInterpretation = use.transfer == SourceTextureTransfer::Srgb
                    ? TextureColorInterpretation::SRGB : TextureColorInterpretation::Linear;
                texture.uvSet = { use.transform.texCoordOverride.value_or(use.texCoord.value),
                    use.transform.texCoordOverride ? MaterialValueOrigin::Explicit :
                        provenanceOrigin(use.texCoord.origin) };
                texture.sampler.samplerObjectExplicit = use.sampler.sourceIndex.has_value();
                texture.sampler.magFilter = use.sampler.magFilter.value;
                texture.sampler.minFilter = use.sampler.minFilter.value;
                texture.sampler.wrapS = use.sampler.wrapS.value;
                texture.sampler.wrapT = use.sampler.wrapT.value;
                texture.sampler.wrapSExplicit = use.sampler.wrapS.origin == SourceValueOrigin::Authored;
                texture.sampler.wrapTExplicit = use.sampler.wrapT.origin == SourceValueOrigin::Authored;
                texture.usedEngineFallback = false;
                texture.consumedByRuntime = *semantic != MaterialTextureSemantic::Occlusion;
                if (texture.uvSet.value != 0) result.warnings.push_back(
                    std::string(materialTextureSemanticName(*semantic)) + " declares TEXCOORD_" +
                    std::to_string(texture.uvSet.value) +
                    "; the current importer retains only one shared UV stream");
                if (use.transform.offset.origin == SourceValueOrigin::Authored ||
                    use.transform.rotation.origin == SourceValueOrigin::Authored ||
                    use.transform.scale.origin == SourceValueOrigin::Authored)
                    result.warnings.push_back(std::string(materialTextureSemanticName(*semantic)) +
                        " uses KHR_texture_transform, which is not consumed");
                if (texture.sampler.samplerObjectExplicit) result.warnings.push_back(
                    std::string(materialTextureSemanticName(*semantic)) +
                    " declares a source sampler; runtime currently uses engine sampler defaults");
                result.textures.push_back(std::move(texture));
            }
            addMissingTextureSlots(result);
            if (findSourceTexture(source, SourceTextureSemantic::Occlusion))
                result.warnings.push_back("occlusionTexture is present but the current material path does not consume it");

            for (const SourceMaterialExtension& sourceExtension : source.extensions) {
                MaterialExtensionProvenance extension{};
                extension.name = sourceExtension.name;
                extension.required = sourceExtension.required;
                extension.sourceValues = sourceExtension.canonicalValues;
                extension.disposition = runtimeApplies(extension.name)
                    ? MaterialExtensionDisposition::Applied
                    : sourceExtension.supportedByM2
                        ? MaterialExtensionDisposition::ParsedNotConsumed
                        : MaterialExtensionDisposition::UnsupportedIgnored;
                if (extension.disposition != MaterialExtensionDisposition::Applied)
                    result.warnings.push_back(extension.name + (sourceExtension.supportedByM2
                        ? " is present but is not consumed by the current runtime material"
                        : " is unknown and ignored by the current importer"));
                result.extensions.push_back(std::move(extension));
            }
            for (const SourceMaterialDiagnostic& item : document.diagnostics()) {
                if (item.severity != SourceDiagnosticSeverity::Info)
                    result.warnings.push_back(item.code + ": " + item.message);
            }
            results.push_back(std::move(result));
        }
        return results;
    }

    MaterialProvenance makeDefaultMaterialProvenance(
        const std::filesystem::path& path, uint32_t materialIndex) {
        MaterialProvenance result{};
        result.sourceAsset = path.generic_string();
        result.sourceMaterialIndex = materialIndex;
        result.sourceName = "Default Material";
        result.baseColor = { glm::vec4(1.0f), MaterialValueOrigin::FormatDefault };
        result.metallic = { 1.0f, MaterialValueOrigin::FormatDefault };
        result.roughness = { 1.0f, MaterialValueOrigin::FormatDefault };
        result.emissive = { glm::vec3(0.0f), MaterialValueOrigin::FormatDefault };
        result.emissiveStrength = { 1.0f, MaterialValueOrigin::FormatDefault };
        result.normalScale = { 1.0f, MaterialValueOrigin::FormatDefault };
        result.alphaMode = { "OPAQUE", MaterialValueOrigin::FormatDefault };
        result.alphaCutoff = { 0.5f, MaterialValueOrigin::FormatDefault };
        result.transmission = { 0.0f, MaterialValueOrigin::FormatDefault };
        result.doubleSided = { false, MaterialValueOrigin::FormatDefault };
        addMissingTextureSlots(result);
        result.warnings.push_back(
            "primitive has no material; glTF format defaults and engine fallback textures are used");
        return result;
    }

    void attachRuntimeMaterial(MaterialProvenance& provenance,
        const MaterialAsset& runtime, MaterialBinding binding) {
        provenance.runtime = runtime;
        provenance.gpuBinding = binding;
        provenance.pushConstantInputs.baseColor = runtime.baseColor;
        provenance.pushConstantInputs.emissiveFactor = runtime.emissiveFactor;
        provenance.pushConstantInputs.metallic = runtime.metallic;
        provenance.pushConstantInputs.roughness = runtime.roughness;
        provenance.pushConstantInputs.normalScale = runtime.normalScale;
        provenance.pushConstantInputs.alphaCutoff = runtime.alphaCutoff;
        provenance.pushConstantInputs.transmissionFactor = runtime.transmissionFactor;
        constexpr std::array semantics{ MaterialTextureSemantic::BaseColor, MaterialTextureSemantic::Normal,
            MaterialTextureSemantic::MetallicRoughness, MaterialTextureSemantic::Emissive,
            MaterialTextureSemantic::Transmission };
        const std::array handles{ runtime.albedoMap, runtime.normalMap, runtime.pbrMap,
            runtime.emissiveMap, runtime.transmissionMap };
        for (size_t index = 0; index < semantics.size(); ++index)
            for (MaterialTextureProvenance& texture : provenance.textures)
                if (texture.semantic == semantics[index]) {
                    texture.runtimeTexture = handles[index];
                    texture.usedEngineFallback = !texture.present;
                    break;
                }
    }

    void markRuntimeTextureFallbacks(MaterialProvenance& provenance,
        const std::array<TextureHandle, 5>& fallbackHandles) {
        constexpr std::array semantics{ MaterialTextureSemantic::BaseColor, MaterialTextureSemantic::Normal,
            MaterialTextureSemantic::MetallicRoughness, MaterialTextureSemantic::Emissive,
            MaterialTextureSemantic::Transmission };
        for (size_t index = 0; index < semantics.size(); ++index)
            for (MaterialTextureProvenance& texture : provenance.textures) {
                if (texture.semantic != semantics[index]) continue;
                texture.usedEngineFallback = texture.runtimeTexture == fallbackHandles[index];
                if (texture.present && texture.usedEngineFallback) provenance.warnings.push_back(
                    std::string(materialTextureSemanticName(texture.semantic)) +
                    " source texture could not be resolved; engine fallback is bound");
                break;
            }
    }

    const char* materialValueOriginName(MaterialValueOrigin origin) noexcept {
        switch (origin) {
        case MaterialValueOrigin::Explicit: return "explicit";
        case MaterialValueOrigin::FormatDefault: return "glTF default";
        case MaterialValueOrigin::EngineFallback: return "engine fallback";
        case MaterialValueOrigin::Unavailable: return "unavailable";
        }
        return "unknown";
    }
    const char* materialTextureSemanticName(MaterialTextureSemantic semantic) noexcept {
        switch (semantic) {
        case MaterialTextureSemantic::BaseColor: return "base color";
        case MaterialTextureSemantic::Normal: return "normal";
        case MaterialTextureSemantic::MetallicRoughness: return "metallic/roughness";
        case MaterialTextureSemantic::Occlusion: return "occlusion";
        case MaterialTextureSemantic::Emissive: return "emissive";
        case MaterialTextureSemantic::Transmission: return "transmission";
        }
        return "unknown";
    }
    const char* textureColorInterpretationName(TextureColorInterpretation interpretation) noexcept {
        return interpretation == TextureColorInterpretation::SRGB ? "sRGB" : "linear";
    }
    const char* materialExtensionDispositionName(MaterialExtensionDisposition disposition) noexcept {
        switch (disposition) {
        case MaterialExtensionDisposition::Applied: return "applied";
        case MaterialExtensionDisposition::ParsedNotConsumed: return "not consumed";
        case MaterialExtensionDisposition::UnsupportedIgnored: return "unsupported/ignored";
        }
        return "unknown";
    }

} // namespace Iridium
