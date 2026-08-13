#include "material/MaterialRuntime.h"

#include "renderer/color/SceneColor.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Iridium {

    namespace {

        using Json = nlohmann::ordered_json;

        constexpr size_t MaxInstanceCapacity =
            static_cast<size_t>(MaterialInstanceHandle::MaxIndex) + 1;

        bool finite(glm::vec3 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool finite(glm::vec4 value) noexcept {
            return finite(glm::vec3(value)) && std::isfinite(value.w);
        }

        bool unit(float value) noexcept {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        }

        bool unit(glm::vec3 value) noexcept {
            return finite(value) && glm::all(glm::greaterThanEqual(value, glm::vec3(0.0f))) &&
                glm::all(glm::lessThanEqual(value, glm::vec3(1.0f)));
        }

        bool unit(glm::vec4 value) noexcept {
            return finite(value) && glm::all(glm::greaterThanEqual(value, glm::vec4(0.0f))) &&
                glm::all(glm::lessThanEqual(value, glm::vec4(1.0f)));
        }

        bool nonzero(glm::vec3 value, float strength) noexcept {
            return strength > 0.0f && glm::any(glm::greaterThan(value, glm::vec3(0.0f)));
        }

        uint32_t semanticIndex(SourceTextureSemantic semantic) noexcept {
            return static_cast<uint32_t>(semantic);
        }

        bool hasTexture(const CompiledMaterial& material, SourceTextureSemantic semantic) noexcept {
            return (material.standard.textureMask & (1u << semanticIndex(semantic))) != 0;
        }

        bool validTextureHandle(TextureHandle handle,
            const MaterialPackingContext& context) noexcept {
            return handle.isValid() && handle.getIndex() < context.textureGenerations.size() &&
                context.textureGenerations[handle.getIndex()] == handle.getGeneration();
        }

        bool validSamplerHandle(SamplerHandle handle,
            const MaterialPackingContext& context) noexcept {
            return handle.isValid() && handle.getIndex() < context.samplerGenerations.size() &&
                context.samplerGenerations[handle.getIndex()] == handle.getGeneration();
        }

        void packError(MaterialPackResult& result, MaterialPackError error,
            std::string code, std::string message) {
            result.diagnostics.push_back({ error, std::move(code), std::move(message) });
        }

        bool packHalf(float value, uint16_t& destination, MaterialPackResult& result,
            std::string_view field) {
            if (!std::isfinite(value) || std::abs(value) > 65504.0f) {
                packError(result, MaterialPackError::HalfOverflow, "MATERIAL_PACK_HALF_RANGE",
                    std::string(field) + " is not representable as finite FP16");
                return false;
            }
            destination = floatToHalf(value);
            return true;
        }

        const char* overrideFieldName(MaterialOverrideField field) noexcept {
            switch (field) {
            case MaterialOverrideBaseColor: return "baseColor";
            case MaterialOverrideMetallic: return "metallic";
            case MaterialOverrideRoughness: return "roughness";
            case MaterialOverrideIor: return "ior";
            case MaterialOverrideSpecular: return "specular";
            case MaterialOverrideDiffuse: return "diffuse";
            case MaterialOverrideSpecularGlossiness: return "specularGlossiness";
            case MaterialOverrideEmissive: return "emissive";
            case MaterialOverrideNormalScale: return "normalScale";
            case MaterialOverrideOcclusionStrength: return "occlusionStrength";
            case MaterialOverrideAlphaCutoff: return "alphaCutoff";
            case MaterialOverrideTexture: return "texture";
            default: return "none";
            }
        }

        const char* compileSeverityName(MaterialCompileSeverity severity) noexcept {
            switch (severity) {
            case MaterialCompileSeverity::Info: return "info";
            case MaterialCompileSeverity::Warning: return "warning";
            case MaterialCompileSeverity::Error: return "error";
            }
            return "unknown";
        }

        Json vec3(glm::vec3 value) { return Json::array({ value.x, value.y, value.z }); }
        Json vec4(glm::vec4 value) { return Json::array({ value.x, value.y, value.z, value.w }); }

    } // namespace

    MaterialInstance::MaterialInstance(std::shared_ptr<const CompiledMaterial> compiled,
        std::span<const MaterialTextureBinding> textureBindings)
        : m_compiled(std::move(compiled)) {
        if (!m_compiled) throw std::invalid_argument("material instance requires a compiled material");
        if (textureBindings.size() != m_compiled->textureOperations.size())
            throw std::invalid_argument("texture binding count must match compiled texture operations");
        m_values = m_compiled->standard;
        for (size_t index = 0; index < textureBindings.size(); ++index) {
            if (!textureBindings[index].texture.isValid() || !textureBindings[index].sampler.isValid())
                throw std::invalid_argument("material texture and sampler bindings must be valid handles");
            const uint32_t semantic = semanticIndex(m_compiled->textureOperations[index].semantic);
            m_textureBindings[semantic] = textureBindings[index];
        }
    }

    const CompiledMaterial& MaterialInstance::compiled() const noexcept { return *m_compiled; }
    const std::shared_ptr<const CompiledMaterial>& MaterialInstance::compiledAsset() const noexcept { return m_compiled; }
    const StandardClosureRecipe& MaterialInstance::values() const noexcept { return m_values; }
    std::span<const MaterialTextureBinding, 21> MaterialInstance::textureBindings() const noexcept { return m_textureBindings; }
    uint64_t MaterialInstance::revision() const noexcept { return m_revision; }
    uint64_t MaterialInstance::overrideMask() const noexcept { return m_overrideMask; }
    bool MaterialInstance::isOverridden(MaterialOverrideField field) const noexcept {
        return (m_overrideMask & static_cast<uint64_t>(field)) != 0;
    }

    template <typename T>
    MaterialOverrideStatus MaterialInstance::applyValue(T& destination, const T& value,
        MaterialOverrideField field) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (std::memcmp(&destination, &value, sizeof(T)) == 0)
            return MaterialOverrideStatus::Unchanged;
        destination = value;
        m_overrideMask |= static_cast<uint64_t>(field);
        ++m_revision;
        if (m_revision == 0) m_revision = 1;
        return MaterialOverrideStatus::Applied;
    }

    MaterialOverrideStatus MaterialInstance::setBaseColor(glm::vec4 value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow == MaterialWorkflow::SpecularGlossiness)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.baseColorFactor, value, MaterialOverrideBaseColor);
    }

    MaterialOverrideStatus MaterialInstance::setMetallic(float value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::MetallicRoughness)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.metallicFactor, value, MaterialOverrideMetallic);
    }

    MaterialOverrideStatus MaterialInstance::setRoughness(float value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::MetallicRoughness)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.roughnessFactor, value, MaterialOverrideRoughness);
    }

    MaterialOverrideStatus MaterialInstance::setIor(float value) noexcept {
        if (!std::isfinite(value) || value < 1.0f) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::MetallicRoughness)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.ior, value, MaterialOverrideIor);
    }

    MaterialOverrideStatus MaterialInstance::setSpecular(float factor, glm::vec3 color) noexcept {
        if (!unit(factor) || !unit(color)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::MetallicRoughness)
            return MaterialOverrideStatus::RequiresRecompile;
        if (m_values.specularFactor == factor && m_values.specularColorFactor == color)
            return MaterialOverrideStatus::Unchanged;
        m_values.specularFactor = factor;
        m_values.specularColorFactor = color;
        m_overrideMask |= MaterialOverrideSpecular;
        ++m_revision;
        return MaterialOverrideStatus::Applied;
    }

    MaterialOverrideStatus MaterialInstance::setDiffuseFactor(glm::vec4 value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::SpecularGlossiness)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.diffuseFactor, value, MaterialOverrideDiffuse);
    }

    MaterialOverrideStatus MaterialInstance::setSpecularGlossiness(glm::vec3 factor,
        float glossiness) noexcept {
        if (!unit(factor) || !unit(glossiness)) return MaterialOverrideStatus::InvalidValue;
        if (m_compiled->workflow != MaterialWorkflow::SpecularGlossiness)
            return MaterialOverrideStatus::RequiresRecompile;
        if (m_values.specularGlossinessFactor == factor && m_values.glossinessFactor == glossiness)
            return MaterialOverrideStatus::Unchanged;
        m_values.specularGlossinessFactor = factor;
        m_values.glossinessFactor = glossiness;
        m_overrideMask |= MaterialOverrideSpecularGlossiness;
        ++m_revision;
        return MaterialOverrideStatus::Applied;
    }

    MaterialOverrideStatus MaterialInstance::setEmissive(glm::vec3 factor,
        float strength) noexcept {
        if (!unit(factor) || !std::isfinite(strength) || strength < 0.0f)
            return MaterialOverrideStatus::InvalidValue;
        const bool textureKeepsFeature = hasTexture(*m_compiled, SourceTextureSemantic::Emissive);
        const bool originallyActive = (m_compiled->featureFlags & MaterialFeatureEmissive) != 0;
        const bool newlyActive = textureKeepsFeature || nonzero(factor, strength);
        if (originallyActive != newlyActive) return MaterialOverrideStatus::RequiresRecompile;
        if (m_values.emissiveFactor == factor && m_values.emissiveStrength == strength)
            return MaterialOverrideStatus::Unchanged;
        m_values.emissiveFactor = factor;
        m_values.emissiveStrength = strength;
        m_overrideMask |= MaterialOverrideEmissive;
        ++m_revision;
        return MaterialOverrideStatus::Applied;
    }

    MaterialOverrideStatus MaterialInstance::setNormalScale(float value) noexcept {
        if (!std::isfinite(value)) return MaterialOverrideStatus::InvalidValue;
        if (!hasTexture(*m_compiled, SourceTextureSemantic::Normal))
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.normalScale, value, MaterialOverrideNormalScale);
    }

    MaterialOverrideStatus MaterialInstance::setOcclusionStrength(float value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (!hasTexture(*m_compiled, SourceTextureSemantic::Occlusion))
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.occlusionStrength, value, MaterialOverrideOcclusionStrength);
    }

    MaterialOverrideStatus MaterialInstance::setAlphaCutoff(float value) noexcept {
        if (!unit(value)) return MaterialOverrideStatus::InvalidValue;
        if (m_values.alphaMode != SourceAlphaMode::Mask)
            return MaterialOverrideStatus::RequiresRecompile;
        return applyValue(m_values.alphaCutoff, value, MaterialOverrideAlphaCutoff);
    }

    MaterialOverrideStatus MaterialInstance::setTextureBinding(SourceTextureSemantic semantic,
        std::optional<MaterialTextureBinding> binding) noexcept {
        const uint32_t index = semanticIndex(semantic);
        if (!hasTexture(*m_compiled, semantic) || !binding.has_value())
            return MaterialOverrideStatus::RequiresRecompile;
        if (!binding->texture.isValid() || !binding->sampler.isValid())
            return MaterialOverrideStatus::InvalidValue;
        return applyValue(m_textureBindings[index], *binding, MaterialOverrideTexture);
    }

    MaterialOverrideStatus MaterialInstance::setAlphaMode(SourceAlphaMode value) noexcept {
        return value == m_values.alphaMode ? MaterialOverrideStatus::Unchanged
            : MaterialOverrideStatus::RequiresRecompile;
    }

    MaterialOverrideStatus MaterialInstance::setDoubleSided(bool value) noexcept {
        return value == m_values.doubleSided ? MaterialOverrideStatus::Unchanged
            : MaterialOverrideStatus::RequiresRecompile;
    }

    uint16_t floatToHalf(float value) noexcept {
        const uint32_t bits = std::bit_cast<uint32_t>(value);
        const uint32_t sign = (bits >> 16u) & 0x8000u;
        const uint32_t exponent = (bits >> 23u) & 0xffu;
        uint32_t mantissa = bits & 0x7fffffu;
        if (exponent == 0xffu) {
            if (mantissa == 0) return static_cast<uint16_t>(sign | 0x7c00u);
            return static_cast<uint16_t>(sign | 0x7e00u | (mantissa >> 13u));
        }
        int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
        if (halfExponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
        if (halfExponent <= 0) {
            if (halfExponent < -10) return static_cast<uint16_t>(sign);
            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
            uint32_t halfMantissa = mantissa >> shift;
            const uint32_t remainder = mantissa & ((1u << shift) - 1u);
            const uint32_t halfway = 1u << (shift - 1u);
            if (remainder > halfway || (remainder == halfway && (halfMantissa & 1u)))
                ++halfMantissa;
            return static_cast<uint16_t>(sign | halfMantissa);
        }
        uint32_t halfMantissa = mantissa >> 13u;
        const uint32_t remainder = mantissa & 0x1fffu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (halfMantissa & 1u))) {
            ++halfMantissa;
            if (halfMantissa == 0x400u) {
                halfMantissa = 0;
                ++halfExponent;
                if (halfExponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
            }
        }
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10u) |
            halfMantissa);
    }

    bool MaterialPackResult::succeeded() const noexcept {
        return material.has_value() && diagnostics.empty();
    }

    uint32_t packedTextureReconstructNormalZMask(
        const PackedGpuMaterial& material) noexcept {
        uint32_t mask = 0;
        std::memcpy(&mask, material.reserved.data(),
            sizeof(mask));
        return mask;
    }

    bool consumeMaterialUploadRevision(uint64_t revision,
        uint64_t& uploadedRevision) noexcept {
        if (revision == 0 || revision == uploadedRevision) return false;
        uploadedRevision = revision;
        return true;
    }

    MaterialPackResult packMaterialInstance(const MaterialInstance& instance,
        const MaterialPackingContext& context) {
        MaterialPackResult result{};
        PackedGpuMaterial packed{};
        packed.textureIndices.fill(PackedGpuMaterial::InvalidTextureIndex);
        const CompiledMaterial& compiled = instance.compiled();
        const StandardClosureRecipe& values = instance.values();
        packed.closureClass = static_cast<uint32_t>(compiled.closureClass);
        packed.workflow = static_cast<uint32_t>(compiled.workflow);
        packed.featureFlags = compiled.featureFlags;
        packed.textureMask = values.textureMask;
        packed.alphaMode = static_cast<uint32_t>(values.alphaMode);
        packed.doubleSided = values.doubleSided ? 1u : 0u;
        packed.baseColorFactor = { values.baseColorFactor.r, values.baseColorFactor.g,
            values.baseColorFactor.b, values.baseColorFactor.a };
        packed.metallicRoughnessIorSpecular = { values.metallicFactor,
            values.roughnessFactor, values.ior, values.specularFactor };
        packed.specularColorNormalScale = { values.specularColorFactor.r,
            values.specularColorFactor.g, values.specularColorFactor.b, values.normalScale };
        packed.diffuseFactor = { values.diffuseFactor.r, values.diffuseFactor.g,
            values.diffuseFactor.b, values.diffuseFactor.a };
        packed.specularGlossinessFactorGloss = { values.specularGlossinessFactor.r,
            values.specularGlossinessFactor.g, values.specularGlossinessFactor.b,
            values.glossinessFactor };
        packed.emissiveFactorStrength = { values.emissiveFactor.r, values.emissiveFactor.g,
            values.emissiveFactor.b, values.emissiveStrength };
        packed.surfaceParameters = { values.occlusionStrength, values.alphaCutoff, 0.0f, 0.0f };

        uint32_t reconstructNormalZMask = 0;
        for (const CompiledTextureOperation& operation : compiled.textureOperations) {
            const uint32_t semantic = semanticIndex(operation.semantic);
            const MaterialTextureBinding binding = instance.textureBindings()[semantic];
            if (!validTextureHandle(binding.texture, context)) {
                packError(result, MaterialPackError::StaleTextureHandle,
                    "MATERIAL_PACK_STALE_TEXTURE", std::string(sourceTextureSemanticName(
                        operation.semantic)) + " binding is stale or absent from the texture table");
                continue;
            }
            if (!validSamplerHandle(binding.sampler, context)) {
                packError(result, MaterialPackError::StaleSamplerHandle,
                    "MATERIAL_PACK_STALE_SAMPLER", std::string(sourceTextureSemanticName(
                        operation.semantic)) + " sampler binding is stale or absent from the sampler table");
                continue;
            }
            packed.textureIndices[semantic] = binding.texture.getIndex();
            if (binding.reconstructNormalZ) {
                reconstructNormalZMask |= 1u << semantic;
            }
            PackedGpuTextureUse& use = packed.textureUses[semantic];
            use.semantic = static_cast<uint8_t>(semantic);
            const uint32_t texCoord = operation.transform.texCoordOverride.value_or(operation.texCoord);
            if (texCoord > std::numeric_limits<uint8_t>::max())
                packError(result, MaterialPackError::TexCoordOverflow,
                    "MATERIAL_PACK_TEXCOORD_RANGE", "texture coordinate index exceeds 8-bit packing");
            else use.texCoord = static_cast<uint8_t>(texCoord);
            if (binding.sampler.getIndex() >= 0xffffu)
                packError(result, MaterialPackError::SamplerIndexOverflow,
                    "MATERIAL_PACK_SAMPLER_RANGE", "sampler index exceeds 16-bit packing");
            else use.samplerIndex = static_cast<uint16_t>(binding.sampler.getIndex());
            packHalf(operation.transform.offset.value.x, use.offsetX, result, "texture offset X");
            packHalf(operation.transform.offset.value.y, use.offsetY, result, "texture offset Y");
            packHalf(operation.transform.scale.value.x, use.scaleX, result, "texture scale X");
            packHalf(operation.transform.scale.value.y, use.scaleY, result, "texture scale Y");
            packHalf(operation.transform.rotation.value, use.rotation, result, "texture rotation");
            packHalf(operation.scalar.value, use.scalar, result, "texture scalar");
        }
        std::memcpy(packed.reserved.data(),
            &reconstructNormalZMask,
            sizeof(reconstructNormalZMask));

        if (compiled.complexLobes.size() > PackedGpuMaterial::MaxComplexLobes) {
            packError(result, MaterialPackError::TooManyComplexLobes,
                "MATERIAL_PACK_COMPLEX_CAPACITY", "compiled material exceeds packed complex-lobe capacity");
        }
        else {
            packed.complexLobeCount = static_cast<uint32_t>(compiled.complexLobes.size());
            for (size_t index = 0; index < compiled.complexLobes.size(); ++index) {
                const ComplexLobeRecord& source = compiled.complexLobes[index];
                PackedGpuComplexLobe& destination = packed.complexLobes[index];
                destination.type = static_cast<uint32_t>(source.type);
                std::visit([&destination](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, ClearcoatLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.factor, data.roughnessFactor,
                            data.normalScale, 0.0f, 0.0f, 0.0f };
                    }
                    else if constexpr (std::is_same_v<T, SheenLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.color.r, data.color.g, data.color.b,
                            data.roughnessFactor, 0.0f, 0.0f };
                    }
                    else if constexpr (std::is_same_v<T, AnisotropyLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.strength, data.rotation, 0.0f, 0.0f, 0.0f, 0.0f };
                    }
                    else if constexpr (std::is_same_v<T, IridescenceLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.factor, data.ior,
                            data.thicknessMinimumNm, data.thicknessMaximumNm, 0.0f, 0.0f };
                    }
                    else if constexpr (std::is_same_v<T, ThinTransmissionLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.factor, data.ior, data.specularFactor,
                            data.specularColor.r, data.specularColor.g, data.specularColor.b };
                    }
                    else if constexpr (std::is_same_v<T, VolumeTransmissionLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.thicknessFactor, data.attenuationDistance,
                            data.attenuationColor.r, data.attenuationColor.g,
                            data.attenuationColor.b, 0.0f };
                    }
                    else if constexpr (std::is_same_v<T, DispersionLobe>)
                        destination.parameters[0] = data.dispersion;
                    else if constexpr (std::is_same_v<T, DiffuseTransmissionLobe>) {
                        destination.textureMask = data.textureMask;
                        destination.parameters = { data.factor, data.color.r, data.color.g,
                            data.color.b, 0.0f, 0.0f };
                    }
                }, source.data);
            }
        }

        if (result.diagnostics.empty()) result.material = packed;
        return result;
    }

    UnpackedGpuMaterial unpackGpuMaterial(const PackedGpuMaterial& material) {
        if (material.schemaVersion != PackedGpuMaterial::SchemaVersion)
            throw std::invalid_argument("unsupported packed material schema version");
        UnpackedGpuMaterial result{};
        result.closureClass = static_cast<MaterialClosureClass>(material.closureClass);
        result.workflow = static_cast<MaterialWorkflow>(material.workflow);
        result.featureFlags = material.featureFlags;
        result.values.textureMask = material.textureMask;
        result.values.alphaMode = static_cast<SourceAlphaMode>(material.alphaMode);
        result.values.doubleSided = material.doubleSided != 0;
        result.values.baseColorFactor = glm::make_vec4(material.baseColorFactor.data());
        result.values.metallicFactor = material.metallicRoughnessIorSpecular[0];
        result.values.roughnessFactor = material.metallicRoughnessIorSpecular[1];
        result.values.ior = material.metallicRoughnessIorSpecular[2];
        result.values.specularFactor = material.metallicRoughnessIorSpecular[3];
        result.values.specularColorFactor = glm::make_vec3(material.specularColorNormalScale.data());
        result.values.normalScale = material.specularColorNormalScale[3];
        result.values.diffuseFactor = glm::make_vec4(material.diffuseFactor.data());
        result.values.specularGlossinessFactor = glm::make_vec3(
            material.specularGlossinessFactorGloss.data());
        result.values.glossinessFactor = material.specularGlossinessFactorGloss[3];
        result.values.emissiveFactor = glm::make_vec3(material.emissiveFactorStrength.data());
        result.values.emissiveStrength = material.emissiveFactorStrength[3];
        result.values.occlusionStrength = material.surfaceParameters[0];
        result.values.alphaCutoff = material.surfaceParameters[1];
        result.textureIndices = material.textureIndices;
        for (size_t index = 0; index < material.textureUses.size(); ++index) {
            const PackedGpuTextureUse& source = material.textureUses[index];
            result.textureUses[index] = {
                { Color::halfToFloat(source.offsetX), Color::halfToFloat(source.offsetY) },
                { Color::halfToFloat(source.scaleX), Color::halfToFloat(source.scaleY) },
                Color::halfToFloat(source.rotation), Color::halfToFloat(source.scalar),
                source.samplerIndex, source.texCoord,
                static_cast<SourceTextureSemantic>(source.semantic),
            };
        }
        result.complexLobeCount = material.complexLobeCount;
        result.complexLobes = material.complexLobes;
        return result;
    }

    MaterialInstanceStore::MaterialInstanceStore(size_t reserveSize) {
        if (reserveSize > MaxInstanceCapacity)
            throw std::invalid_argument("material instance store exceeds handle capacity");
        m_slots.resize(reserveSize);
        for (size_t index = reserveSize; index-- > 0;)
            m_freeIndices.push_back(static_cast<uint32_t>(index));
    }

    MaterialInstanceHandle MaterialInstanceStore::create(
        std::shared_ptr<const CompiledMaterial> compiled,
        std::span<const MaterialTextureBinding> textureBindings) {
        if (m_freeIndices.empty()) {
            const size_t oldSize = m_slots.size();
            if (oldSize >= MaxInstanceCapacity)
                throw std::overflow_error("material instance handle capacity exhausted");
            const size_t newSize = std::min(MaxInstanceCapacity, oldSize == 0 ? size_t{ 1 } : oldSize * 2);
            m_slots.resize(newSize);
            for (size_t index = newSize; index-- > oldSize;)
                m_freeIndices.push_back(static_cast<uint32_t>(index));
        }
        const uint32_t index = m_freeIndices.back();
        Slot& slot = m_slots[index];
        slot.instance.emplace(std::move(compiled), textureBindings);
        m_freeIndices.pop_back();
        slot.uploadedRevision = 0;
        return MaterialInstanceHandle::fromParts(index, slot.generation);
    }

    void MaterialInstanceStore::destroy(MaterialInstanceHandle handle) noexcept {
        if (!handle.isValid() || handle.getIndex() >= m_slots.size()) return;
        Slot& slot = m_slots[handle.getIndex()];
        if (!slot.instance || slot.generation != handle.getGeneration()) return;
        slot.instance.reset();
        slot.uploadedRevision = 0;
        slot.generation = slot.generation == MaterialInstanceHandle::MaxGeneration
            ? 1 : slot.generation + 1;
        m_freeIndices.push_back(handle.getIndex());
    }

    MaterialInstance* MaterialInstanceStore::get(MaterialInstanceHandle handle) noexcept {
        if (!handle.isValid() || handle.getIndex() >= m_slots.size()) return nullptr;
        Slot& slot = m_slots[handle.getIndex()];
        return slot.instance && slot.generation == handle.getGeneration() ? &*slot.instance : nullptr;
    }

    const MaterialInstance* MaterialInstanceStore::get(MaterialInstanceHandle handle) const noexcept {
        if (!handle.isValid() || handle.getIndex() >= m_slots.size()) return nullptr;
        const Slot& slot = m_slots[handle.getIndex()];
        return slot.instance && slot.generation == handle.getGeneration() ? &*slot.instance : nullptr;
    }

    size_t MaterialInstanceStore::activeCount() const noexcept {
        return m_slots.size() - m_freeIndices.size();
    }

    MaterialUploadStats MaterialInstanceStore::collectChanged(
        const MaterialPackingContext& context, std::vector<MaterialUpload>& output) {
        output.clear();
        MaterialUploadStats stats{};
        for (size_t index = 0; index < m_slots.size(); ++index) {
            Slot& slot = m_slots[index];
            if (!slot.instance) continue;
            ++stats.activeInstances;
            if (slot.uploadedRevision == slot.instance->revision()) continue;
            MaterialPackResult packed = packMaterialInstance(*slot.instance, context);
            if (!packed.succeeded()) {
                ++stats.failedInstances;
                continue;
            }
            const MaterialInstanceHandle handle = MaterialInstanceHandle::fromParts(
                static_cast<uint32_t>(index), slot.generation);
            output.push_back({ handle, *packed.material });
            slot.uploadedRevision = slot.instance->revision();
            ++stats.changedInstances;
            stats.uploadedBytes += sizeof(PackedGpuMaterial);
        }
        return stats;
    }

    MaterialDiagnosticSnapshot buildMaterialDiagnosticSnapshot(
        const SourceMaterial& source, const MaterialCompileResult& compileResult,
        const MaterialInstance& instance, const MaterialPackResult& packResult) {
        Json root;
        root["schema_version"] = 1;
        root["source"] = {
            { "material_index", source.localIndex },
            { "name", source.name },
            { "base_color", vec4(source.metallicRoughness.baseColorFactor.value) },
            { "base_color_origin", sourceValueOriginName(source.metallicRoughness.baseColorFactor.origin) },
            { "metallic", source.metallicRoughness.metallicFactor.value },
            { "metallic_origin", sourceValueOriginName(source.metallicRoughness.metallicFactor.origin) },
            { "roughness", source.metallicRoughness.roughnessFactor.value },
            { "roughness_origin", sourceValueOriginName(source.metallicRoughness.roughnessFactor.origin) },
            { "emissive", vec3(source.emissiveFactor.value) },
            { "emissive_origin", sourceValueOriginName(source.emissiveFactor.origin) },
            { "emissive_strength", source.emissiveStrength.value },
            { "emissive_strength_origin", sourceValueOriginName(source.emissiveStrength.origin) },
            { "normal_scale", source.normalScale.value },
            { "normal_scale_origin", sourceValueOriginName(source.normalScale.origin) },
            { "occlusion_strength", source.occlusionStrength.value },
            { "occlusion_strength_origin", sourceValueOriginName(source.occlusionStrength.origin) },
            { "alpha_mode", sourceAlphaModeName(source.alphaMode.value) },
            { "alpha_mode_origin", sourceValueOriginName(source.alphaMode.origin) },
            { "alpha_cutoff", source.alphaCutoff.value },
            { "alpha_cutoff_origin", sourceValueOriginName(source.alphaCutoff.origin) },
            { "double_sided", source.doubleSided.value },
            { "double_sided_origin", sourceValueOriginName(source.doubleSided.origin) },
        };
        root["source"]["textures"] = Json::array();
        for (const SourceTextureUse& texture : source.textures) {
            root["source"]["textures"].push_back({
                { "semantic", sourceTextureSemanticName(texture.semantic) },
                { "source_texture_index", texture.textureIndex },
                { "source_image_index", texture.imageIndex ? Json(*texture.imageIndex) : Json(nullptr) },
                { "image", texture.imageIdentity },
                { "channels", texture.channels },
                { "transfer", texture.transfer == SourceTextureTransfer::Srgb ? "sRGB" : "linear" },
                { "uv", texture.transform.texCoordOverride.value_or(texture.texCoord.value) },
                { "uv_origin", sourceValueOriginName(texture.texCoord.origin) },
                { "offset", { texture.transform.offset.value.x, texture.transform.offset.value.y } },
                { "scale", { texture.transform.scale.value.x, texture.transform.scale.value.y } },
                { "rotation", texture.transform.rotation.value },
                { "scalar", texture.scalar.value },
                { "sampler_source_index", texture.sampler.sourceIndex
                    ? Json(*texture.sampler.sourceIndex) : Json(nullptr) },
                { "mag_filter", texture.sampler.magFilter.value
                    ? Json(*texture.sampler.magFilter.value) : Json(nullptr) },
                { "min_filter", texture.sampler.minFilter.value
                    ? Json(*texture.sampler.minFilter.value) : Json(nullptr) },
                { "wrap_s", texture.sampler.wrapS.value },
                { "wrap_t", texture.sampler.wrapT.value },
            });
        }
        root["source"]["extensions"] = Json::array();
        for (const SourceMaterialExtension& extension : source.extensions) {
            Json record = {
                { "name", extension.name }, { "required", extension.required },
                { "supported_by_m2", extension.supportedByM2 },
                { "canonical_values", extension.canonicalValues },
                { "properties", Json::array() },
            };
            for (const SourceExtensionProperty& property : extension.properties)
                record["properties"].push_back({
                    { "name", property.name }, { "value", property.canonicalValue },
                    { "origin", sourceValueOriginName(property.origin) },
                });
            root["source"]["extensions"].push_back(std::move(record));
        }
        root["compiled"] = nullptr;
        if (compileResult.material) {
            const CompiledMaterial& compiled = *compileResult.material;
            root["compiled"] = {
                { "schema_version", compiled.schemaVersion },
                { "hash", compiled.contentHash },
                { "workflow", materialWorkflowName(compiled.workflow) },
                { "closure", materialClosureClassName(compiled.closureClass) },
                { "feature_flags", compiled.featureFlags },
                { "base_color", vec4(compiled.standard.baseColorFactor) },
                { "metallic", compiled.standard.metallicFactor },
                { "roughness", compiled.standard.roughnessFactor },
                { "ior", compiled.standard.ior },
                { "specular_factor", compiled.standard.specularFactor },
                { "specular_color", vec3(compiled.standard.specularColorFactor) },
                { "emissive", vec3(compiled.standard.emissiveFactor) },
                { "emissive_strength", compiled.standard.emissiveStrength },
                { "normal_scale", compiled.standard.normalScale },
                { "occlusion_strength", compiled.standard.occlusionStrength },
                { "alpha_mode", sourceAlphaModeName(compiled.standard.alphaMode) },
                { "alpha_cutoff", compiled.standard.alphaCutoff },
                { "double_sided", compiled.standard.doubleSided },
                { "texture_mask", compiled.standard.textureMask },
            };
            root["compiled"]["complex_lobes"] = Json::array();
            for (const ComplexLobeRecord& lobe : compiled.complexLobes)
                root["compiled"]["complex_lobes"].push_back({
                    { "type", complexLobeTypeName(lobe.type) },
                    { "source_extension", lobe.sourceExtension },
                });
            root["compiled"]["texture_operations"] = Json::array();
            for (const CompiledTextureOperation& texture : compiled.textureOperations)
                root["compiled"]["texture_operations"].push_back({
                    { "semantic", sourceTextureSemanticName(texture.semantic) },
                    { "image", texture.imageIdentity },
                    { "channels", texture.channels },
                    { "transfer", texture.transfer == SourceTextureTransfer::Srgb ? "sRGB" : "linear" },
                    { "uv", texture.transform.texCoordOverride.value_or(texture.texCoord) },
                    { "offset", { texture.transform.offset.value.x, texture.transform.offset.value.y } },
                    { "scale", { texture.transform.scale.value.x, texture.transform.scale.value.y } },
                    { "rotation", texture.transform.rotation.value },
                    { "scalar", texture.scalar.value },
                    { "sampler_source_index", texture.sampler.sourceIndex
                        ? Json(*texture.sampler.sourceIndex) : Json(nullptr) },
                });
        }
        root["instance"] = {
            { "revision", instance.revision() },
            { "override_mask", instance.overrideMask() },
            { "base_color", vec4(instance.values().baseColorFactor) },
            { "metallic", instance.values().metallicFactor },
            { "roughness", instance.values().roughnessFactor },
            { "ior", instance.values().ior },
            { "specular_factor", instance.values().specularFactor },
            { "specular_color", vec3(instance.values().specularColorFactor) },
            { "diffuse", vec4(instance.values().diffuseFactor) },
            { "specular_glossiness", vec3(instance.values().specularGlossinessFactor) },
            { "glossiness", instance.values().glossinessFactor },
            { "emissive", vec3(instance.values().emissiveFactor) },
            { "emissive_strength", instance.values().emissiveStrength },
            { "normal_scale", instance.values().normalScale },
            { "occlusion_strength", instance.values().occlusionStrength },
            { "alpha_mode", sourceAlphaModeName(instance.values().alphaMode) },
            { "alpha_cutoff", instance.values().alphaCutoff },
            { "double_sided", instance.values().doubleSided },
        };
        root["instance"]["overrides"] = Json::array();
        root["instance"]["texture_bindings"] = Json::array();
        for (const CompiledTextureOperation& operation : instance.compiled().textureOperations) {
            const MaterialTextureBinding binding = instance.textureBindings()[
                semanticIndex(operation.semantic)];
            root["instance"]["texture_bindings"].push_back({
                { "semantic", sourceTextureSemanticName(operation.semantic) },
                { "texture_index", binding.texture.getIndex() },
                { "texture_generation", binding.texture.getGeneration() },
                { "sampler_index", binding.sampler.getIndex() },
                { "sampler_generation", binding.sampler.getGeneration() },
            });
        }
        constexpr std::array fields{ MaterialOverrideBaseColor, MaterialOverrideMetallic,
            MaterialOverrideRoughness, MaterialOverrideIor, MaterialOverrideSpecular,
            MaterialOverrideDiffuse, MaterialOverrideSpecularGlossiness,
            MaterialOverrideEmissive, MaterialOverrideNormalScale,
            MaterialOverrideOcclusionStrength, MaterialOverrideAlphaCutoff,
            MaterialOverrideTexture };
        for (const MaterialOverrideField field : fields)
            if (instance.isOverridden(field))
                root["instance"]["overrides"].push_back(overrideFieldName(field));

        root["packed"] = nullptr;
        if (packResult.material) {
            const PackedGpuMaterial& packed = *packResult.material;
            root["packed"] = {
                { "schema_version", packed.schemaVersion },
                { "byte_size", sizeof(PackedGpuMaterial) },
                { "sha256", sha256(std::as_bytes(std::span(&packed, size_t{ 1 }))) },
                { "closure", packed.closureClass },
                { "workflow", packed.workflow },
                { "feature_flags", packed.featureFlags },
                { "texture_mask", packed.textureMask },
                { "alpha_mode", packed.alphaMode },
                { "double_sided", packed.doubleSided },
                { "complex_lobe_count", packed.complexLobeCount },
                { "base_color", packed.baseColorFactor },
                { "metallic_roughness_ior_specular", packed.metallicRoughnessIorSpecular },
                { "specular_color_normal_scale", packed.specularColorNormalScale },
                { "diffuse", packed.diffuseFactor },
                { "specular_glossiness", packed.specularGlossinessFactorGloss },
                { "emissive_strength", packed.emissiveFactorStrength },
                { "surface_parameters", packed.surfaceParameters },
                { "texture_indices", packed.textureIndices },
            };
            root["packed"]["complex_lobes"] = Json::array();
            for (uint32_t index = 0; index < packed.complexLobeCount; ++index)
                root["packed"]["complex_lobes"].push_back({
                    { "type", packed.complexLobes[index].type },
                    { "texture_mask", packed.complexLobes[index].textureMask },
                    { "parameters", packed.complexLobes[index].parameters },
                });
            root["packed"]["texture_half_bits"] = Json::array();
            for (size_t index = 0; index < packed.textureUses.size(); ++index) {
                if (packed.textureIndices[index] == PackedGpuMaterial::InvalidTextureIndex) continue;
                const PackedGpuTextureUse& use = packed.textureUses[index];
                root["packed"]["texture_half_bits"].push_back({
                    { "semantic", use.semantic }, { "offset", { use.offsetX, use.offsetY } },
                    { "scale", { use.scaleX, use.scaleY } }, { "rotation", use.rotation },
                    { "scalar", use.scalar }, { "sampler_index", use.samplerIndex },
                    { "uv", use.texCoord },
                });
            }
        }
        root["diagnostics"] = Json::array();
        for (const MaterialCompileDiagnostic& diagnostic : compileResult.diagnostics)
            root["diagnostics"].push_back({ { "stage", "compile" },
                { "severity", compileSeverityName(diagnostic.severity) },
                { "code", diagnostic.code }, { "message", diagnostic.message } });
        for (const MaterialPackDiagnostic& diagnostic : packResult.diagnostics)
            root["diagnostics"].push_back({ { "stage", "pack" },
                { "severity", "error" }, { "code", diagnostic.code },
                { "message", diagnostic.message } });

        MaterialDiagnosticSnapshot snapshot{};
        snapshot.json = root.dump(2);
        snapshot.sha256 = sha256(std::as_bytes(std::span(snapshot.json.data(), snapshot.json.size())));
        return snapshot;
    }

} // namespace Iridium
