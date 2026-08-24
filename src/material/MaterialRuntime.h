#pragma once

#include "material/MaterialCompiler.h"
#include "renderer/rhi/RenderHandles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Iridium {

    enum class MaterialOverrideStatus : uint8_t {
        Applied,
        Unchanged,
        RequiresRecompile,
        InvalidValue,
    };

    enum MaterialOverrideField : uint64_t {
        MaterialOverrideNone = 0,
        MaterialOverrideBaseColor = 1ull << 0u,
        MaterialOverrideMetallic = 1ull << 1u,
        MaterialOverrideRoughness = 1ull << 2u,
        MaterialOverrideIor = 1ull << 3u,
        MaterialOverrideSpecular = 1ull << 4u,
        MaterialOverrideDiffuse = 1ull << 5u,
        MaterialOverrideSpecularGlossiness = 1ull << 6u,
        MaterialOverrideEmissive = 1ull << 7u,
        MaterialOverrideNormalScale = 1ull << 8u,
        MaterialOverrideOcclusionStrength = 1ull << 9u,
        MaterialOverrideAlphaCutoff = 1ull << 10u,
        MaterialOverrideTexture = 1ull << 11u,
    };

    struct MaterialTextureBinding {
        TextureHandle texture;
        SamplerHandle sampler;
        bool reconstructNormalZ = false;

        constexpr bool operator==(const MaterialTextureBinding&) const noexcept = default;
    };

    class MaterialInstance {
    public:
        MaterialInstance(std::shared_ptr<const CompiledMaterial> compiled,
            std::span<const MaterialTextureBinding> textureBindings = {});

        [[nodiscard]] const CompiledMaterial& compiled() const noexcept;
        [[nodiscard]] const std::shared_ptr<const CompiledMaterial>& compiledAsset() const noexcept;
        [[nodiscard]] const StandardClosureRecipe& values() const noexcept;
        [[nodiscard]] std::span<const MaterialTextureBinding, 21> textureBindings() const noexcept;
        [[nodiscard]] uint64_t revision() const noexcept;
        [[nodiscard]] uint64_t overrideMask() const noexcept;
        [[nodiscard]] bool isOverridden(MaterialOverrideField field) const noexcept;

        MaterialOverrideStatus setBaseColor(glm::vec4 value) noexcept;
        MaterialOverrideStatus setMetallic(float value) noexcept;
        MaterialOverrideStatus setRoughness(float value) noexcept;
        MaterialOverrideStatus setIor(float value) noexcept;
        MaterialOverrideStatus setSpecular(float factor, glm::vec3 color) noexcept;
        MaterialOverrideStatus setDiffuseFactor(glm::vec4 value) noexcept;
        MaterialOverrideStatus setSpecularGlossiness(glm::vec3 factor,
            float glossiness) noexcept;
        MaterialOverrideStatus setEmissive(glm::vec3 factor, float strength) noexcept;
        MaterialOverrideStatus setNormalScale(float value) noexcept;
        MaterialOverrideStatus setOcclusionStrength(float value) noexcept;
        MaterialOverrideStatus setAlphaCutoff(float value) noexcept;
        MaterialOverrideStatus setTextureBinding(SourceTextureSemantic semantic,
            std::optional<MaterialTextureBinding> binding) noexcept;
        MaterialOverrideStatus setAlphaMode(SourceAlphaMode value) noexcept;
        MaterialOverrideStatus setDoubleSided(bool value) noexcept;

    private:
        template <typename T>
        MaterialOverrideStatus applyValue(T& destination, const T& value,
            MaterialOverrideField field) noexcept;

        std::shared_ptr<const CompiledMaterial> m_compiled;
        StandardClosureRecipe m_values;
        std::array<MaterialTextureBinding, 21> m_textureBindings{};
        uint64_t m_revision = 1;
        uint64_t m_overrideMask = MaterialOverrideNone;
    };

    struct alignas(16) PackedGpuTextureUse {
        uint16_t offsetX = 0;
        uint16_t offsetY = 0;
        uint16_t scaleX = 0;
        uint16_t scaleY = 0;
        uint16_t rotation = 0;
        uint16_t scalar = 0;
        uint16_t samplerIndex = 0xffffu;
        uint8_t texCoord = 0;
        uint8_t semantic = 0;
    };

    struct alignas(16) PackedGpuComplexLobe {
        uint32_t type = 0;
        uint32_t textureMask = 0;
        std::array<float, 6> parameters{};
    };

    struct alignas(16) PackedGpuMaterial {
        // Schema 3 is laid out so the record can be consumed directly as a
        // std430 storage-buffer element. Texture uses map to uvec4 values.
        static constexpr uint32_t SchemaVersion = 3;
        static constexpr uint32_t MaxTextureUses = 21;
        static constexpr uint32_t MaxComplexLobes = 8;
        static constexpr uint32_t InvalidTextureIndex = 0xffffffffu;

        uint32_t schemaVersion = SchemaVersion;
        uint32_t closureClass = 0;
        uint32_t workflow = 0;
        uint32_t featureFlags = 0;
        uint32_t textureMask = 0;
        uint32_t alphaMode = 0;
        uint32_t doubleSided = 0;
        uint32_t complexLobeCount = 0;
        std::array<float, 4> baseColorFactor{};
        std::array<float, 4> metallicRoughnessIorSpecular{};
        std::array<float, 4> specularColorNormalScale{};
        std::array<float, 4> diffuseFactor{};
        std::array<float, 4> specularGlossinessFactorGloss{};
        std::array<float, 4> emissiveFactorStrength{};
        std::array<float, 4> surfaceParameters{};
        std::array<PackedGpuComplexLobe, MaxComplexLobes> complexLobes{};
        std::array<PackedGpuTextureUse, MaxTextureUses> textureUses{};
        std::array<uint32_t, MaxTextureUses> textureIndices{};
        uint32_t transparencyPolicy = 0;
        int32_t transparencyPriority = 0;
        float thinSheetThicknessMeters = 0.0f;
    };

    static_assert(sizeof(PackedGpuTextureUse) == 16);
    static_assert(sizeof(PackedGpuComplexLobe) == 32);
    static_assert(sizeof(PackedGpuMaterial) == 832);
    static_assert(alignof(PackedGpuMaterial) == 16);
    static_assert(std::is_standard_layout_v<PackedGpuMaterial>);
    static_assert(std::is_trivially_copyable_v<PackedGpuMaterial>);

    struct MaterialPackingContext {
        // Index -> currently live generation. A zero generation marks a free slot.
        std::span<const uint32_t> textureGenerations;
        std::span<const uint32_t> samplerGenerations;
    };

    enum class MaterialPackError : uint8_t {
        None,
        StaleTextureHandle,
        StaleSamplerHandle,
        HalfOverflow,
        SamplerIndexOverflow,
        TexCoordOverflow,
        TooManyComplexLobes,
    };

    struct MaterialPackDiagnostic {
        MaterialPackError error = MaterialPackError::None;
        std::string code;
        std::string message;
    };

    struct MaterialPackResult {
        std::optional<PackedGpuMaterial> material;
        std::vector<MaterialPackDiagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept;
    };

    struct UnpackedGpuTextureUse {
        glm::vec2 offset{ 0.0f };
        glm::vec2 scale{ 1.0f };
        float rotation = 0.0f;
        float scalar = 1.0f;
        uint16_t samplerIndex = 0xffffu;
        uint8_t texCoord = 0;
        SourceTextureSemantic semantic = SourceTextureSemantic::BaseColor;
    };

    struct UnpackedGpuMaterial {
        StandardClosureRecipe values;
        MaterialClosureClass closureClass = MaterialClosureClass::Invalid;
        MaterialWorkflow workflow = MaterialWorkflow::MetallicRoughness;
        uint32_t featureFlags = 0;
        CompiledTransparencyPolicy transparency;
        std::array<uint32_t, 21> textureIndices{};
        std::array<UnpackedGpuTextureUse, 21> textureUses{};
        std::array<PackedGpuComplexLobe, 8> complexLobes{};
        uint32_t complexLobeCount = 0;
    };

    [[nodiscard]] uint16_t floatToHalf(float value) noexcept;
    [[nodiscard]] uint32_t packedTextureReconstructNormalZMask(
        const PackedGpuMaterial& material) noexcept;
    [[nodiscard]] MaterialPackResult packMaterialInstance(const MaterialInstance& instance,
        const MaterialPackingContext& context);
    [[nodiscard]] UnpackedGpuMaterial unpackGpuMaterial(const PackedGpuMaterial& material);
    [[nodiscard]] bool consumeMaterialUploadRevision(uint64_t revision,
        uint64_t& uploadedRevision) noexcept;

    struct MaterialInstanceTag {};
    using MaterialInstanceHandle = RenderHandle<MaterialInstanceTag>;

    struct MaterialUpload {
        MaterialInstanceHandle instance;
        PackedGpuMaterial material;
    };

    struct MaterialUploadStats {
        uint32_t activeInstances = 0;
        uint32_t changedInstances = 0;
        uint32_t failedInstances = 0;
        uint64_t uploadedBytes = 0;
    };

    class MaterialInstanceStore {
    public:
        explicit MaterialInstanceStore(size_t reserveSize = 4096);

        MaterialInstanceHandle create(std::shared_ptr<const CompiledMaterial> compiled,
            std::span<const MaterialTextureBinding> textureBindings = {});
        void destroy(MaterialInstanceHandle handle) noexcept;
        [[nodiscard]] MaterialInstance* get(MaterialInstanceHandle handle) noexcept;
        [[nodiscard]] const MaterialInstance* get(MaterialInstanceHandle handle) const noexcept;
        [[nodiscard]] size_t activeCount() const noexcept;
        [[nodiscard]] MaterialUploadStats collectChanged(const MaterialPackingContext& context,
            std::vector<MaterialUpload>& output);

    private:
        struct Slot {
            std::optional<MaterialInstance> instance;
            uint32_t generation = 1;
            uint64_t uploadedRevision = 0;
        };

        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_freeIndices;
    };

    struct MaterialDiagnosticSnapshot {
        std::string json;
        std::string sha256;
    };

    [[nodiscard]] MaterialDiagnosticSnapshot buildMaterialDiagnosticSnapshot(
        const SourceMaterial& source,
        const MaterialCompileResult& compileResult,
        const MaterialInstance& instance,
        const MaterialPackResult& packResult);

} // namespace Iridium
