#pragma once
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "assets/AssetGuid.h"
#include "PipelineTypes.h"

namespace Iridium {

    enum class ViewProjectionKind : uint32_t {
        Perspective = 0,
        Orthographic = 1,
    };

    inline constexpr uint32_t ViewTransportRefractionPyramidsAvailable =
        1u << 0u;

    struct ViewTransportRecord {
        alignas(16) glm::mat4 view{ 1.0f };
        alignas(16) glm::mat4 projection{ 1.0f };
        alignas(16) glm::mat4 inverseView{ 1.0f };
        alignas(16) glm::mat4 inverseProjection{ 1.0f };
        alignas(16) glm::vec4 cameraPosition{ 0.0f, 0.0f, 0.0f, 1.0f };
        alignas(16) glm::vec4 depthRange{ 0.1f, 100.0f, 0.0f, 0.0f };
        alignas(16) glm::uvec4 renderInfo{ 1u, 1u,
            static_cast<uint32_t>(ViewProjectionKind::Perspective), 0u };
        alignas(16) glm::vec4 worldUnits{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    static_assert(sizeof(ViewTransportRecord) == 320);

    [[nodiscard]] inline ViewTransportRecord makeViewTransportRecord(
        const glm::mat4& view, const glm::mat4& projection,
        glm::vec3 cameraPosition, float nearPlane, float farPlane,
        glm::uvec2 renderExtent, ViewProjectionKind projectionKind =
            ViewProjectionKind::Perspective,
        float metresPerWorldUnit = 1.0f) noexcept {
        return {
            .view = view,
            .projection = projection,
            .inverseView = glm::inverse(view),
            .inverseProjection = glm::inverse(projection),
            .cameraPosition = glm::vec4(cameraPosition, 1.0f),
            .depthRange = glm::vec4(nearPlane, farPlane, 0.0f, 0.0f),
            .renderInfo = glm::uvec4(renderExtent,
                static_cast<uint32_t>(projectionKind), 0u),
            .worldUnits = glm::vec4(metresPerWorldUnit, 0.0f, 0.0f, 0.0f),
        };
    }

    [[nodiscard]] inline glm::vec3 reconstructViewPosition(
        const ViewTransportRecord& view, glm::vec2 screenUv,
        float deviceDepth) noexcept {
        const glm::vec4 homogeneous = view.inverseProjection * glm::vec4(
            screenUv * 2.0f - 1.0f, deviceDepth, 1.0f);
        const float divisor = std::abs(homogeneous.w) > 1.0e-7f
            ? homogeneous.w : std::copysign(1.0e-7f,
                homogeneous.w == 0.0f ? 1.0f : homogeneous.w);
        return glm::vec3(homogeneous) / divisor;
    }

    struct SubMesh {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        int materialIndex = -1;
        AssetGuid sourcePrimitiveGuid;
        AssetGuid primitiveGuid;
        AssetGuid materialGuid;
        uint32_t sourceNode = 0;
        uint32_t sourceMesh = 0;
        uint32_t sourcePrimitive = 0;
        uint32_t attributeMask = 0;
        uint32_t flags = 0;
        uint8_t coverage = 0;
        CompiledTransparencyPolicy transparency;
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        glm::vec3 boundsSphereCenter{ 0.0f };
        float boundsSphereRadius = 0.0f;
    };

    // The Vertex is Plain Old Data (POD)
    struct Vertex {
        glm::vec3 pos;
        glm::vec4 color = glm::vec4(1.0f);
        glm::vec3 normal;
        glm::vec2 uv0;
        glm::vec4 tangent;
        glm::vec2 uv1;
    };

    struct CanonicalMeshPushConstants {
        glm::mat4 renderMatrix;
        uint32_t materialIndex = 0;
        uint32_t padding[3]{};
    };

    static_assert(sizeof(CanonicalMeshPushConstants) == 80);
    static_assert(offsetof(CanonicalMeshPushConstants, materialIndex) == 64);

    struct UniformBufferObject {
        alignas(16) glm::mat4 model;
        alignas(16) glm::mat4 view;
        alignas(16) glm::mat4 proj;
        alignas(16) glm::mat4 inverseView;
        alignas(16) glm::mat4 inverseProjection;
        alignas(16) glm::vec4 cameraPosition;
        alignas(16) glm::vec4 depthRange;
        alignas(16) glm::uvec4 renderInfo;
        alignas(16) glm::vec4 worldUnits;
    };

    static_assert(sizeof(UniformBufferObject) == 384);
    static_assert(offsetof(UniformBufferObject, inverseProjection) == 256);
    static_assert(offsetof(UniformBufferObject, worldUnits) == 368);

    enum class AlphaMode {
        Opaque,
        Mask,
        Blend
    };

    struct ModelAsset {
        std::string filePath;
        AssetGuid assetGuid;
        std::string artifactCookKey;
        TransparencyExecutionMode transparencyExecutionMode =
            TransparencyExecutionMode::LegacyTwoBucket;

        // The single ticket to the GPU buffers
        GeometryHandle geometry;

        uint32_t totalIndices = 0;
        std::vector<SubMesh> subMeshes;

        // Material bindings carry material, PSO, queue, and opaque-sort identity together.
        std::vector<MaterialBinding> materials;

        // Textures allocated while loading this model; ownership remains RHI-handle only.
        std::vector<TextureHandle> ownedTextures;

        // Cooked runtime models own their uploaded geometry while material
        // revisions remain owned by the asset-runtime publisher.
        bool ownsGeometry = true;
        bool ownsMaterials = true;
        bool ownsTextures = true;
    };

    // Matches Application's classified queue routing without constructing draw
    // packets. A classified SortedSurface uses only the sorted forward pass;
    // every other transparent route currently consumes the shared refraction
    // pyramids. Invalid material indices cannot produce a draw and are ignored.
    [[nodiscard]] inline bool modelRequiresRefractionPyramids(
        const ModelAsset& model) noexcept {
        for (const SubMesh& subMesh : model.subMeshes) {
            if (subMesh.materialIndex < 0 ||
                static_cast<size_t>(subMesh.materialIndex) >=
                    model.materials.size()) {
                continue;
            }
            if (model.materials[static_cast<size_t>(subMesh.materialIndex)].
                    renderQueue != RenderQueue::Transparent) {
                continue;
            }
            if (model.transparencyExecutionMode ==
                    TransparencyExecutionMode::Classified &&
                subMesh.transparency.resolvedClass ==
                    TransparencyClass::SortedSurface) {
                continue;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] inline bool modelRequiresLayeredInterfaces(
        const ModelAsset& model, TransparencyQuality quality) noexcept {
        if (model.transparencyExecutionMode !=
            TransparencyExecutionMode::Classified) {
            return false;
        }
        for (const SubMesh& subMesh : model.subMeshes) {
            if (subMesh.materialIndex < 0 ||
                static_cast<size_t>(subMesh.materialIndex) >=
                    model.materials.size()) {
                continue;
            }
            if (model.materials[static_cast<size_t>(subMesh.materialIndex)].
                    renderQueue == RenderQueue::Transparent &&
                subMesh.transparency.resolvedClass ==
                    TransparencyClass::LayeredGlass &&
                subMesh.transparency.quality == quality) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline bool modelRequiresOrdinary2LayeredInterfaces(
        const ModelAsset& model) noexcept {
        return modelRequiresLayeredInterfaces(model,
            TransparencyQuality::Ordinary2);
    }

    [[nodiscard]] inline bool modelRequiresHero4LayeredInterfaces(
        const ModelAsset& model) noexcept {
        return modelRequiresLayeredInterfaces(model,
            TransparencyQuality::Hero4);
    }

    [[nodiscard]] inline bool modelRequiresCinematic8LayeredInterfaces(
        const ModelAsset& model) noexcept {
        return modelRequiresLayeredInterfaces(model,
            TransparencyQuality::Cinematic8);
    }

} // namespace Iridium
