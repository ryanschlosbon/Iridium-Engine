#pragma once

#include "assets/material/CompiledMaterialProduct.h"
#include "assets/cooker/CookedArtifact.h"
#include "assets/cooker/CookTypes.h"
#include "assets/texture/TextureProduct.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kCookedModelSchemaVersion = 5;
    inline constexpr uint32_t kCookedModelManifestSection = 0x4d444d31;   // MDM1
    inline constexpr uint32_t kCookedModelMaterialSection = 0x4d544c31;   // MTL1
    inline constexpr uint32_t kCookedModelTextureViewSection = 0x4d545831; // MTX1
    inline constexpr uint32_t kCookedModelVertexSection = 0x4d445631;     // MDV1
    inline constexpr uint32_t kCookedModelIndexSection = 0x4d444931;      // MDI1
    inline constexpr uint32_t kCookedModelRtPositionSection = 0x4d445031; // MDP1
    inline constexpr uint32_t kCookedModelRtIndexSection = 0x4d445231;    // MDR1
    inline constexpr uint32_t kNoModelSection = std::numeric_limits<uint32_t>::max();
    inline constexpr uint32_t kCookedModelVertexStride = 72;

    enum class ModelPrimitiveTopology : uint8_t {
        Triangles,
        TriangleStrip,
        TriangleFan,
        Lines,
        LineStrip,
        Points,
    };

    enum class ModelWinding : uint8_t {
        CounterClockwise,
        Clockwise,
    };

    enum class ModelCoverage : uint8_t {
        Opaque,
        Masked,
        Transparent,
    };

    enum class ModelIndexFormat : uint8_t {
        UInt16,
        UInt32,
    };

    enum ModelAttributeBits : uint32_t {
        ModelAttributePosition = 1u << 0,
        ModelAttributeColor0 = 1u << 1,
        ModelAttributeNormal = 1u << 2,
        ModelAttributeTexCoord0 = 1u << 3,
        ModelAttributeTangent = 1u << 4,
        ModelAttributeTexCoord1 = 1u << 5,
    };

    enum ModelPrimitiveFlagBits : uint32_t {
        ModelPrimitiveDoubleSided = 1u << 0,
        ModelPrimitiveMirroredTransform = 1u << 1,
        ModelPrimitiveGeneratedTangent = 1u << 2,
    };

    enum ModelRtGeometryFlagBits : uint32_t {
        ModelRtBuildInput = 1u << 0,
        ModelRtOpaque = 1u << 1,
        ModelRtAllowAnyHit = 1u << 2,
    };

    struct CookedModelVertex {
        std::array<float, 3> position{};
        std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::array<float, 3> normal{};
        std::array<float, 2> texCoord0{};
        std::array<float, 4> tangent{};
        std::array<float, 2> texCoord1{};

        bool operator==(const CookedModelVertex&) const = default;
    };

    struct CookedModelBounds {
        std::array<float, 3> aabbMin{};
        std::array<float, 3> aabbMax{};
        std::array<float, 3> sphereCenter{};
        float sphereRadius = 0.0f;

        bool operator==(const CookedModelBounds&) const = default;
    };

    struct CookedModelPrimitive {
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        AssetGuid materialGuid;
        std::string sourceKey;
        uint32_t sourceNode = 0;
        uint32_t sourceMesh = 0;
        uint32_t sourcePrimitive = 0;
        uint32_t attributeMask = ModelAttributePosition;
        uint64_t firstVertex = 0;
        uint64_t vertexCount = 0;
        uint64_t firstIndex = 0;
        uint64_t indexCount = 0;
        uint64_t rtFirstPosition = 0;
        uint64_t rtPositionCount = 0;
        uint64_t rtFirstIndex = 0;
        uint64_t rtIndexCount = 0;
        ModelPrimitiveTopology topology = ModelPrimitiveTopology::Triangles;
        ModelWinding winding = ModelWinding::CounterClockwise;
        ModelCoverage coverage = ModelCoverage::Opaque;
        ModelIndexFormat indexFormat = ModelIndexFormat::UInt32;
        uint32_t flags = 0;
        uint32_t rtFlags = ModelRtBuildInput | ModelRtOpaque;
        uint32_t lodSection = kNoModelSection;
        uint32_t meshletSection = kNoModelSection;
        CompiledTransparencyPolicy transparency;
        CookedModelBounds bounds;

        bool operator==(const CookedModelPrimitive&) const = default;
    };

    struct CookedModelManifest {
        uint32_t schemaVersion = kCookedModelSchemaVersion;
        uint32_t vertexStride = kCookedModelVertexStride;
        uint64_t vertexCount = 0;
        uint64_t indexCount = 0;
        uint64_t rtPositionCount = 0;
        uint64_t rtIndexCount = 0;
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::LegacyTwoBucket;
        std::vector<CookedModelPrimitive> primitives;

        bool operator==(const CookedModelManifest&) const = default;
    };

    struct CookedModelTextureBinding {
        uint32_t operationIndex = 0;
        uint32_t sourceImageIndex = 0;
        uint32_t textureViewIndex = 0;
        AssetGuid textureGuid;

        bool operator==(const CookedModelTextureBinding&) const = default;
    };

    struct CookedModelTextureView {
        AssetGuid textureGuid;
        uint32_t sourceImageIndex = 0;
        std::string viewKey;
        CookedTextureManifest manifest;
        std::vector<std::byte> payload;

        bool operator==(const CookedModelTextureView&) const = default;
    };

    struct CookedModelMaterial {
        AssetGuid materialGuid;
        std::string sourceKey;
        CompiledMaterial compiled;
        std::vector<CookedModelTextureBinding> textureBindings;

        bool operator==(const CookedModelMaterial& other) const;
    };

    struct CookedModelProductData {
        CookedModelManifest manifest;
        std::vector<CookedModelMaterial> materials;
        std::vector<CookedModelTextureView> textureViews;
        std::vector<CookedModelVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<std::array<float, 3>> rtPositions;
        std::vector<uint32_t> rtIndices;

        bool operator==(const CookedModelProductData&) const = default;
    };

    struct CookedModelReadResult {
        std::optional<CookedModelProductData> data;
        std::vector<CookDiagnostic> diagnostics;

        [[nodiscard]] bool valid() const noexcept {
            return data.has_value() && !hasCookErrors(diagnostics);
        }
    };

    [[nodiscard]] std::vector<std::byte> serializeModelManifest(
        const CookedModelManifest& manifest);
    [[nodiscard]] std::optional<CookedModelManifest> readModelManifest(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<std::byte> serializeModelMaterials(
        std::span<const CookedModelMaterial> materials);
    [[nodiscard]] std::optional<std::vector<CookedModelMaterial>>
        readModelMaterials(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<std::byte>
        serializeModelTextureViews(
            std::span<const CookedModelTextureView> views);
    [[nodiscard]] std::string calculateModelTextureViewKey(
        const CookedModelTextureView& view);
    [[nodiscard]] std::optional<
        std::vector<CookedModelTextureView>>
        readModelTextureViews(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<std::byte> serializeModelVertices(
        std::span<const CookedModelVertex> vertices);
    [[nodiscard]] std::vector<std::byte> serializeModelIndices(
        std::span<const uint32_t> indices);
    [[nodiscard]] std::vector<std::byte> serializeModelRtPositions(
        std::span<const std::array<float, 3>> positions);
    [[nodiscard]] std::optional<std::vector<CookedModelVertex>> readModelVertices(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::optional<std::vector<uint32_t>> readModelIndices(
        std::span<const std::byte> bytes,
        std::vector<CookDiagnostic>& diagnostics,
        std::string field = "/indices");
    [[nodiscard]] std::optional<std::vector<std::array<float, 3>>>
        readModelRtPositions(
            std::span<const std::byte> bytes,
            std::vector<CookDiagnostic>& diagnostics);
    [[nodiscard]] std::vector<CookDiagnostic> validateModelProduct(
        const CookedModelProductData& data);
    [[nodiscard]] CookProduct makeCookedModelProduct(
        const CookedModelProductData& data);
    [[nodiscard]] CookedModelReadResult readCookedModelProduct(
        const CookedArtifact& artifact);

} // namespace Iridium
