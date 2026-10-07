#pragma once
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "core/types/AssetGuid.h"
#include "PipelineTypes.h"
#include "renderer/graph/ViewHistory.h"

namespace Iridium {

    enum class ViewProjectionKind : uint32_t {
        Perspective = 0,
        Orthographic = 1,
    };

    // CPU-side temporal ownership, deliberately separate from the shader UBO.
    // Callers change identity when switching views, and resetRevision on cuts.
    // One type with the render graph (M9 G1): identity 0 is "no view", so
    // producers always set an explicit identity.
    using ViewHistoryContext = RenderGraph::ViewHistoryContext;

    inline constexpr uint32_t ViewTransportRefractionPyramidsAvailable =
        1u << 0u;
    inline constexpr uint32_t ViewTransportDebugViewShift = 8u;
    inline constexpr uint32_t ViewTransportDebugViewMask =
        0xffu << ViewTransportDebugViewShift;
    static_assert((ViewTransportDebugViewMask &
        ViewTransportRefractionPyramidsAvailable) == 0u);

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
        // M9 G5b temporal fields. `projection`/`inverseProjection` stay
        // unjittered (culling, clustering, Hi-Z, LOD, shadows, probes and the
        // CPU frustum classification read them). Raster vertex stages and
        // depth reconstruction read the jittered pair, which equals the
        // unjittered pair bit for bit when jitter is off.
        alignas(16) glm::mat4 jitteredProjection{ 1.0f };
        alignas(16) glm::mat4 jitteredInverseProjection{ 1.0f };
        // This view's previous-turn unjittered view-projection (equals the
        // current one on a cut or first turn).
        alignas(16) glm::mat4 previousViewProjection{ 1.0f };
        // xy = current jitter in NDC, zw = previous turn's jitter in NDC.
        alignas(16) glm::vec4 jitter{ 0.0f };
        // x = jitter sequence index, y = turns since cut, z = ViewTemporal*
        // flags, w = jitter sequence length.
        alignas(16) glm::uvec4 temporalInfo{ 0u };
    };

    inline constexpr uint32_t ViewTemporalJitterActive = 1u << 0u;
    inline constexpr uint32_t ViewTemporalHistoryReset = 1u << 1u;

    static_assert(sizeof(ViewTransportRecord) == 544);
    static_assert(offsetof(ViewTransportRecord, view) == 0);
    static_assert(offsetof(ViewTransportRecord, projection) == 64);
    static_assert(offsetof(ViewTransportRecord, inverseView) == 128);
    static_assert(offsetof(ViewTransportRecord, inverseProjection) == 192);
    static_assert(offsetof(ViewTransportRecord, cameraPosition) == 256);
    static_assert(offsetof(ViewTransportRecord, depthRange) == 272);
    static_assert(offsetof(ViewTransportRecord, renderInfo) == 288);
    static_assert(offsetof(ViewTransportRecord, worldUnits) == 304);
    static_assert(offsetof(ViewTransportRecord, jitteredProjection) == 320);
    static_assert(offsetof(ViewTransportRecord, jitteredInverseProjection) == 384);
    static_assert(offsetof(ViewTransportRecord, previousViewProjection) == 448);
    static_assert(offsetof(ViewTransportRecord, jitter) == 512);
    static_assert(offsetof(ViewTransportRecord, temporalInfo) == 528);

    [[nodiscard]] inline ViewTransportRecord makeViewTransportRecord(
        const glm::mat4& view, const glm::mat4& projection,
        glm::vec3 cameraPosition, float nearPlane, float farPlane,
        glm::uvec2 renderExtent, ViewProjectionKind projectionKind =
            ViewProjectionKind::Perspective,
        float metresPerWorldUnit = 1.0f) noexcept {
        const glm::mat4 inverseProjection = glm::inverse(projection);
        return {
            .view = view,
            .projection = projection,
            .inverseView = glm::inverse(view),
            .inverseProjection = inverseProjection,
            .cameraPosition = glm::vec4(cameraPosition, 1.0f),
            .depthRange = glm::vec4(nearPlane, farPlane, 0.0f, 0.0f),
            .renderInfo = glm::uvec4(renderExtent,
                static_cast<uint32_t>(projectionKind), 0u),
            .worldUnits = glm::vec4(metresPerWorldUnit, 0.0f, 0.0f, 0.0f),
            // No jitter and no history until the extractor supplies them.
            .jitteredProjection = projection,
            .jitteredInverseProjection = inverseProjection,
            .previousViewProjection = projection * view,
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
        // Optional M7.3 per-primitive arena range. Legacy geometry falls back
        // to ModelAsset::geometry.
        GeometryHandle geometry;
        uint8_t indexFormat = 1; // 0 = UInt16, 1 = UInt32
        int32_t vertexOffset = 0;
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

    struct ModelLodLevel {
        float geometricError = 0.0f;
        SubMesh subMesh;
    };

    struct ModelLodChain {
        AssetGuid sourcePrimitiveGuid;
        AssetGuid basePrimitiveGuid;
        // Runtime-only physical residency provenance. Level zero remains the
        // stable canonical primitive identity even when its draw range has been
        // rebound to this coarser cooked level.
        uint32_t residentBaseLevel = 0;
        std::vector<ModelLodLevel> levels;
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

    // M9.1: direct draws that write velocity (G-buffer, forward-opaque) also
    // push last frame's world transform. Only the G-buffer and forward
    // pipeline layouts carry the larger range; every other layout keeps 80 B.
    struct CanonicalMotionPushConstants {
        CanonicalMeshPushConstants mesh{};
        alignas(16) glm::mat4 previousRenderMatrix{ 1.0f };
    };

    static_assert(sizeof(CanonicalMotionPushConstants) == 144);
    static_assert(offsetof(CanonicalMotionPushConstants, previousRenderMatrix) == 80);

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
        alignas(16) glm::mat4 jitteredProjection;
        alignas(16) glm::mat4 jitteredInverseProjection;
        alignas(16) glm::mat4 previousViewProjection;
        alignas(16) glm::vec4 jitter;
        alignas(16) glm::uvec4 temporalInfo;
    };

    // M9 G5a: every field is pinned; include/view_uniforms.glsl mirrors it.
    static_assert(sizeof(UniformBufferObject) == 608);
    static_assert(offsetof(UniformBufferObject, model) == 0);
    static_assert(offsetof(UniformBufferObject, view) == 64);
    static_assert(offsetof(UniformBufferObject, proj) == 128);
    static_assert(offsetof(UniformBufferObject, inverseView) == 192);
    static_assert(offsetof(UniformBufferObject, inverseProjection) == 256);
    static_assert(offsetof(UniformBufferObject, cameraPosition) == 320);
    static_assert(offsetof(UniformBufferObject, depthRange) == 336);
    static_assert(offsetof(UniformBufferObject, renderInfo) == 352);
    static_assert(offsetof(UniformBufferObject, worldUnits) == 368);
    static_assert(offsetof(UniformBufferObject, jitteredProjection) == 384);
    static_assert(offsetof(UniformBufferObject, jitteredInverseProjection) == 448);
    static_assert(offsetof(UniformBufferObject, previousViewProjection) == 512);
    static_assert(offsetof(UniformBufferObject, jitter) == 576);
    static_assert(offsetof(UniformBufferObject, temporalInfo) == 592);

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
            TransparencyExecutionMode::Classified;

        // The single ticket to the GPU buffers
        GeometryHandle geometry;
        std::vector<GeometryHandle> geometryArena;

        uint32_t totalIndices = 0;
        uint64_t sourceIndexBytes = 0;
        uint64_t arenaIndexBytes = 0;
        uint64_t arenaSavedIndexBytes = 0;
        uint64_t arenaUInt16IndexCount = 0;
        uint64_t arenaUInt32IndexCount = 0;
        uint32_t lodResidentBaseLevel = 0;
        uint32_t lodFallbackChainCount = 0;
        uint32_t lodWithheldPrimitiveRangeCount = 0;
        uint64_t lodWithheldIndexBytes = 0;
        std::vector<SubMesh> subMeshes;
        // Optional finest-to-coarsest cooked chains. Level zero aliases the
        // canonical submesh identity; coarser levels own distinct geometry
        // identities but inherit its material and consumer semantics.
        std::vector<ModelLodChain> lodChains;

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
    // packets. SortedSurface and WeightedOIT do not consume refraction products;
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
                (subMesh.transparency.resolvedClass ==
                        TransparencyClass::SortedSurface ||
                    subMesh.transparency.resolvedClass ==
                        TransparencyClass::WeightedOit)) {
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

    [[nodiscard]] inline bool modelRequiresWeightedOit(
        const ModelAsset& model) noexcept {
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
                    TransparencyClass::WeightedOit) {
                return true;
            }
        }
        return false;
    }

} // namespace Iridium
