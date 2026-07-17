#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "PipelineTypes.h"

namespace Iridium {

    struct Node;

    struct SubMesh {
        uint32_t indexStart;
        uint32_t indexCount;
        int materialIndex;
    };

    struct BakedMesh {
        int subMeshIndex;
        glm::mat4 transform;
    };

    // The Vertex is Plain Old Data (POD)
    struct Vertex {
        glm::vec3 pos;
        glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
        glm::vec3 normal;
        glm::vec2 uv;
        glm::vec4 tangent;
    };

    struct MeshPushConstants {
        glm::mat4 renderMatrix;
        glm::vec4 baseColor;
        glm::vec4 emissiveFactor;
        float metallicFactor;
        float roughnessFactor;
        float normalScale;
        float alphaCutoff = 0.0f;
        float transmissionFactor = 0.0f;
        float padding = 0.0f;
    };

    static_assert(sizeof(MeshPushConstants) == 120);
    static_assert(offsetof(MeshPushConstants, renderMatrix) == 0);
    static_assert(offsetof(MeshPushConstants, baseColor) == 64);
    static_assert(offsetof(MeshPushConstants, emissiveFactor) == 80);
    static_assert(offsetof(MeshPushConstants, metallicFactor) == 96);
    static_assert(offsetof(MeshPushConstants, transmissionFactor) == 112);

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

        // The single ticket to the GPU buffers
        GeometryHandle geometry;

        uint32_t totalIndices;
        std::vector<SubMesh> subMeshes;
        std::map<int, std::vector<int>> meshToSubMeshes;

        // Material bindings carry material, PSO, queue, and opaque-sort identity together.
        std::vector<MaterialBinding> materials;

        // Textures allocated while loading this model; ownership remains RHI-handle only.
        std::vector<TextureHandle> ownedTextures;

        std::vector<std::unique_ptr<Node>> rootNodes;

        struct BakedPart {
            int subMeshIndex;
            glm::mat4 transform;
        };

        std::map<int, std::vector<BakedPart>> materialBuckets;
    };

} // namespace Iridium
