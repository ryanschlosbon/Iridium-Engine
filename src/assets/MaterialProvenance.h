#pragma once

#include "renderer/rhi/PipelineTypes.h"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Iridium {

    enum class MaterialValueOrigin : uint8_t {
        Explicit,
        FormatDefault,
        EngineFallback,
        Unavailable,
    };

    template <typename T>
    struct ProvenancedValue {
        T value{};
        MaterialValueOrigin origin = MaterialValueOrigin::Unavailable;
    };

    enum class MaterialTextureSemantic : uint8_t {
        BaseColor,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive,
        Transmission,
    };

    enum class TextureColorInterpretation : uint8_t {
        SRGB,
        Linear,
    };

    struct SourceSamplerProvenance {
        bool samplerObjectExplicit = false;
        std::optional<int32_t> magFilter;
        std::optional<int32_t> minFilter;
        int32_t wrapS = 10497;
        int32_t wrapT = 10497;
        bool wrapSExplicit = false;
        bool wrapTExplicit = false;
    };

    struct MaterialTextureProvenance {
        MaterialTextureSemantic semantic = MaterialTextureSemantic::BaseColor;
        bool present = false;
        std::optional<uint32_t> textureIndex;
        std::optional<uint32_t> imageIndex;
        std::string imageIdentity;
        std::string channels;
        TextureColorInterpretation colorInterpretation =
            TextureColorInterpretation::Linear;
        ProvenancedValue<uint32_t> uvSet{ 0, MaterialValueOrigin::FormatDefault };
        SourceSamplerProvenance sampler;
        TextureHandle runtimeTexture;
        bool usedEngineFallback = true;
        bool consumedByRuntime = true;
    };

    enum class MaterialExtensionDisposition : uint8_t {
        Applied,
        ParsedNotConsumed,
        UnsupportedIgnored,
    };

    struct MaterialExtensionProvenance {
        std::string name;
        bool required = false;
        MaterialExtensionDisposition disposition =
            MaterialExtensionDisposition::UnsupportedIgnored;
        std::string sourceValues;
    };

    struct MaterialPushConstantInputs {
        glm::vec4 baseColor{ 1.0f };
        glm::vec4 emissiveFactor{ 0.0f };
        float metallic = 1.0f;
        float roughness = 1.0f;
        float normalScale = 1.0f;
        float alphaCutoff = 0.0f;
        float transmissionFactor = 0.0f;
    };

    struct MaterialDiagnosticTexturePreview {
        std::string semantic;
        TextureHandle texture;
        bool engineFallback = false;
    };

    struct MaterialProvenance {
        std::string sourceAsset;
        uint32_t sourceMaterialIndex = 0;
        std::string sourceName;

        ProvenancedValue<glm::vec4> baseColor;
        ProvenancedValue<float> metallic;
        ProvenancedValue<float> roughness;
        ProvenancedValue<glm::vec3> emissive;
        ProvenancedValue<float> emissiveStrength;
        ProvenancedValue<float> normalScale;
        ProvenancedValue<std::string> alphaMode;
        ProvenancedValue<float> alphaCutoff;
        ProvenancedValue<float> transmission;
        ProvenancedValue<bool> doubleSided;

        std::vector<MaterialTextureProvenance> textures;
        std::vector<MaterialExtensionProvenance> extensions;
        std::vector<std::string> warnings;

        std::string compiledWorkflow;
        std::string compiledClosure;
        std::string diagnosticSnapshotJson;
        std::string diagnosticSnapshotSha256;
        std::vector<MaterialDiagnosticTexturePreview> diagnosticTexturePreviews;

        MaterialAsset runtime;
        MaterialBinding gpuBinding;
        MaterialPushConstantInputs pushConstantInputs;
    };

    [[nodiscard]] std::vector<MaterialProvenance> inspectGltfMaterialSources(
        const std::filesystem::path& path);
    [[nodiscard]] MaterialProvenance makeDefaultMaterialProvenance(
        const std::filesystem::path& path, uint32_t materialIndex);
    void attachRuntimeMaterial(MaterialProvenance& provenance,
        const MaterialAsset& runtime, MaterialBinding binding);
    void markRuntimeTextureFallbacks(MaterialProvenance& provenance,
        const std::array<TextureHandle, 5>& fallbackHandles);

    [[nodiscard]] const char* materialValueOriginName(MaterialValueOrigin origin) noexcept;
    [[nodiscard]] const char* materialTextureSemanticName(
        MaterialTextureSemantic semantic) noexcept;
    [[nodiscard]] const char* textureColorInterpretationName(
        TextureColorInterpretation interpretation) noexcept;
    [[nodiscard]] const char* materialExtensionDispositionName(
        MaterialExtensionDisposition disposition) noexcept;

} // namespace Iridium
