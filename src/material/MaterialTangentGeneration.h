#pragma once

#include "material/StandardMaterialShading.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Iridium {

    struct TangentGenerationStats {
        uint32_t triangleCount = 0;
        uint32_t degenerateUvTriangleCount = 0;
        uint32_t fallbackVertexCount = 0;
    };

    // MikkTSpace-compatible convention: tangent.xyz follows increasing U and
    // tangent.w is the sign required to reconstruct B = cross(N, T) * w.
    // Importers must split vertices at UV/normal/orientation discontinuities.
    template <typename VertexType>
    TangentGenerationStats generateMikkCompatibleTangents(
        std::span<VertexType> vertices, std::span<const uint32_t> indices,
        uint32_t globalVertexBase = 0) {
        if ((indices.size() % 3u) != 0u)
            throw std::invalid_argument("tangent generation requires triangle indices");
        std::vector<glm::vec3> accumulatedTangents(vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> accumulatedBitangents(vertices.size(), glm::vec3(0.0f));
        TangentGenerationStats stats{};
        stats.triangleCount = static_cast<uint32_t>(indices.size() / 3u);
        for (size_t triangle = 0; triangle < indices.size(); triangle += 3u) {
            uint32_t local[3]{};
            for (uint32_t corner = 0; corner < 3; ++corner) {
                if (indices[triangle + corner] < globalVertexBase)
                    throw std::out_of_range("tangent index precedes vertex span");
                local[corner] = indices[triangle + corner] - globalVertexBase;
                if (local[corner] >= vertices.size())
                    throw std::out_of_range("tangent index exceeds vertex span");
            }
            const glm::vec3 edge1 = vertices[local[1]].pos - vertices[local[0]].pos;
            const glm::vec3 edge2 = vertices[local[2]].pos - vertices[local[0]].pos;
            const glm::vec2 uv1 = vertices[local[1]].uv0 - vertices[local[0]].uv0;
            const glm::vec2 uv2 = vertices[local[2]].uv0 - vertices[local[0]].uv0;
            const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
            if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f ||
                glm::dot(glm::cross(edge1, edge2), glm::cross(edge1, edge2)) <=
                    MaterialNormalEpsilon) {
                ++stats.degenerateUvTriangleCount;
                continue;
            }
            const float reciprocal = 1.0f / determinant;
            const glm::vec3 tangent = (edge1 * uv2.y - edge2 * uv1.y) * reciprocal;
            const glm::vec3 bitangent = (edge2 * uv1.x - edge1 * uv2.x) * reciprocal;
            for (uint32_t corner = 0; corner < 3; ++corner) {
                accumulatedTangents[local[corner]] += tangent;
                accumulatedBitangents[local[corner]] += bitangent;
            }
        }
        for (size_t index = 0; index < vertices.size(); ++index) {
            const glm::vec3 normal = glm::normalize(vertices[index].normal);
            glm::vec3 tangent = accumulatedTangents[index] - normal *
                glm::dot(normal, accumulatedTangents[index]);
            if (!finiteMaterialVector(tangent) ||
                glm::dot(tangent, tangent) <= MaterialNormalEpsilon) {
                tangent = materialFallbackTangent(normal);
                ++stats.fallbackVertexCount;
            }
            else tangent = glm::normalize(tangent);
            float handedness = 1.0f;
            if (finiteMaterialVector(accumulatedBitangents[index]) &&
                glm::dot(accumulatedBitangents[index], accumulatedBitangents[index]) >
                    MaterialNormalEpsilon)
                handedness = glm::dot(glm::cross(normal, tangent),
                    accumulatedBitangents[index]) < 0.0f ? -1.0f : 1.0f;
            vertices[index].tangent = glm::vec4(tangent, handedness);
        }
        return stats;
    }

} // namespace Iridium
