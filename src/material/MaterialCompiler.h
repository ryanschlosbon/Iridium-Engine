#pragma once

#include "material/SourceMaterial.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Iridium {

    enum class MaterialCompilePolicy : uint8_t {
        Strict,
        PermissiveEditor,
    };

    enum class MaterialWorkflow : uint8_t {
        MetallicRoughness,
        SpecularGlossiness,
        Unlit,
    };

    enum class MaterialClosureClass : uint8_t {
        StandardDeferred,
        StandardForward,
        ComplexForward,
        Unlit,
        Invalid,
    };

    enum class MaterialCompileSeverity : uint8_t {
        Info,
        Warning,
        Error,
    };

    enum class ComplexLobeType : uint8_t {
        Clearcoat,
        Sheen,
        Anisotropy,
        Iridescence,
        ThinTransmission,
        VolumeTransmission,
        Dispersion,
        DiffuseTransmission,
    };

    enum MaterialFeatureFlag : uint32_t {
        MaterialFeatureNone = 0,
        MaterialFeatureNormalMap = 1u << 0u,
        MaterialFeatureOcclusion = 1u << 1u,
        MaterialFeatureEmissive = 1u << 2u,
        MaterialFeatureAlphaMask = 1u << 3u,
        MaterialFeatureAlphaBlend = 1u << 4u,
        MaterialFeatureDoubleSided = 1u << 5u,
        MaterialFeatureSpecular = 1u << 6u,
        MaterialFeatureTextureTransform = 1u << 7u,
        MaterialFeaturePermissiveFallback = 1u << 8u,
        MaterialFeatureClearcoat = 1u << 9u,
        MaterialFeatureSheen = 1u << 10u,
        MaterialFeatureAnisotropy = 1u << 11u,
        MaterialFeatureIridescence = 1u << 12u,
        MaterialFeatureTransmission = 1u << 13u,
        MaterialFeatureVolume = 1u << 14u,
        MaterialFeatureDispersion = 1u << 15u,
        MaterialFeatureDiffuseTransmission = 1u << 16u,
        MaterialFeatureUnlit = 1u << 17u,
        MaterialFeaturePackedNormalReconstructZ = 1u << 18u,
        // Applied only while publishing a classified cooked product; source
        // material identity remains independent of the selected execution mode.
        MaterialFeatureClassifiedTransparencyExecution = 1u << 19u,
    };

    struct MaterialCompileDiagnostic {
        MaterialCompileSeverity severity = MaterialCompileSeverity::Info;
        std::string code;
        std::string message;
    };

    struct StandardClosureRecipe {
        glm::vec4 baseColorFactor{ 1.0f };
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float ior = 1.5f;
        float specularFactor = 1.0f;
        glm::vec3 specularColorFactor{ 1.0f };
        glm::vec4 diffuseFactor{ 1.0f };
        glm::vec3 specularGlossinessFactor{ 1.0f };
        float glossinessFactor = 1.0f;
        glm::vec3 emissiveFactor{ 0.0f };
        float emissiveStrength = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        SourceAlphaMode alphaMode = SourceAlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        bool doubleSided = false;
        uint32_t textureMask = 0;
    };

    struct CompiledTextureOperation {
        SourceTextureSemantic semantic = SourceTextureSemantic::BaseColor;
        uint32_t sourceTextureIndex = 0;
        std::optional<uint32_t> sourceImageIndex;
        std::string imageIdentity;
        std::string channels;
        SourceTextureTransfer transfer = SourceTextureTransfer::Linear;
        uint32_t texCoord = 0;
        SourceTextureTransform transform;
        SourceSampler sampler;
        SourceValue<float> scalar{ 1.0f, SourceValueOrigin::FormatDefault };
    };

    struct ClearcoatLobe {
        float factor = 0.0f;
        float roughnessFactor = 0.0f;
        float normalScale = 1.0f;
        uint32_t textureMask = 0;
    };
    struct SheenLobe {
        glm::vec3 color{ 1.0f };
        float roughnessFactor = 0.0f;
        uint32_t textureMask = 0;
    };
    struct AnisotropyLobe {
        float strength = 0.0f;
        float rotation = 0.0f;
        uint32_t textureMask = 0;
    };
    struct IridescenceLobe {
        float factor = 0.0f;
        float ior = 1.3f;
        float thicknessMinimumNm = 100.0f;
        float thicknessMaximumNm = 400.0f;
        uint32_t textureMask = 0;
    };
    struct ThinTransmissionLobe {
        float factor = 0.0f;
        float ior = 1.5f;
        float specularFactor = 1.0f;
        glm::vec3 specularColor{ 1.0f };
        uint32_t textureMask = 0;
    };
    struct VolumeTransmissionLobe {
        float thicknessFactor = 0.0f;
        float attenuationDistance = std::numeric_limits<float>::infinity();
        glm::vec3 attenuationColor{ 1.0f };
        uint32_t textureMask = 0;
    };
    struct DispersionLobe {
        float dispersion = 0.0f;
    };
    struct DiffuseTransmissionLobe {
        float factor = 0.0f;
        glm::vec3 color{ 1.0f };
        uint32_t textureMask = 0;
    };

    using ComplexLobeData = std::variant<ClearcoatLobe, SheenLobe, AnisotropyLobe,
        IridescenceLobe, ThinTransmissionLobe, VolumeTransmissionLobe,
        DispersionLobe, DiffuseTransmissionLobe>;

    struct ComplexLobeRecord {
        ComplexLobeType type = ComplexLobeType::Clearcoat;
        std::string sourceExtension;
        ComplexLobeData data = ClearcoatLobe{};
    };

    struct CompiledMaterial {
        static constexpr uint32_t SchemaVersion = 2;

        uint32_t schemaVersion = SchemaVersion;
        uint32_t sourceMaterialIndex = 0;
        std::string sourceName;
        MaterialWorkflow workflow = MaterialWorkflow::MetallicRoughness;
        MaterialClosureClass closureClass = MaterialClosureClass::Invalid;
        uint32_t featureFlags = MaterialFeatureNone;
        CompiledTransparencyPolicy transparency;
        StandardClosureRecipe standard;
        std::vector<CompiledTextureOperation> textureOperations;
        std::vector<ComplexLobeRecord> complexLobes;
        std::string contentHash;
    };

    struct MaterialCompileResult {
        // Compilation is the only mutation boundary. Consumers receive shared
        // immutable ownership so instances cannot alter a compiled recipe.
        std::shared_ptr<const CompiledMaterial> material;
        std::vector<MaterialCompileDiagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept;
    };

    struct MaterialCompileDocumentResult {
        std::vector<MaterialCompileResult> materials;
        std::vector<MaterialCompileDiagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept;
    };

    struct MaterialTextureSamples {
        std::array<glm::vec4, 21> encodedOrLinear{};

        MaterialTextureSamples() noexcept;
        [[nodiscard]] const glm::vec4& sample(SourceTextureSemantic semantic) const noexcept;
        glm::vec4& sample(SourceTextureSemantic semantic) noexcept;
    };

    struct MaterialSurfaceInputs {
        MaterialTextureSamples textures;
        glm::vec4 vertexColor{ 1.0f };
        glm::vec3 geometricNormal{ 0.0f, 0.0f, 1.0f };
        glm::vec3 tangent{ 1.0f, 0.0f, 0.0f };
        float tangentHandedness = 1.0f;
        bool frontFacing = true;
    };

    struct CanonicalMaterialSurface {
        glm::vec3 diffuseAlbedo{ 0.0f };
        glm::vec3 f0{ 0.0f };
        float f90 = 1.0f;
        float perceptualRoughness = 1.0f;
        glm::vec3 shadingNormal{ 0.0f, 0.0f, 1.0f };
        float ao = 1.0f;
        glm::vec3 emissive{ 0.0f };
        glm::vec3 unlitRadiance{ 0.0f };
        float alpha = 1.0f;
        bool alphaPasses = true;
        uint32_t flags = MaterialFeatureNone;
    };

    [[nodiscard]] MaterialCompileResult compileSourceMaterial(
        const SourceMaterial& source,
        MaterialCompilePolicy policy = MaterialCompilePolicy::Strict,
        std::span<const SourceMaterialDiagnostic> sourceDiagnostics = {});
    [[nodiscard]] MaterialCompileDocumentResult compileSourceMaterialDocument(
        const SourceMaterialDocument& document,
        MaterialCompilePolicy policy = MaterialCompilePolicy::Strict);
    [[nodiscard]] MaterialCompileResult applyCompiledTransparencyPolicy(
        const CompiledMaterial& material,
        const TransparencyPolicyV1& policy,
        TransparencyTopology topology = TransparencyTopology::Unknown);
    // Canonical compiled-closure identity used by cooked material products.
    // Source names and local source indices are provenance, not render identity,
    // and intentionally do not participate in this hash.
    [[nodiscard]] std::string calculateCompiledMaterialHash(
        const CompiledMaterial& material);
    [[nodiscard]] CanonicalMaterialSurface evaluateMaterialSurface(
        const CompiledMaterial& material, const MaterialSurfaceInputs& inputs = {});

    [[nodiscard]] const char* materialWorkflowName(MaterialWorkflow workflow) noexcept;
    [[nodiscard]] const char* materialClosureClassName(MaterialClosureClass closureClass) noexcept;
    [[nodiscard]] const char* complexLobeTypeName(ComplexLobeType type) noexcept;

} // namespace Iridium
