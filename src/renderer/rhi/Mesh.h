#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "assets/AssetGuid.h"
#include "PipelineTypes.h"

namespace Iridium {

    struct SubMesh {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        int materialIndex = -1;
        AssetGuid primitiveGuid;
        AssetGuid materialGuid;
        uint32_t sourceNode = 0;
        uint32_t sourceMesh = 0;
        uint32_t sourcePrimitive = 0;
        uint32_t attributeMask = 0;
        uint32_t flags = 0;
        uint8_t coverage = 0;
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
    };

    enum class AlphaMode {
        Opaque,
        Mask,
        Blend
    };

    struct ModelAsset {
        std::string filePath;
        AssetGuid assetGuid;
        std::string artifactCookKey;

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

} // namespace Iridium
