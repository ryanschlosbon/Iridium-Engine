#include "GpuScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>

namespace Iridium {
namespace {
    bool finite(const glm::vec4& v) noexcept {
        return std::isfinite(v.x) && std::isfinite(v.y) &&
            std::isfinite(v.z) && std::isfinite(v.w);
    }
    bool finite(const glm::mat4& m) noexcept {
        for (uint32_t column = 0; column < 4; ++column)
            if (!finite(m[column])) return false;
        return true;
    }
    bool validBounds(const glm::vec4& sphere, const glm::vec4& minimum,
        const glm::vec4& maximum) noexcept {
        return finite(sphere) && sphere.w >= 0.0f && finite(minimum) &&
            finite(maximum) && minimum.x <= maximum.x &&
            minimum.y <= maximum.y && minimum.z <= maximum.z;
    }
    GpuSceneFloat4 pack(const glm::vec4& v) noexcept {
        return { v.x, v.y, v.z, v.w };
    }
}

    GpuSceneAffineTransform packGpuSceneAffine(const glm::mat4& transform) {
        if (!finite(transform))
            throw std::invalid_argument("GPU-scene affine transform is non-finite");
        if (transform[0][3] != 0.0f || transform[1][3] != 0.0f ||
            transform[2][3] != 0.0f || transform[3][3] != 1.0f)
            throw std::invalid_argument("GPU-scene transform must be affine");
        return {
            .row0 = { transform[0][0], transform[1][0], transform[2][0], transform[3][0] },
            .row1 = { transform[0][1], transform[1][1], transform[2][1], transform[3][1] },
            .row2 = { transform[0][2], transform[1][2], transform[2][2], transform[3][2] },
        };
    }

    glm::vec3 transformGpuScenePoint(const GpuSceneAffineTransform& t,
        glm::vec3 p) noexcept {
        return {
            t.row0.x*p.x + t.row0.y*p.y + t.row0.z*p.z + t.row0.w,
            t.row1.x*p.x + t.row1.y*p.y + t.row1.z*p.z + t.row1.w,
            t.row2.x*p.x + t.row2.y*p.y + t.row2.z*p.z + t.row2.w,
        };
    }

    glm::mat4 unpackGpuSceneAffine(
        const GpuSceneAffineTransform& t) noexcept {
        glm::mat4 result(1.0f);
        result[0] = { t.row0.x, t.row1.x, t.row2.x, 0.0f };
        result[1] = { t.row0.y, t.row1.y, t.row2.y, 0.0f };
        result[2] = { t.row0.z, t.row1.z, t.row2.z, 0.0f };
        result[3] = { t.row0.w, t.row1.w, t.row2.w, 1.0f };
        return result;
    }

    void collectGpuSceneConsumerPrimitiveIndices(
        const GpuScenePackedTables& scene, uint32_t consumerMask,
        std::vector<uint32_t>& destination) {
        destination.clear();
        destination.reserve(scene.primitives.size());
        if (consumerMask == 0u) return;
        for (uint32_t primitiveIndex = 0;
            primitiveIndex < scene.primitives.size(); ++primitiveIndex) {
            const GpuScenePrimitiveRecord& primitive =
                scene.primitives[primitiveIndex];
            if ((primitive.state.w & consumerMask) != consumerMask ||
                primitive.binding.x >= scene.instances.size())
                continue;
            const GpuSceneInstanceRecord& instance =
                scene.instances[primitive.binding.x];
            if ((instance.state.z & GpuSceneInstanceEnabled) == 0u ||
                (instance.state.w & consumerMask) != consumerMask)
                continue;
            destination.push_back(primitiveIndex);
        }
    }

    void publishGpuSceneConsumerMembership(GpuScenePackedTables& scene) {
        collectGpuSceneConsumerPrimitiveIndices(scene, GpuSceneConsumerShadow,
            scene.shadowConsumerPrimitiveIndices);
        collectGpuSceneConsumerPrimitiveIndices(scene, GpuSceneConsumerProbe,
            scene.probeConsumerPrimitiveIndices);

        const auto revisionFor = [&scene](uint32_t consumerMask,
            std::span<const uint32_t> primitiveIndices) noexcept {
            // FNV-1a over only data that can alter membership or the derived
            // geometry/material bins. World transforms, bounds, and their
            // revisions remain deliberately absent so movable instances keep
            // a stable membership revision.
            uint64_t hash = 1469598103934665603ull;
            const auto mix = [&hash](uint64_t value) noexcept {
                for (uint32_t byte = 0; byte < 8u; ++byte) {
                    hash ^= (value >> (byte * 8u)) & 0xffu;
                    hash *= 1099511628211ull;
                }
            };
            mix(scene.sceneEpoch);
            mix(consumerMask);
            mix(primitiveIndices.size());
            for (const uint32_t primitiveIndex : primitiveIndices) {
                mix(primitiveIndex);
                if (primitiveIndex >= scene.primitives.size()) continue;
                const GpuScenePrimitiveRecord& primitive =
                    scene.primitives[primitiveIndex];
                mix(primitive.binding.x); mix(primitive.binding.y);
                mix(primitive.binding.z); mix(primitive.binding.w);
                mix(primitive.state.x); mix(primitive.state.y);
                mix(primitive.state.w);
                mix(primitive.revisions.x); mix(primitive.revisions.y);
                mix(primitive.revisions.z); mix(primitive.revisions.w);
                if (primitive.binding.x < scene.instances.size()) {
                    const GpuSceneInstanceRecord& instance =
                        scene.instances[primitive.binding.x];
                    mix(instance.state.x);
                    mix(instance.state.z);
                    mix(instance.state.w);
                }
                uint32_t geometryIndex = primitive.binding.y;
                for (uint32_t lod = 0; lod < MaximumGpuSceneLodLevels &&
                    geometryIndex < scene.geometries.size(); ++lod) {
                    const GpuSceneGeometryRecord& geometry =
                        scene.geometries[geometryIndex];
                    mix(geometryIndex);
                    mix(geometry.draw.x); mix(geometry.draw.y);
                    mix(geometry.draw.z); mix(geometry.draw.w);
                    mix(geometry.storage.x); mix(geometry.storage.y);
                    mix(geometry.storage.z); mix(geometry.storage.w);
                    mix(geometry.state.x); mix(geometry.state.z);
                    mix(geometry.state.w);
                    if (geometryIndex < scene.geometryRevisions.size())
                        mix(scene.geometryRevisions[geometryIndex]);
                    if (geometry.state.z == InvalidGpuSceneIndex ||
                        geometry.state.z == geometryIndex)
                        break;
                    geometryIndex = geometry.state.z;
                }
            }
            return hash == 0u ? 1u : hash;
        };
        scene.shadowConsumerMembershipRevision = revisionFor(
            GpuSceneConsumerShadow, scene.shadowConsumerPrimitiveIndices);
        scene.probeConsumerMembershipRevision = revisionFor(
            GpuSceneConsumerProbe, scene.probeConsumerPrimitiveIndices);
    }

    GpuScenePackedTables packGpuSceneReference(
        const GpuScenePublicationInput& input, const GpuSceneCapacity& capacity) {
        GpuScenePackedTables result;
        result.sceneEpoch = input.sceneEpoch;
        result.publicationRevision = input.publicationRevision;

        std::vector<const GpuSceneGeometrySource*> geometryOrder;
        for (const auto& geometry : input.geometries) geometryOrder.push_back(&geometry);
        std::ranges::sort(geometryOrder, {}, [](const auto* source) {
            return source->handle;
        });
        std::map<GpuSceneGeometryHandle, uint32_t> geometrySlots;
        std::vector<const GpuSceneGeometrySource*> acceptedGeometrySources;
        std::set<GpuSceneGeometryHandle> seenGeometry;
        for (const GpuSceneGeometrySource* source : geometryOrder) {
            if (result.geometries.size() >= capacity.maximumGeometries) break;
            if (!source->handle.isValid() ||
                !seenGeometry.insert(source->handle).second ||
                source->indexCount == 0 ||
                source->indexType > 1 || !std::isfinite(source->geometricError) ||
                source->geometricError < 0.0f || source->lodLevel >= MaximumGpuSceneLodLevels ||
                (source->lodLevel == 0 && source->geometricError != 0.0f) ||
                source->vertexArena == InvalidGpuSceneIndex ||
                source->indexArena == InvalidGpuSceneIndex ||
                !validBounds(source->localBoundsSphere, source->localBoundsMin,
                    source->localBoundsMax)) {
                ++result.invalidSourceCount;
                continue;
            }
            const uint32_t denseSlot = static_cast<uint32_t>(result.geometries.size());
            geometrySlots.emplace(source->handle, denseSlot);
            acceptedGeometrySources.push_back(source);
            result.geometries.push_back({
                .localBoundsSphere = pack(source->localBoundsSphere),
                .localBoundsMin = pack(source->localBoundsMin),
                .localBoundsMax = pack(source->localBoundsMax),
                .draw = { source->firstIndex, source->indexCount,
                    std::bit_cast<uint32_t>(source->vertexOffset), source->indexType },
                .storage = { source->vertexArena, source->indexArena,
                    source->vertexLayout, source->flags },
                .state = { source->handle.generation,
                    static_cast<uint32_t>(result.geometryIdentities.size()),
                    InvalidGpuSceneIndex, source->productRevision },
            });
            result.geometries.back().localBoundsMin.w = source->geometricError;
            result.geometries.back().localBoundsMax.w = static_cast<float>(source->lodLevel);
            result.geometryIdentities.push_back(source->identity);
            result.denseGeometryHandles.push_back(source->handle);
            result.geometryRevisions.push_back(source->recordRevision);
        }

        for (const auto* source : acceptedGeometrySources) {
            const auto base = geometrySlots.find(source->handle);
            if (base == geometrySlots.end() || !source->coarserLod.isValid()) continue;
            const auto next = geometrySlots.find(source->coarserLod);
            if (next != geometrySlots.end()) result.geometries[base->second].state.z = next->second;
            else ++result.invalidSourceCount; // Missing child retains LOD0.
        }

        std::vector<const GpuSceneInstanceSource*> instanceOrder;
        for (const auto& instance : input.instances) instanceOrder.push_back(&instance);
        std::ranges::sort(instanceOrder, {}, [](const auto* source) {
            return source->handle;
        });
        std::set<GpuSceneInstanceHandle> seenInstance;
        std::set<GpuScenePrimitiveHandle> seenPrimitive;
        for (const GpuSceneInstanceSource* source : instanceOrder) {
            bool invalid = !source->handle.isValid() ||
                !seenInstance.insert(source->handle).second ||
                !finite(source->currentWorld) || !finite(source->previousWorld) ||
                source->primitives.empty();
            std::vector<const GpuScenePrimitiveSource*> primitiveOrder;
            for (const auto& primitive : source->primitives) {
                primitiveOrder.push_back(&primitive);
                invalid = invalid || !primitive.handle.isValid() ||
                    !primitive.material.isValid() || !primitive.pipeline.isValid() ||
                    !geometrySlots.contains(primitive.geometry) ||
                    seenPrimitive.contains(primitive.handle);
            }
            std::ranges::sort(primitiveOrder, {}, [](const auto* source) {
                return source->handle;
            });
            const bool overflow = result.instances.size() >= capacity.maximumInstances ||
                result.transforms.size() + 2u > capacity.maximumTransforms ||
                primitiveOrder.size() > static_cast<size_t>(capacity.maximumPrimitives) -
                    result.primitives.size();
            if (invalid || overflow) {
                result.directFallbackInstances.push_back(source->handle);
                if (invalid) ++result.invalidSourceCount;
                else ++result.capacityOmittedInstanceCount;
                continue;
            }

            GpuSceneAffineTransform current;
            GpuSceneAffineTransform previous;
            try {
                current = packGpuSceneAffine(source->currentWorld);
                previous = packGpuSceneAffine(source->previousWorld);
            } catch (const std::invalid_argument&) {
                result.directFallbackInstances.push_back(source->handle);
                ++result.invalidSourceCount;
                continue;
            }
            const uint32_t instanceSlot = static_cast<uint32_t>(result.instances.size());
            const uint32_t currentTransform = static_cast<uint32_t>(result.transforms.size());
            result.transforms.push_back(current);
            result.transforms.push_back(previous);
            result.transformRevisions.push_back(source->currentTransformRevision);
            result.transformRevisions.push_back(source->previousTransformRevision);

            uint32_t flags = source->flags;
            glm::vec4 sphere = source->worldBoundsSphere;
            glm::vec4 minimum = source->worldBoundsMin;
            glm::vec4 maximum = source->worldBoundsMax;
            if (!validBounds(sphere, minimum, maximum)) {
                sphere = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
                minimum = maximum = glm::vec4(0.0f);
                flags |= GpuSceneInstanceInvalidBoundsFailVisible;
            }
            const uint32_t firstPrimitive = static_cast<uint32_t>(result.primitives.size());
            result.instances.push_back({
                .worldBoundsSphere = pack(sphere),
                .worldBoundsMin = pack(minimum),
                .worldBoundsMax = pack(maximum),
                .references = { currentTransform, currentTransform + 1u,
                    firstPrimitive, static_cast<uint32_t>(primitiveOrder.size()) },
                .state = { source->handle.generation,
                    static_cast<uint32_t>(source->mobility),
                    packGpuSceneInstanceFlags(flags, source->maximumLod),
                    source->consumerMask },
            });
            result.instanceIdentities.push_back(source->identity);
            result.denseInstanceHandles.push_back(source->handle);
            result.instanceRevisions.push_back(source->instanceRevision);
            for (const GpuScenePrimitiveSource* primitive : primitiveOrder) {
                seenPrimitive.insert(primitive->handle);
                result.primitives.push_back({
                    .binding = { instanceSlot, geometrySlots.at(primitive->geometry),
                        primitive->material.id, primitive->pipeline.id },
                    .state = { primitive->handle.generation, primitive->flags,
                        static_cast<uint32_t>(result.primitiveIdentities.size()),
                        primitive->consumerMask },
                    .revisions = { primitive->primitiveLayoutRevision,
                        primitive->geometryProductRevision,
                        primitive->instanceBindingRevision,
                        primitive->materialRevision },
                });
                result.primitiveIdentities.push_back(primitive->identity);
                result.densePrimitiveHandles.push_back(primitive->handle);
                result.primitiveRevisions.push_back(primitive->recordRevision);
            }
        }
        publishGpuSceneConsumerMembership(result);
        return result;
    }

} // namespace Iridium
