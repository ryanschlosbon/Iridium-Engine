#include "material/MaterialCompiler.h"
#include "material/StandardMaterialShading.h"

#include "renderer/color/SceneColor.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace Iridium {

    namespace {

        using Json = nlohmann::json;

        constexpr uint32_t textureBit(SourceTextureSemantic semantic) noexcept {
            return 1u << static_cast<uint32_t>(semantic);
        }

        void addDiagnostic(MaterialCompileResult& result, MaterialCompileSeverity severity,
            std::string code, std::string message) {
            result.diagnostics.push_back({ severity, std::move(code), std::move(message) });
        }

        const SourceExtensionProperty* property(const SourceMaterialExtension* extension,
            std::string_view name) noexcept {
            if (extension == nullptr) return nullptr;
            const auto found = std::find_if(extension->properties.begin(), extension->properties.end(),
                [name](const SourceExtensionProperty& value) { return value.name == name; });
            return found == extension->properties.end() ? nullptr : &*found;
        }

        float propertyFloat(const SourceMaterialExtension* extension, std::string_view name,
            float fallback) {
            const SourceExtensionProperty* value = property(extension, name);
            if (!value) return fallback;
            if (value->canonicalValue == "infinity") return std::numeric_limits<float>::infinity();
            return Json::parse(value->canonicalValue).get<float>();
        }

        glm::vec3 propertyVec3(const SourceMaterialExtension* extension, std::string_view name,
            glm::vec3 fallback) {
            const SourceExtensionProperty* value = property(extension, name);
            if (!value) return fallback;
            const Json values = Json::parse(value->canonicalValue);
            return { values.at(0).get<float>(), values.at(1).get<float>(), values.at(2).get<float>() };
        }

        glm::vec4 propertyVec4(const SourceMaterialExtension* extension, std::string_view name,
            glm::vec4 fallback) {
            const SourceExtensionProperty* value = property(extension, name);
            if (!value) return fallback;
            const Json values = Json::parse(value->canonicalValue);
            return { values.at(0).get<float>(), values.at(1).get<float>(),
                values.at(2).get<float>(), values.at(3).get<float>() };
        }

        bool hasTexture(const SourceMaterial& source, SourceTextureSemantic semantic) noexcept {
            return findSourceTexture(source, semantic) != nullptr;
        }

        uint32_t sourceTextureMask(const SourceMaterial& source) noexcept {
            uint32_t mask = 0;
            for (const SourceTextureUse& texture : source.textures) mask |= textureBit(texture.semantic);
            return mask;
        }

        bool finite(glm::vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool finite(glm::vec4 value) noexcept {
            return finite(glm::vec3(value)) && std::isfinite(value.w);
        }

        bool unit(float value) noexcept { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }

        bool validateRecipe(const StandardClosureRecipe& recipe, MaterialCompileResult& result) {
            bool valid = true;
            const auto unitValue = [&](float value, std::string_view name) {
                if (!unit(value)) {
                    valid = false;
                    addDiagnostic(result, MaterialCompileSeverity::Error, "MATERIAL_FACTOR_RANGE",
                        std::string(name) + " must be finite and in [0, 1]");
                }
            };
            for (float value : { recipe.baseColorFactor.r, recipe.baseColorFactor.g,
                recipe.baseColorFactor.b, recipe.baseColorFactor.a, recipe.metallicFactor,
                recipe.roughnessFactor, recipe.specularFactor, recipe.diffuseFactor.r,
                recipe.diffuseFactor.g, recipe.diffuseFactor.b, recipe.diffuseFactor.a,
                recipe.specularGlossinessFactor.r, recipe.specularGlossinessFactor.g,
                recipe.specularGlossinessFactor.b, recipe.glossinessFactor,
                recipe.occlusionStrength, recipe.alphaCutoff }) unitValue(value, "material factor");
            if (!finite(recipe.specularColorFactor) || glm::any(glm::lessThan(recipe.specularColorFactor,
                glm::vec3(0.0f))) || glm::any(glm::greaterThan(recipe.specularColorFactor, glm::vec3(1.0f)))) {
                valid = false;
                addDiagnostic(result, MaterialCompileSeverity::Error, "MATERIAL_FACTOR_RANGE",
                    "specular color factor must be finite and in [0, 1]");
            }
            if (!finite(recipe.emissiveFactor) || glm::any(glm::lessThan(recipe.emissiveFactor,
                glm::vec3(0.0f))) || glm::any(glm::greaterThan(recipe.emissiveFactor,
                glm::vec3(1.0f))) || !std::isfinite(recipe.emissiveStrength) || recipe.emissiveStrength < 0.0f) {
                valid = false;
                addDiagnostic(result, MaterialCompileSeverity::Error, "MATERIAL_EMISSIVE_INVALID",
                    "emissive factor must be in [0, 1] and strength must be finite and nonnegative");
            }
            if (!std::isfinite(recipe.ior) || recipe.ior < 1.0f) {
                valid = false;
                addDiagnostic(result, MaterialCompileSeverity::Error, "MATERIAL_IOR_INVALID",
                    "index of refraction must be finite and at least 1");
            }
            if (!std::isfinite(recipe.normalScale)) {
                valid = false;
                addDiagnostic(result, MaterialCompileSeverity::Error, "MATERIAL_NORMAL_SCALE_INVALID",
                    "normal scale must be finite");
            }
            return valid;
        }

        bool validateComplexLobes(const CompiledMaterial& material,
            MaterialCompileResult& result) {
            bool valid = true;
            const auto error = [&](std::string message) {
                valid = false;
                addDiagnostic(result, MaterialCompileSeverity::Error,
                    "MATERIAL_COMPLEX_PARAMETER_INVALID", std::move(message));
            };
            const auto unitValue = [&](float value, std::string_view name) {
                if (!unit(value)) error(std::string(name) + " must be finite and in [0, 1]");
            };
            const auto unitColor = [&](glm::vec3 color, std::string_view name) {
                if (!finite(color) || glm::any(glm::lessThan(color, glm::vec3(0.0f))) ||
                    glm::any(glm::greaterThan(color, glm::vec3(1.0f))))
                    error(std::string(name) + " must be finite and in [0, 1]");
            };
            for (const ComplexLobeRecord& lobe : material.complexLobes) {
                std::visit([&](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, ClearcoatLobe>) {
                        unitValue(data.factor, "clearcoat factor");
                        unitValue(data.roughnessFactor, "clearcoat roughness");
                        if (!std::isfinite(data.normalScale)) error("clearcoat normal scale must be finite");
                    }
                    else if constexpr (std::is_same_v<T, SheenLobe>) {
                        unitColor(data.color, "sheen color"); unitValue(data.roughnessFactor, "sheen roughness");
                    }
                    else if constexpr (std::is_same_v<T, AnisotropyLobe>) {
                        unitValue(data.strength, "anisotropy strength");
                        if (!std::isfinite(data.rotation)) error("anisotropy rotation must be finite");
                    }
                    else if constexpr (std::is_same_v<T, IridescenceLobe>) {
                        unitValue(data.factor, "iridescence factor");
                        if (!std::isfinite(data.ior) || data.ior < 1.0f) error("iridescence IOR must be at least 1");
                        if (!std::isfinite(data.thicknessMinimumNm) || !std::isfinite(data.thicknessMaximumNm) ||
                            data.thicknessMinimumNm < 0.0f || data.thicknessMaximumNm < data.thicknessMinimumNm)
                            error("iridescence thickness range is invalid");
                    }
                    else if constexpr (std::is_same_v<T, ThinTransmissionLobe>) {
                        unitValue(data.factor, "transmission factor");
                        unitValue(data.specularFactor, "transmission specular factor");
                        unitColor(data.specularColor, "transmission specular color");
                        if (!std::isfinite(data.ior) || data.ior < 1.0f) error("transmission IOR must be at least 1");
                    }
                    else if constexpr (std::is_same_v<T, VolumeTransmissionLobe>) {
                        if (!std::isfinite(data.thicknessFactor) || data.thicknessFactor < 0.0f)
                            error("volume thickness must be finite and nonnegative");
                        if ((!std::isfinite(data.attenuationDistance) && !std::isinf(data.attenuationDistance)) ||
                            data.attenuationDistance <= 0.0f) error("attenuation distance must be positive");
                        unitColor(data.attenuationColor, "attenuation color");
                    }
                    else if constexpr (std::is_same_v<T, DispersionLobe>) {
                        if (!std::isfinite(data.dispersion) || data.dispersion < 0.0f)
                            error("dispersion must be finite and nonnegative");
                    }
                    else if constexpr (std::is_same_v<T, DiffuseTransmissionLobe>) {
                        unitValue(data.factor, "diffuse transmission factor");
                        unitColor(data.color, "diffuse transmission color");
                    }
                }, lobe.data);
            }
            return valid;
        }

        void appendU32(std::vector<std::byte>& bytes, uint32_t value) {
            for (uint32_t shift : { 0u, 8u, 16u, 24u })
                bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
        }

        void appendFloat(std::vector<std::byte>& bytes, float value) {
            appendU32(bytes, std::bit_cast<uint32_t>(value));
        }

        void appendString(std::vector<std::byte>& bytes, std::string_view value) {
            appendU32(bytes, static_cast<uint32_t>(value.size()));
            for (char character : value) bytes.push_back(static_cast<std::byte>(character));
        }

        void appendOrigin(std::vector<std::byte>& bytes, SourceValueOrigin origin) {
            appendU32(bytes, static_cast<uint32_t>(origin));
        }

        void appendTextureMaskBit(std::vector<std::byte>& bytes, uint32_t mask) {
            appendU32(bytes, mask);
        }

        void resolveTransparencyPolicy(CompiledMaterial& material,
            TransparencyPolicyV1 policy, TransparencyTopology topology,
            MaterialCompileResult& result) {
            CompiledTransparencyPolicy resolved;
            if (!isAuthoredTransparencyClass(policy.requestedClass)) {
                policy.requestedClass = TransparencyClass::Auto;
                resolved.flags |= CompiledTransparencyPolicySanitized;
                addDiagnostic(result, MaterialCompileSeverity::Warning,
                    "MATERIAL_TRANSPARENCY_CLASS_UNKNOWN",
                    "unknown transparency class was normalized to Auto");
            }
            if (!isTransparencyQuality(policy.quality)) {
                policy.quality = TransparencyQuality::Ordinary2;
                resolved.flags |= CompiledTransparencyPolicySanitized;
                addDiagnostic(result, MaterialCompileSeverity::Warning,
                    "MATERIAL_TRANSPARENCY_QUALITY_UNKNOWN",
                    "unknown transparency quality was normalized to Ordinary2");
            }
            if (!std::isfinite(policy.thinSheetThicknessMeters) ||
                policy.thinSheetThicknessMeters < 0.0f ||
                policy.thinSheetThicknessMeters > 1.0e6f) {
                policy.thinSheetThicknessMeters = 0.0f;
                resolved.flags |= CompiledTransparencyPolicySanitized;
                addDiagnostic(result, MaterialCompileSeverity::Warning,
                    "MATERIAL_TRANSPARENCY_THICKNESS_INVALID",
                    "thin-sheet thickness must be finite and in [0, 1000000] metres; zero was selected");
            }

            resolved.requestedClass = policy.requestedClass;
            resolved.quality = policy.quality;
            resolved.priority = policy.priority;
            resolved.thinSheetThicknessMeters =
                policy.thinSheetThicknessMeters;
            if (policy.requestedClass != TransparencyClass::Auto) {
                resolved.flags |= CompiledTransparencyExplicitClass;
            }

            const bool transmission =
                (material.featureFlags & (MaterialFeatureTransmission |
                    MaterialFeatureDiffuseTransmission)) != 0;
            const bool volume =
                (material.featureFlags & MaterialFeatureVolume) != 0;
            const bool dispersion =
                (material.featureFlags & MaterialFeatureDispersion) != 0;
            const auto autoClass = [&]() {
                if (volume) {
                    if (topology == TransparencyTopology::ValidClosed) {
                        return TransparencyClass::LayeredGlass;
                    }
                    resolved.flags |= CompiledTransparencyTopologyRequired;
                    if (topology == TransparencyTopology::Invalid) {
                        resolved.flags |= CompiledTransparencyFallbackApplied;
                    }
                    return TransparencyClass::ThinGlass;
                }
                if (transmission) return TransparencyClass::ThinGlass;
                if (material.standard.alphaMode == SourceAlphaMode::Mask) {
                    return TransparencyClass::AlphaClip;
                }
                if (material.standard.alphaMode == SourceAlphaMode::Blend) {
                    return TransparencyClass::SortedSurface;
                }
                return TransparencyClass::None;
            };
            const auto fallback = [&](TransparencyClass value,
                std::string code, std::string message) {
                resolved.flags |= CompiledTransparencyFallbackApplied;
                addDiagnostic(result, MaterialCompileSeverity::Warning,
                    std::move(code), std::move(message));
                return value;
            };

            switch (policy.requestedClass) {
            case TransparencyClass::Auto:
                resolved.resolvedClass = autoClass();
                break;
            case TransparencyClass::None:
                resolved.requestedClass = TransparencyClass::Auto;
                resolved.flags |= CompiledTransparencyPolicySanitized;
                resolved.resolvedClass = autoClass();
                break;
            case TransparencyClass::AlphaClip:
                if (transmission || material.standard.alphaMode !=
                        SourceAlphaMode::Mask) {
                    resolved.resolvedClass = fallback(autoClass(),
                        "MATERIAL_TRANSPARENCY_ALPHA_CLIP_INCOMPATIBLE",
                        "AlphaClip requires non-transmissive mask coverage; the safe Auto class was selected");
                } else {
                    resolved.resolvedClass = TransparencyClass::AlphaClip;
                }
                break;
            case TransparencyClass::SortedSurface:
                if (transmission || volume || dispersion) {
                    resolved.resolvedClass = fallback(
                        TransparencyClass::ThinGlass,
                        "MATERIAL_TRANSPARENCY_SORTED_INCOMPATIBLE",
                        "SortedSurface cannot carry transmissive or volume transport; ThinGlass was selected");
                } else {
                    resolved.resolvedClass =
                        TransparencyClass::SortedSurface;
                }
                break;
            case TransparencyClass::ThinGlass:
                resolved.resolvedClass = TransparencyClass::ThinGlass;
                break;
            case TransparencyClass::LayeredGlass:
                if (!volume || topology !=
                        TransparencyTopology::ValidClosed) {
                    resolved.flags |= CompiledTransparencyTopologyRequired;
                    resolved.resolvedClass = fallback(
                        TransparencyClass::ThinGlass,
                        "MATERIAL_TRANSPARENCY_LAYERED_FALLBACK",
                        "LayeredGlass requires validated closed volume topology; ThinGlass was selected");
                } else {
                    resolved.resolvedClass =
                        TransparencyClass::LayeredGlass;
                }
                break;
            case TransparencyClass::WeightedOit:
                if (transmission || volume || dispersion) {
                    resolved.resolvedClass = fallback(
                        TransparencyClass::SortedSurface,
                        "MATERIAL_TRANSPARENCY_OIT_INCOMPATIBLE",
                        "WeightedOIT forbids refractive, volume, and dispersive transport; SortedSurface was selected");
                } else {
                    resolved.resolvedClass = TransparencyClass::WeightedOit;
                }
                break;
            }
            material.transparency = resolved;
        }

        std::string compiledHash(const CompiledMaterial& material) {
            std::vector<std::byte> bytes;
            bytes.reserve(256 + material.complexLobes.size() * 64);
            appendU32(bytes, material.schemaVersion);
            appendU32(bytes, static_cast<uint32_t>(material.workflow));
            appendU32(bytes, static_cast<uint32_t>(material.closureClass));
            appendU32(bytes, material.featureFlags);
            appendU32(bytes, packTransparencyPolicyWord(
                material.transparency));
            appendU32(bytes, static_cast<uint32_t>(
                material.transparency.priority));
            appendFloat(bytes,
                material.transparency.thinSheetThicknessMeters);
            const StandardClosureRecipe& recipe = material.standard;
            for (float value : { recipe.baseColorFactor.r, recipe.baseColorFactor.g,
                recipe.baseColorFactor.b, recipe.baseColorFactor.a, recipe.metallicFactor,
                recipe.roughnessFactor, recipe.ior, recipe.specularFactor,
                recipe.specularColorFactor.r, recipe.specularColorFactor.g,
                recipe.specularColorFactor.b, recipe.diffuseFactor.r, recipe.diffuseFactor.g,
                recipe.diffuseFactor.b, recipe.diffuseFactor.a,
                recipe.specularGlossinessFactor.r, recipe.specularGlossinessFactor.g,
                recipe.specularGlossinessFactor.b, recipe.glossinessFactor,
                recipe.emissiveFactor.r, recipe.emissiveFactor.g, recipe.emissiveFactor.b,
                recipe.emissiveStrength, recipe.normalScale, recipe.occlusionStrength,
                recipe.alphaCutoff }) appendFloat(bytes, value);
            appendU32(bytes, static_cast<uint32_t>(recipe.alphaMode));
            appendU32(bytes, recipe.doubleSided ? 1u : 0u);
            appendU32(bytes, recipe.textureMask);
            appendU32(bytes, static_cast<uint32_t>(material.textureOperations.size()));
            for (const CompiledTextureOperation& texture : material.textureOperations) {
                appendU32(bytes, static_cast<uint32_t>(texture.semantic));
                appendU32(bytes, texture.sourceTextureIndex);
                appendU32(bytes, texture.sourceImageIndex ? 1u : 0u);
                appendU32(bytes, texture.sourceImageIndex.value_or(0u));
                appendString(bytes, texture.imageIdentity);
                appendString(bytes, texture.channels);
                appendU32(bytes, static_cast<uint32_t>(texture.transfer));
                appendU32(bytes, texture.texCoord);
                appendFloat(bytes, texture.transform.offset.value.x);
                appendFloat(bytes, texture.transform.offset.value.y);
                appendOrigin(bytes, texture.transform.offset.origin);
                appendFloat(bytes, texture.transform.rotation.value);
                appendOrigin(bytes, texture.transform.rotation.origin);
                appendFloat(bytes, texture.transform.scale.value.x);
                appendFloat(bytes, texture.transform.scale.value.y);
                appendOrigin(bytes, texture.transform.scale.origin);
                appendU32(bytes, texture.transform.texCoordOverride ? 1u : 0u);
                appendU32(bytes, texture.transform.texCoordOverride.value_or(0u));
                appendU32(bytes, texture.sampler.sourceIndex ? 1u : 0u);
                appendU32(bytes, texture.sampler.sourceIndex.value_or(0u));
                appendU32(bytes, texture.sampler.magFilter.value ? 1u : 0u);
                appendU32(bytes, texture.sampler.magFilter.value.value_or(0));
                appendOrigin(bytes, texture.sampler.magFilter.origin);
                appendU32(bytes, texture.sampler.minFilter.value ? 1u : 0u);
                appendU32(bytes, texture.sampler.minFilter.value.value_or(0));
                appendOrigin(bytes, texture.sampler.minFilter.origin);
                appendU32(bytes, static_cast<uint32_t>(texture.sampler.wrapS.value));
                appendOrigin(bytes, texture.sampler.wrapS.origin);
                appendU32(bytes, static_cast<uint32_t>(texture.sampler.wrapT.value));
                appendOrigin(bytes, texture.sampler.wrapT.origin);
                appendFloat(bytes, texture.scalar.value);
                appendOrigin(bytes, texture.scalar.origin);
            }
            appendU32(bytes, static_cast<uint32_t>(material.complexLobes.size()));
            for (const ComplexLobeRecord& lobe : material.complexLobes) {
                appendU32(bytes, static_cast<uint32_t>(lobe.type));
                appendString(bytes, lobe.sourceExtension);
                std::visit([&bytes](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, ClearcoatLobe>) {
                        appendFloat(bytes, data.factor); appendFloat(bytes, data.roughnessFactor);
                        appendFloat(bytes, data.normalScale);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, SheenLobe>) {
                        appendFloat(bytes, data.color.r); appendFloat(bytes, data.color.g);
                        appendFloat(bytes, data.color.b); appendFloat(bytes, data.roughnessFactor);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, AnisotropyLobe>) {
                        appendFloat(bytes, data.strength); appendFloat(bytes, data.rotation);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, IridescenceLobe>) {
                        appendFloat(bytes, data.factor); appendFloat(bytes, data.ior);
                        appendFloat(bytes, data.thicknessMinimumNm); appendFloat(bytes, data.thicknessMaximumNm);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, ThinTransmissionLobe>) {
                        appendFloat(bytes, data.factor); appendFloat(bytes, data.ior);
                        appendFloat(bytes, data.specularFactor); appendFloat(bytes, data.specularColor.r);
                        appendFloat(bytes, data.specularColor.g); appendFloat(bytes, data.specularColor.b);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, VolumeTransmissionLobe>) {
                        appendFloat(bytes, data.thicknessFactor); appendFloat(bytes, data.attenuationDistance);
                        appendFloat(bytes, data.attenuationColor.r); appendFloat(bytes, data.attenuationColor.g);
                        appendFloat(bytes, data.attenuationColor.b); appendTextureMaskBit(bytes, data.textureMask);
                    }
                    else if constexpr (std::is_same_v<T, DispersionLobe>) appendFloat(bytes, data.dispersion);
                    else if constexpr (std::is_same_v<T, DiffuseTransmissionLobe>) {
                        appendFloat(bytes, data.factor); appendFloat(bytes, data.color.r);
                        appendFloat(bytes, data.color.g); appendFloat(bytes, data.color.b);
                        appendTextureMaskBit(bytes, data.textureMask);
                    }
                }, lobe.data);
            }
            return sha256(bytes);
        }

        glm::vec3 decodeSrgb(glm::vec3 value) {
            return { static_cast<float>(Color::decodeSrgb(value.r)),
                static_cast<float>(Color::decodeSrgb(value.g)),
                static_cast<float>(Color::decodeSrgb(value.b)) };
        }

        glm::vec3 toAcesCg(glm::vec3 value) {
            const Color::Rgb result = Color::linearSrgbToAcesCg({ value.r, value.g, value.b });
            return { static_cast<float>(result.r), static_cast<float>(result.g),
                static_cast<float>(result.b) };
        }

        void validateEvaluationInputs(const MaterialSurfaceInputs& inputs) {
            for (const glm::vec4 sample : inputs.textures.encodedOrLinear)
                if (!finite(sample) || glm::any(glm::lessThan(sample, glm::vec4(0.0f))) ||
                    glm::any(glm::greaterThan(sample, glm::vec4(1.0f))))
                    throw std::domain_error("material texture samples must be finite and in [0, 1]");
            if (!finite(inputs.vertexColor) || glm::any(glm::lessThan(inputs.vertexColor,
                glm::vec4(0.0f))) || glm::any(glm::greaterThan(inputs.vertexColor, glm::vec4(1.0f))))
                throw std::domain_error("vertex color must be finite and in [0, 1]");
            if (!std::isfinite(inputs.tangentHandedness) || inputs.tangentHandedness == 0.0f)
                throw std::domain_error("tangent handedness must be finite and nonzero");
        }

    } // namespace

    bool MaterialCompileResult::succeeded() const noexcept {
        return material != nullptr && std::none_of(diagnostics.begin(), diagnostics.end(),
            [](const MaterialCompileDiagnostic& value) { return value.severity == MaterialCompileSeverity::Error; });
    }

    bool MaterialCompileDocumentResult::succeeded() const noexcept {
        return std::none_of(diagnostics.begin(), diagnostics.end(),
            [](const MaterialCompileDiagnostic& value) { return value.severity == MaterialCompileSeverity::Error; }) &&
            std::all_of(materials.begin(), materials.end(),
                [](const MaterialCompileResult& value) { return value.succeeded(); });
    }

    MaterialTextureSamples::MaterialTextureSamples() noexcept {
        encodedOrLinear.fill(glm::vec4(1.0f));
    }
    const glm::vec4& MaterialTextureSamples::sample(SourceTextureSemantic semantic) const noexcept {
        return encodedOrLinear[static_cast<size_t>(semantic)];
    }
    glm::vec4& MaterialTextureSamples::sample(SourceTextureSemantic semantic) noexcept {
        return encodedOrLinear[static_cast<size_t>(semantic)];
    }

    MaterialCompileResult compileSourceMaterial(const SourceMaterial& source,
        MaterialCompilePolicy policy, std::span<const SourceMaterialDiagnostic> sourceDiagnostics) {
        MaterialCompileResult result{};
        for (const SourceMaterialDiagnostic& diagnostic : sourceDiagnostics) {
            const MaterialCompileSeverity severity = diagnostic.severity == SourceDiagnosticSeverity::Error
                ? MaterialCompileSeverity::Error : diagnostic.severity == SourceDiagnosticSeverity::Warning
                    ? MaterialCompileSeverity::Warning : MaterialCompileSeverity::Info;
            addDiagnostic(result, severity, "SOURCE_" + diagnostic.code,
                diagnostic.path + ": " + diagnostic.message);
        }

        CompiledMaterial compiled{};
        compiled.sourceMaterialIndex = source.localIndex;
        compiled.sourceName = source.name;
        StandardClosureRecipe& recipe = compiled.standard;
        recipe.baseColorFactor = source.metallicRoughness.baseColorFactor.value;
        recipe.metallicFactor = source.metallicRoughness.metallicFactor.value;
        recipe.roughnessFactor = source.metallicRoughness.roughnessFactor.value;
        recipe.emissiveFactor = source.emissiveFactor.value;
        recipe.emissiveStrength = source.emissiveStrength.value;
        recipe.normalScale = source.normalScale.value;
        recipe.occlusionStrength = source.occlusionStrength.value;
        recipe.alphaMode = source.alphaMode.value;
        recipe.alphaCutoff = source.alphaCutoff.value;
        recipe.doubleSided = source.doubleSided.value;
        recipe.textureMask = sourceTextureMask(source);
        compiled.textureOperations.reserve(source.textures.size());
        for (const SourceTextureUse& texture : source.textures) {
            compiled.textureOperations.push_back({ texture.semantic, texture.textureIndex,
                texture.imageIndex, texture.imageIdentity, texture.channels, texture.transfer,
                texture.texCoord.value, texture.transform, texture.sampler, texture.scalar });
        }
        std::sort(compiled.textureOperations.begin(), compiled.textureOperations.end(),
            [](const CompiledTextureOperation& lhs, const CompiledTextureOperation& rhs) {
                return static_cast<uint32_t>(lhs.semantic) < static_cast<uint32_t>(rhs.semantic);
            });

        if (hasTexture(source, SourceTextureSemantic::Normal)) compiled.featureFlags |= MaterialFeatureNormalMap;
        if (hasTexture(source, SourceTextureSemantic::Occlusion)) compiled.featureFlags |= MaterialFeatureOcclusion;
        if (hasTexture(source, SourceTextureSemantic::Emissive) ||
            glm::any(glm::greaterThan(recipe.emissiveFactor * recipe.emissiveStrength, glm::vec3(0.0f))))
            compiled.featureFlags |= MaterialFeatureEmissive;
        if (recipe.alphaMode == SourceAlphaMode::Mask) compiled.featureFlags |= MaterialFeatureAlphaMask;
        if (recipe.alphaMode == SourceAlphaMode::Blend) compiled.featureFlags |= MaterialFeatureAlphaBlend;
        if (recipe.doubleSided) compiled.featureFlags |= MaterialFeatureDoubleSided;
        for (const SourceTextureUse& texture : source.textures)
            if (texture.transform.offset.origin == SourceValueOrigin::Authored ||
                texture.transform.rotation.origin == SourceValueOrigin::Authored ||
                texture.transform.scale.origin == SourceValueOrigin::Authored ||
                texture.transform.texCoordOverride)
                compiled.featureFlags |= MaterialFeatureTextureTransform;

        for (const SourceMaterialExtension& extension : source.extensions) {
            if (extension.supportedByM2) continue;
            const bool error = extension.required || policy == MaterialCompilePolicy::Strict;
            addDiagnostic(result, error ? MaterialCompileSeverity::Error : MaterialCompileSeverity::Warning,
                extension.required ? "MATERIAL_REQUIRED_EXTENSION_UNSUPPORTED" :
                    error ? "MATERIAL_OPTIONAL_EXTENSION_STRICT" : "MATERIAL_OPTIONAL_EXTENSION_FALLBACK",
                extension.name + (error ? " cannot be compiled under the selected policy" :
                    " is preserved but the base workflow is used explicitly"));
            if (!error) compiled.featureFlags |= MaterialFeaturePermissiveFallback;
        }

        const SourceMaterialExtension* ior = findSourceExtension(source, "KHR_materials_ior");
        const SourceMaterialExtension* specular = findSourceExtension(source, "KHR_materials_specular");
        recipe.ior = propertyFloat(ior, "ior", 1.5f);
        recipe.specularFactor = propertyFloat(specular, "specularFactor", 1.0f);
        recipe.specularColorFactor = propertyVec3(specular, "specularColorFactor", glm::vec3(1.0f));
        if (specular) compiled.featureFlags |= MaterialFeatureSpecular;

        const SourceMaterialExtension* specGloss = findSourceExtension(source,
            "KHR_materials_pbrSpecularGlossiness");
        if (specGloss) {
            compiled.workflow = MaterialWorkflow::SpecularGlossiness;
            recipe.diffuseFactor = propertyVec4(specGloss, "diffuseFactor", glm::vec4(1.0f));
            recipe.specularGlossinessFactor = propertyVec3(specGloss, "specularFactor", glm::vec3(1.0f));
            recipe.glossinessFactor = propertyFloat(specGloss, "glossinessFactor", 1.0f);
            addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_SPEC_GLOSS_PRECEDENCE",
                "archived specular/glossiness workflow takes precedence over core metallic/roughness");
            if (ior || specular) addDiagnostic(result, MaterialCompileSeverity::Error,
                "MATERIAL_SPEC_GLOSS_EXTENSION_CONFLICT",
                "KHR_materials_ior/specular cannot be combined with archived specular/glossiness");
        }

        const SourceMaterialExtension* unlit = findSourceExtension(source, "KHR_materials_unlit");
        if (unlit) {
            compiled.workflow = MaterialWorkflow::Unlit;
            compiled.closureClass = MaterialClosureClass::Unlit;
            compiled.featureFlags |= MaterialFeatureUnlit;
            for (const SourceMaterialExtension& extension : source.extensions)
                if (extension.name != "KHR_materials_unlit")
                    addDiagnostic(result, MaterialCompileSeverity::Warning, "MATERIAL_UNLIT_INPUT_IGNORED",
                        extension.name + " does not participate in the unlit closure");
        }
        else {
            const auto maskFor = [&](std::initializer_list<SourceTextureSemantic> semantics) {
                uint32_t mask = 0;
                for (const SourceTextureSemantic semantic : semantics)
                    if (hasTexture(source, semantic)) mask |= textureBit(semantic);
                return mask;
            };
            const SourceMaterialExtension* clearcoat = findSourceExtension(source, "KHR_materials_clearcoat");
            if (clearcoat) {
                const float factor = propertyFloat(clearcoat, "clearcoatFactor", 0.0f);
                if (factor != 0.0f) {
                    compiled.featureFlags |= MaterialFeatureClearcoat;
                    const SourceTextureUse* normal = findSourceTexture(source,
                        SourceTextureSemantic::ClearcoatNormal);
                    compiled.complexLobes.push_back({ ComplexLobeType::Clearcoat,
                        "KHR_materials_clearcoat", ClearcoatLobe{ factor,
                            propertyFloat(clearcoat, "clearcoatRoughnessFactor", 0.0f),
                            normal ? normal->scalar.value : 1.0f,
                            maskFor({ SourceTextureSemantic::Clearcoat,
                                SourceTextureSemantic::ClearcoatRoughness,
                                SourceTextureSemantic::ClearcoatNormal }) } });
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_clearcoat is mathematically dormant at zero factor");
            }
            const SourceMaterialExtension* sheen = findSourceExtension(source, "KHR_materials_sheen");
            if (sheen) {
                const glm::vec3 sheenColor = propertyVec3(sheen, "sheenColorFactor", glm::vec3(0.0f));
                if (glm::any(glm::notEqual(sheenColor, glm::vec3(0.0f))))
                {
                    compiled.featureFlags |= MaterialFeatureSheen;
                    compiled.complexLobes.push_back({ ComplexLobeType::Sheen,
                        "KHR_materials_sheen", SheenLobe{ sheenColor,
                            propertyFloat(sheen, "sheenRoughnessFactor", 0.0f),
                            maskFor({ SourceTextureSemantic::SheenColor,
                                SourceTextureSemantic::SheenRoughness }) } });
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_sheen is mathematically dormant at zero color");
            }
            const SourceMaterialExtension* anisotropy = findSourceExtension(source, "KHR_materials_anisotropy");
            if (anisotropy) {
                const float strength = propertyFloat(anisotropy, "anisotropyStrength", 0.0f);
                if (strength != 0.0f) {
                    compiled.featureFlags |= MaterialFeatureAnisotropy;
                    compiled.complexLobes.push_back({ ComplexLobeType::Anisotropy,
                    "KHR_materials_anisotropy", AnisotropyLobe{ strength,
                        propertyFloat(anisotropy, "anisotropyRotation", 0.0f),
                        maskFor({ SourceTextureSemantic::Anisotropy }) } });
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_anisotropy is mathematically dormant at zero strength");
            }
            const SourceMaterialExtension* iridescence = findSourceExtension(source, "KHR_materials_iridescence");
            if (iridescence) {
                const float factor = propertyFloat(iridescence, "iridescenceFactor", 0.0f);
                if (factor != 0.0f) {
                    compiled.featureFlags |= MaterialFeatureIridescence;
                    compiled.complexLobes.push_back({ ComplexLobeType::Iridescence,
                    "KHR_materials_iridescence", IridescenceLobe{ factor,
                        propertyFloat(iridescence, "iridescenceIor", 1.3f),
                        propertyFloat(iridescence, "iridescenceThicknessMinimum", 100.0f),
                        propertyFloat(iridescence, "iridescenceThicknessMaximum", 400.0f),
                        maskFor({ SourceTextureSemantic::Iridescence,
                            SourceTextureSemantic::IridescenceThickness }) } });
                    addDiagnostic(result, MaterialCompileSeverity::Warning,
                        "MATERIAL_IRIDESCENCE_M2_APPROXIMATION",
                        "M2 uses a bounded RGB thin-film approximation; spectral transport remains explicit future work");
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_iridescence is mathematically dormant at zero factor");
            }

            const SourceMaterialExtension* transmission = findSourceExtension(source, "KHR_materials_transmission");
            const float transmissionFactor = propertyFloat(transmission, "transmissionFactor", 0.0f);
            const bool transmissionActive = transmission && transmissionFactor != 0.0f;
            if (transmission) {
                if (transmissionActive) {
                    compiled.featureFlags |= MaterialFeatureTransmission;
                    compiled.complexLobes.push_back({
                    ComplexLobeType::ThinTransmission, "KHR_materials_transmission",
                    ThinTransmissionLobe{ transmissionFactor, recipe.ior,
                        recipe.specularFactor, recipe.specularColorFactor,
                        maskFor({ SourceTextureSemantic::Transmission }) } });
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_transmission is mathematically dormant at zero factor");
            }
            if (transmissionActive) {
                addDiagnostic(result, MaterialCompileSeverity::Warning,
                    "MATERIAL_TRANSPORT_DEFERRED_M6",
                    "transmission closure data is exact; production refraction/ordering remains M6 work");
                if (recipe.metallicFactor > 0.0f)
                    addDiagnostic(result, MaterialCompileSeverity::Warning,
                        "MATERIAL_TRANSMISSION_METALLIC_SUPPRESSED",
                        "metallic content suppresses the dielectric portion available to transmission");
            }

            const SourceMaterialExtension* volume = findSourceExtension(source, "KHR_materials_volume");
            const float thickness = propertyFloat(volume, "thicknessFactor", 0.0f);
            if (volume && thickness != 0.0f) {
                if (!transmissionActive) addDiagnostic(result, MaterialCompileSeverity::Error,
                    "MATERIAL_VOLUME_WITHOUT_TRANSMISSION",
                    "active volume requires active KHR_materials_transmission");
                else compiled.complexLobes.push_back({ ComplexLobeType::VolumeTransmission,
                    "KHR_materials_volume", VolumeTransmissionLobe{ thickness,
                        propertyFloat(volume, "attenuationDistance", std::numeric_limits<float>::infinity()),
                        propertyVec3(volume, "attenuationColor", glm::vec3(1.0f)),
                        maskFor({ SourceTextureSemantic::Thickness }) } });
                if (transmissionActive) compiled.featureFlags |= MaterialFeatureVolume;
            }
            else if (volume) addDiagnostic(result, MaterialCompileSeverity::Info,
                "MATERIAL_DORMANT_EXTENSION", "KHR_materials_volume is dormant at zero thickness");

            const SourceMaterialExtension* dispersion = findSourceExtension(source, "KHR_materials_dispersion");
            const float dispersionFactor = propertyFloat(dispersion, "dispersion", 0.0f);
            if (dispersion && dispersionFactor != 0.0f) {
                if (!transmissionActive) addDiagnostic(result, MaterialCompileSeverity::Error,
                    "MATERIAL_DISPERSION_WITHOUT_TRANSMISSION",
                    "active dispersion requires active KHR_materials_transmission");
                else if (!volume) addDiagnostic(result, MaterialCompileSeverity::Error,
                    "MATERIAL_DISPERSION_WITHOUT_VOLUME",
                    "active dispersion requires KHR_materials_volume");
                else {
                    compiled.featureFlags |= MaterialFeatureDispersion;
                    compiled.complexLobes.push_back({ ComplexLobeType::Dispersion,
                        "KHR_materials_dispersion", DispersionLobe{ dispersionFactor } });
                    addDiagnostic(result, MaterialCompileSeverity::Warning,
                        "MATERIAL_DISPERSION_TRANSPORT_DEFERRED_M6",
                        "dispersion is preserved in the closure but the bounded M1 transport remains achromatic until M6");
                }
            }
            else if (dispersion) addDiagnostic(result, MaterialCompileSeverity::Info,
                "MATERIAL_DORMANT_EXTENSION", "KHR_materials_dispersion is dormant at zero factor");

            const SourceMaterialExtension* diffuseTransmission = findSourceExtension(source,
                "KHR_materials_diffuse_transmission");
            if (diffuseTransmission) {
                const float factor = propertyFloat(diffuseTransmission, "diffuseTransmissionFactor", 0.0f);
                if (factor != 0.0f) {
                    compiled.featureFlags |= MaterialFeatureDiffuseTransmission;
                    compiled.complexLobes.push_back({
                    ComplexLobeType::DiffuseTransmission, "KHR_materials_diffuse_transmission",
                    DiffuseTransmissionLobe{ factor,
                        propertyVec3(diffuseTransmission, "diffuseTransmissionColorFactor", glm::vec3(1.0f)),
                        maskFor({ SourceTextureSemantic::DiffuseTransmission,
                            SourceTextureSemantic::DiffuseTransmissionColor }) } });
                }
                else addDiagnostic(result, MaterialCompileSeverity::Info, "MATERIAL_DORMANT_EXTENSION",
                    "KHR_materials_diffuse_transmission is mathematically dormant at zero factor");
                if (factor != 0.0f) addDiagnostic(result, MaterialCompileSeverity::Warning,
                    "MATERIAL_DIFFUSE_TRANSPORT_DEFERRED",
                    "diffuse-transmission data is retained; refraction and volume transport remain M6 work");
            }

            compiled.closureClass = !compiled.complexLobes.empty()
                ? MaterialClosureClass::ComplexForward
                : recipe.alphaMode == SourceAlphaMode::Blend
                    ? MaterialClosureClass::StandardForward
                    : MaterialClosureClass::StandardDeferred;
            if (!compiled.complexLobes.empty()) addDiagnostic(result, MaterialCompileSeverity::Info,
                "MATERIAL_COMPLEX_FORWARD", "active non-reducible lobe selects complex forward closure");
            if (!compiled.complexLobes.empty()) addDiagnostic(result,
                MaterialCompileSeverity::Info, "MATERIAL_COMPLEX_CLUSTERED_LIGHTING_M5",
                "complex-forward shading consumes the shared M5 clustered lights, shadows, and reflection probes");
        }

        if (!validateRecipe(recipe, result) || !validateComplexLobes(compiled, result))
            compiled.closureClass = MaterialClosureClass::Invalid;
        resolveTransparencyPolicy(compiled, source.transparencyPolicy,
            TransparencyTopology::Unknown, result);
        const bool errors = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const MaterialCompileDiagnostic& diagnostic) {
                return diagnostic.severity == MaterialCompileSeverity::Error;
            });
        if (!errors) {
            compiled.contentHash = compiledHash(compiled);
            result.material = std::make_shared<const CompiledMaterial>(std::move(compiled));
        }
        return result;
    }

    MaterialCompileDocumentResult compileSourceMaterialDocument(
        const SourceMaterialDocument& document, MaterialCompilePolicy policy) {
        MaterialCompileDocumentResult result{};
        for (const SourceMaterialDiagnostic& diagnostic : document.diagnostics()) {
            if (!diagnostic.path.starts_with("/materials/")) {
                const MaterialCompileSeverity severity = diagnostic.severity == SourceDiagnosticSeverity::Error
                    ? MaterialCompileSeverity::Error : diagnostic.severity == SourceDiagnosticSeverity::Warning
                        ? MaterialCompileSeverity::Warning : MaterialCompileSeverity::Info;
                result.diagnostics.push_back({ severity, "SOURCE_" + diagnostic.code,
                    diagnostic.path + ": " + diagnostic.message });
            }
        }
        result.materials.reserve(document.materials().size());
        for (const SourceMaterial& source : document.materials()) {
            const std::string prefix = "/materials/" + std::to_string(source.localIndex);
            std::vector<SourceMaterialDiagnostic> diagnostics;
            for (const SourceMaterialDiagnostic& diagnostic : document.diagnostics())
                if (diagnostic.path == prefix || diagnostic.path.starts_with(prefix + "/"))
                    diagnostics.push_back(diagnostic);
            result.materials.push_back(compileSourceMaterial(source, policy, diagnostics));
        }
        return result;
    }

    MaterialCompileResult applyCompiledTransparencyPolicy(
        const CompiledMaterial& material,
        const TransparencyPolicyV1& policy,
        TransparencyTopology topology) {
        MaterialCompileResult result;
        if (material.schemaVersion != CompiledMaterial::SchemaVersion ||
            material.closureClass == MaterialClosureClass::Invalid) {
            addDiagnostic(result, MaterialCompileSeverity::Error,
                "MATERIAL_TRANSPARENCY_BASE_INVALID",
                "transparency policy cannot be applied to an invalid compiled material");
            return result;
        }
        CompiledMaterial updated = material;
        resolveTransparencyPolicy(updated, policy, topology, result);
        updated.contentHash = calculateCompiledMaterialHash(updated);
        result.material = std::make_shared<const CompiledMaterial>(
            std::move(updated));
        return result;
    }

    std::string calculateCompiledMaterialHash(
        const CompiledMaterial& material) {
        return compiledHash(material);
    }

    CanonicalMaterialSurface evaluateMaterialSurface(const CompiledMaterial& material,
        const MaterialSurfaceInputs& inputs) {
        if (material.closureClass == MaterialClosureClass::Invalid)
            throw std::invalid_argument("cannot evaluate an invalid compiled material");
        validateEvaluationInputs(inputs);
        const StandardClosureRecipe& recipe = material.standard;
        const auto sampled = [&](SourceTextureSemantic semantic) -> glm::vec4 {
            return (recipe.textureMask & textureBit(semantic)) != 0
                ? inputs.textures.sample(semantic) : glm::vec4(1.0f);
        };

        CanonicalMaterialSurface surface{};
        surface.flags = material.featureFlags;
        glm::vec3 base{};
        if (material.workflow == MaterialWorkflow::SpecularGlossiness) {
            const glm::vec4 diffuseSample = sampled(SourceTextureSemantic::Diffuse);
            const glm::vec4 specGlossSample = sampled(SourceTextureSemantic::SpecularGlossiness);
            const glm::vec3 diffuse = glm::vec3(recipe.diffuseFactor) * decodeSrgb(glm::vec3(diffuseSample)) *
                glm::vec3(inputs.vertexColor);
            const glm::vec3 specular = recipe.specularGlossinessFactor * decodeSrgb(glm::vec3(specGlossSample));
            surface.diffuseAlbedo = toAcesCg(diffuse * (1.0f - std::max({ specular.r, specular.g, specular.b })));
            surface.f0 = toAcesCg(specular);
            surface.f90 = 1.0f;
            surface.perceptualRoughness = 1.0f - recipe.glossinessFactor * specGlossSample.a;
            surface.alpha = recipe.diffuseFactor.a * diffuseSample.a * inputs.vertexColor.a;
        }
        else {
            const glm::vec4 baseSample = sampled(SourceTextureSemantic::BaseColor);
            base = glm::vec3(recipe.baseColorFactor) * decodeSrgb(glm::vec3(baseSample)) *
                glm::vec3(inputs.vertexColor);
            surface.alpha = recipe.baseColorFactor.a * baseSample.a * inputs.vertexColor.a;
            if (material.workflow == MaterialWorkflow::Unlit) {
                surface.unlitRadiance = toAcesCg(base);
                surface.diffuseAlbedo = surface.unlitRadiance;
                surface.f0 = glm::vec3(0.0f);
                surface.f90 = 0.0f;
                surface.perceptualRoughness = 1.0f;
            }
            else {
                const glm::vec4 mrSample = sampled(SourceTextureSemantic::MetallicRoughness);
                const float metallic = recipe.metallicFactor * mrSample.b;
                surface.perceptualRoughness = recipe.roughnessFactor * mrSample.g;
                const float iorF0 = std::pow((recipe.ior - 1.0f) / (recipe.ior + 1.0f), 2.0f);
                const glm::vec4 specularSample = sampled(SourceTextureSemantic::Specular);
                const glm::vec4 specularColorSample = sampled(SourceTextureSemantic::SpecularColor);
                const float specularWeight = recipe.specularFactor * specularSample.a;
                const glm::vec3 dielectricF0 = glm::min(glm::vec3(iorF0) *
                    recipe.specularColorFactor * decodeSrgb(glm::vec3(specularColorSample)),
                    glm::vec3(1.0f)) * specularWeight;
                surface.diffuseAlbedo = toAcesCg(base * (1.0f - metallic));
                surface.f0 = toAcesCg(glm::mix(dielectricF0, base, metallic));
                surface.f90 = glm::mix(specularWeight, 1.0f, metallic);
            }
        }

        if (material.workflow != MaterialWorkflow::Unlit) {
            const glm::vec4 aoSample = sampled(SourceTextureSemantic::Occlusion);
            surface.ao = glm::mix(1.0f, aoSample.r, recipe.occlusionStrength);
            const glm::vec4 emissiveSample = sampled(SourceTextureSemantic::Emissive);
            surface.emissive = toAcesCg(recipe.emissiveFactor * recipe.emissiveStrength *
                decodeSrgb(glm::vec3(emissiveSample)));
        }
        const StandardTangentFrame tangentFrame = buildStandardTangentFrame(
            inputs.geometricNormal, inputs.tangent, inputs.tangentHandedness,
            recipe.doubleSided, inputs.frontFacing);
        if ((recipe.textureMask & textureBit(SourceTextureSemantic::Normal)) != 0) {
            const glm::vec3 encoded = glm::vec3(sampled(SourceTextureSemantic::Normal));
            surface.shadingNormal = applyStandardTangentNormal(tangentFrame,
                encoded, recipe.normalScale);
        }
        else surface.shadingNormal = tangentFrame.normal;
        surface.alphaPasses = recipe.alphaMode != SourceAlphaMode::Mask || surface.alpha >= recipe.alphaCutoff;
        return surface;
    }

    const char* materialWorkflowName(MaterialWorkflow workflow) noexcept {
        switch (workflow) {
        case MaterialWorkflow::MetallicRoughness: return "metallic-roughness";
        case MaterialWorkflow::SpecularGlossiness: return "specular-glossiness";
        case MaterialWorkflow::Unlit: return "unlit";
        }
        return "unknown";
    }
    const char* materialClosureClassName(MaterialClosureClass closureClass) noexcept {
        switch (closureClass) {
        case MaterialClosureClass::StandardDeferred: return "standard-deferred";
        case MaterialClosureClass::StandardForward: return "standard-forward";
        case MaterialClosureClass::ComplexForward: return "complex-forward";
        case MaterialClosureClass::Unlit: return "unlit";
        case MaterialClosureClass::Invalid: return "invalid";
        }
        return "invalid";
    }
    const char* complexLobeTypeName(ComplexLobeType type) noexcept {
        switch (type) {
        case ComplexLobeType::Clearcoat: return "clearcoat";
        case ComplexLobeType::Sheen: return "sheen";
        case ComplexLobeType::Anisotropy: return "anisotropy";
        case ComplexLobeType::Iridescence: return "iridescence";
        case ComplexLobeType::ThinTransmission: return "thin-transmission";
        case ComplexLobeType::VolumeTransmission: return "volume-transmission";
        case ComplexLobeType::Dispersion: return "dispersion";
        case ComplexLobeType::DiffuseTransmission: return "diffuse-transmission";
        }
        return "unknown";
    }

} // namespace Iridium
