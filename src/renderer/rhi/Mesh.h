#pragma once
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "RenderHandles.h" 

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
        float metallicFactor;
        float roughnessFactor;
        float emissiveFactor;
        float padding;
    };

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

        // MaterialHandles allow multiple models to share the exact same material safely
        std::vector<MaterialHandle> materials;

        bool isTransparent = false;
        std::vector<bool> materialIsTransparent;

        std::vector<std::unique_ptr<Node>> rootNodes;

        struct BakedPart {
            int subMeshIndex;
            glm::mat4 transform;
        };

        std::map<int, std::vector<BakedPart>> materialBuckets;
    };

} // namespace Iridium