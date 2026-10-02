#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "ecs/Registry.h"
#include "renderer/rhi/Mesh.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/TransformComponent.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace Iridium {

    struct ViewportPickRay {
        glm::vec3 origin{};
        glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
    };

    [[nodiscard]] inline std::optional<ViewportPickRay> viewportPickRay(
        float mouseX, float mouseY, float viewportWidth, float viewportHeight,
        const glm::mat4& view, const glm::mat4& projection) noexcept {
        if (!std::isfinite(mouseX) || !std::isfinite(mouseY) ||
            !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
            viewportWidth <= 0.0f || viewportHeight <= 0.0f || mouseX < 0.0f ||
            mouseY < 0.0f || mouseX > viewportWidth ||
            mouseY > viewportHeight) {
            return std::nullopt;
        }
        const glm::vec2 ndc{
            mouseX / viewportWidth * 2.0f - 1.0f,
            mouseY / viewportHeight * 2.0f - 1.0f,
        };
        const glm::mat4 inverse = glm::inverse(projection * view);
        glm::vec4 nearPoint = inverse * glm::vec4(ndc, 0.0f, 1.0f);
        glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
        if (!std::isfinite(nearPoint.w) || !std::isfinite(farPoint.w) ||
            std::abs(nearPoint.w) <= 1.0e-7f ||
            std::abs(farPoint.w) <= 1.0e-7f) {
            return std::nullopt;
        }
        const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
        const glm::vec3 farWorld = glm::vec3(farPoint) / farPoint.w;
        const glm::vec3 delta = farWorld - origin;
        const float lengthSquared = glm::dot(delta, delta);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            return std::nullopt;
        }
        return ViewportPickRay{ origin, delta / std::sqrt(lengthSquared) };
    }

    [[nodiscard]] inline std::optional<float> intersectRayAabb(
        const ViewportPickRay& ray, glm::vec3 minimum,
        glm::vec3 maximum) noexcept {
        float entry = 0.0f;
        float exit = (std::numeric_limits<float>::max)();
        for (int axis = 0; axis < 3; ++axis) {
            const float origin = ray.origin[axis];
            const float direction = ray.direction[axis];
            if (std::abs(direction) <= 1.0e-8f) {
                if (origin < minimum[axis] || origin > maximum[axis]) {
                    return std::nullopt;
                }
                continue;
            }
            float first = (minimum[axis] - origin) / direction;
            float second = (maximum[axis] - origin) / direction;
            if (first > second) std::swap(first, second);
            entry = (std::max)(entry, first);
            exit = (std::min)(exit, second);
            if (exit < entry) return std::nullopt;
        }
        return exit >= 0.0f ? std::optional<float>(entry) : std::nullopt;
    }

    [[nodiscard]] inline Entity pickViewportEntity(
        const Registry& registry, float mouseX, float mouseY,
        float viewportWidth, float viewportHeight, const glm::mat4& view,
        const glm::mat4& projection) noexcept {
        const auto worldRay = viewportPickRay(mouseX, mouseY, viewportWidth,
            viewportHeight, view, projection);
        if (!worldRay) return NULL_ENTITY;
        const auto* meshes = registry.findPool<MeshComponent>();
        const auto* transforms = registry.findPool<TransformComponent>();
        if (!meshes || !transforms) return NULL_ENTITY;

        Entity nearest = NULL_ENTITY;
        float nearestDistance = (std::numeric_limits<float>::max)();
        for (size_t index = 0; index < meshes->entities.size(); ++index) {
            const Entity entity = meshes->entities[index];
            const MeshComponent& mesh = meshes->components[index];
            if (!mesh.enabled || !mesh.model || !transforms->has(entity)) {
                continue;
            }
            const glm::mat4 inverseWorld = glm::inverse(
                transforms->get(entity).worldMatrix);
            const ViewportPickRay localRay{
                glm::vec3(inverseWorld * glm::vec4(worldRay->origin, 1.0f)),
                glm::vec3(inverseWorld * glm::vec4(worldRay->direction, 0.0f)),
            };
            for (const SubMesh& subMesh : mesh.model->subMeshes) {
                const auto hit = intersectRayAabb(localRay,
                    subMesh.boundsMin, subMesh.boundsMax);
                if (hit && *hit < nearestDistance) {
                    nearestDistance = *hit;
                    nearest = entity;
                }
            }
        }
        return nearest;
    }

} // namespace Iridium
