#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class SourceValueOrigin : uint8_t {
        Authored,
        FormatDefault,
    };

    template <typename T>
    struct SourceValue {
        T value{};
        SourceValueOrigin origin = SourceValueOrigin::FormatDefault;
    };

    enum class SourceDiagnosticSeverity : uint8_t {
        Info,
        Warning,
        Error,
    };

    struct SourceMaterialDiagnostic {
        SourceDiagnosticSeverity severity = SourceDiagnosticSeverity::Info;
        std::string code;
        std::string path;
        std::string message;
    };

    enum class SourceAlphaMode : uint8_t {
        Opaque,
        Mask,
        Blend,
    };

    enum class SourceTextureTransfer : uint8_t {
        Srgb,
        Linear,
    };

    enum class SourceTextureSemantic : uint8_t {
        BaseColor,
        MetallicRoughness,
        Normal,
        Occlusion,
        Emissive,
        Diffuse,
        SpecularGlossiness,
        Clearcoat,
        ClearcoatRoughness,
        ClearcoatNormal,
        SheenColor,
        SheenRoughness,
        Specular,
        SpecularColor,
        Anisotropy,
        Iridescence,
        IridescenceThickness,
        Transmission,
        Thickness,
        DiffuseTransmission,
        DiffuseTransmissionColor,
    };

    struct SourceTextureTransform {
        SourceValue<glm::vec2> offset{ glm::vec2(0.0f), SourceValueOrigin::FormatDefault };
        SourceValue<float> rotation{ 0.0f, SourceValueOrigin::FormatDefault };
        SourceValue<glm::vec2> scale{ glm::vec2(1.0f), SourceValueOrigin::FormatDefault };
        std::optional<uint32_t> texCoordOverride;
    };

    struct SourceSampler {
        std::optional<uint32_t> sourceIndex;
        SourceValue<std::optional<int32_t>> magFilter;
        SourceValue<std::optional<int32_t>> minFilter;
        SourceValue<int32_t> wrapS{ 10497, SourceValueOrigin::FormatDefault };
        SourceValue<int32_t> wrapT{ 10497, SourceValueOrigin::FormatDefault };
    };

    struct SourceTextureUse {
        SourceTextureSemantic semantic = SourceTextureSemantic::BaseColor;
        uint32_t textureIndex = 0;
        std::optional<uint32_t> imageIndex;
        std::string imageIdentity;
        std::string channels;
        SourceTextureTransfer transfer = SourceTextureTransfer::Linear;
        SourceValue<uint32_t> texCoord{ 0, SourceValueOrigin::FormatDefault };
        SourceTextureTransform transform;
        SourceSampler sampler;
        SourceValue<float> scalar{ 1.0f, SourceValueOrigin::FormatDefault };
    };

    struct SourceExtensionProperty {
        std::string name;
        std::string canonicalValue;
        SourceValueOrigin origin = SourceValueOrigin::FormatDefault;
    };

    struct SourceMaterialExtension {
        std::string name;
        bool required = false;
        bool supportedByM2 = false;
        std::string canonicalValues;
        std::vector<SourceExtensionProperty> properties;
    };

    struct SourceMetallicRoughness {
        SourceValue<glm::vec4> baseColorFactor{ glm::vec4(1.0f), SourceValueOrigin::FormatDefault };
        SourceValue<float> metallicFactor{ 1.0f, SourceValueOrigin::FormatDefault };
        SourceValue<float> roughnessFactor{ 1.0f, SourceValueOrigin::FormatDefault };
    };

    struct SourceMaterial {
        uint32_t localIndex = 0;
        std::string name;
        SourceMetallicRoughness metallicRoughness;
        SourceValue<glm::vec3> emissiveFactor{ glm::vec3(0.0f), SourceValueOrigin::FormatDefault };
        SourceValue<float> emissiveStrength{ 1.0f, SourceValueOrigin::FormatDefault };
        SourceValue<float> normalScale{ 1.0f, SourceValueOrigin::FormatDefault };
        SourceValue<float> occlusionStrength{ 1.0f, SourceValueOrigin::FormatDefault };
        SourceValue<SourceAlphaMode> alphaMode{ SourceAlphaMode::Opaque, SourceValueOrigin::FormatDefault };
        SourceValue<float> alphaCutoff{ 0.5f, SourceValueOrigin::FormatDefault };
        SourceValue<bool> doubleSided{ false, SourceValueOrigin::FormatDefault };
        std::vector<SourceTextureUse> textures;
        std::vector<SourceMaterialExtension> extensions;
    };

    struct SourceMaterialVariant {
        uint32_t localIndex = 0;
        std::string name;
    };

    struct SourceMaterialVariantMapping {
        uint32_t meshIndex = 0;
        uint32_t primitiveIndex = 0;
        uint32_t materialIndex = 0;
        std::vector<uint32_t> variantIndices;
    };

    class SourceMaterialDocument {
    public:
        SourceMaterialDocument() = default;
        SourceMaterialDocument(std::filesystem::path sourcePath,
            std::vector<SourceMaterial> materials,
            std::vector<SourceMaterialDiagnostic> diagnostics,
            std::vector<std::string> extensionsUsed,
            std::vector<std::string> extensionsRequired,
            std::vector<SourceMaterialVariant> variants = {},
            std::vector<SourceMaterialVariantMapping> variantMappings = {});

        [[nodiscard]] const std::filesystem::path& sourcePath() const noexcept;
        [[nodiscard]] std::span<const SourceMaterial> materials() const noexcept;
        [[nodiscard]] std::span<const SourceMaterialDiagnostic> diagnostics() const noexcept;
        [[nodiscard]] std::span<const std::string> extensionsUsed() const noexcept;
        [[nodiscard]] std::span<const std::string> extensionsRequired() const noexcept;
        [[nodiscard]] std::span<const SourceMaterialVariant> variants() const noexcept;
        [[nodiscard]] std::span<const SourceMaterialVariantMapping> variantMappings() const noexcept;
        [[nodiscard]] bool hasErrors() const noexcept;

    private:
        std::filesystem::path m_sourcePath;
        std::vector<SourceMaterial> m_materials;
        std::vector<SourceMaterialDiagnostic> m_diagnostics;
        std::vector<std::string> m_extensionsUsed;
        std::vector<std::string> m_extensionsRequired;
        std::vector<SourceMaterialVariant> m_variants;
        std::vector<SourceMaterialVariantMapping> m_variantMappings;
    };

    // This is the sole provenance-preserving material JSON/GLB ingestion entry point.
    // It returns a document with stable diagnostics, including errors, instead of
    // throwing for malformed source data. File I/O failures are diagnostics too.
    [[nodiscard]] SourceMaterialDocument importGltfSourceMaterials(
        const std::filesystem::path& path);

    [[nodiscard]] const SourceTextureUse* findSourceTexture(
        const SourceMaterial& material, SourceTextureSemantic semantic) noexcept;
    [[nodiscard]] const SourceMaterialExtension* findSourceExtension(
        const SourceMaterial& material, std::string_view name) noexcept;
    [[nodiscard]] const char* sourceValueOriginName(SourceValueOrigin origin) noexcept;
    [[nodiscard]] const char* sourceDiagnosticSeverityName(
        SourceDiagnosticSeverity severity) noexcept;
    [[nodiscard]] const char* sourceAlphaModeName(SourceAlphaMode mode) noexcept;
    [[nodiscard]] const char* sourceTextureSemanticName(
        SourceTextureSemantic semantic) noexcept;

} // namespace Iridium
