#include "GpuScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
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

    uint64_t gpuSceneConsumerContentWatermark(const GpuScenePackedTables& scene,
        std::span<const uint32_t> primitiveIndices) noexcept {
        uint64_t watermark = 0;
        for (const uint32_t primitiveIndex : primitiveIndices) {
            if (primitiveIndex >= scene.primitives.size()) continue;
            if (primitiveIndex < scene.primitiveRevisions.size())
                watermark = (std::max)(watermark,
                    scene.primitiveRevisions[primitiveIndex]);
            const GpuScenePrimitiveRecord& primitive =
                scene.primitives[primitiveIndex];
            if (primitive.binding.x < scene.instances.size()) {
                const uint32_t transform =
                    scene.instances[primitive.binding.x].references.x;
                if (transform < scene.transformRevisions.size())
                    watermark = (std::max)(watermark,
                        scene.transformRevisions[transform]);
            }
            if (primitive.binding.y < scene.geometryRevisions.size())
                watermark = (std::max)(watermark,
                    scene.geometryRevisions[primitive.binding.y]);
        }
        return watermark;
    }

    void publishGpuSceneConsumerMembership(GpuScenePackedTables& scene) {
        collectGpuSceneConsumerPrimitiveIndices(scene, GpuSceneConsumerShadow,
            scene.shadowConsumerPrimitiveIndices);
        collectGpuSceneConsumerPrimitiveIndices(scene, GpuSceneConsumerProbe,
            scene.probeConsumerPrimitiveIndices);

        // M7R R5c.5: until R5c.5 each revision was an FNV-1a hash, one byte at
        // a time, over the epoch, the consumer mask, the list size and, per
        // member, the fields compared below (about 38 ms at H-stress
        // add_instances). The revision now advances exactly when that hash
        // input changed: the previous publication's inputs are kept in the
        // table's membership history and compared field by field.
        GpuSceneMembershipHistory& history = scene.membershipHistory;
        const bool comparable = history.valid &&
            history.sceneEpoch == scene.sceneEpoch;
        if (comparable) {
            history.instanceMemo.assign(scene.instances.size(), 0u);
            history.geometryMemo.assign(scene.geometries.size(), 0u);
        }
        const auto sameInstance = [&](uint32_t index) noexcept {
            // A member's instance index is below both table sizes: the
            // current list checks it and the equal previous record had it.
            uint8_t& memo = history.instanceMemo[index];
            if (memo == 0u) {
                const GpuSceneUint4& current = scene.instances[index].state;
                const GpuSceneUint4& previous = history.instanceStates[index];
                memo = current.x == previous.x && current.z == previous.z &&
                    current.w == previous.w ? 1u : 2u;
            }
            return memo == 1u;
        };
        // The LOD chain walk of the retired hash, over both tables at once.
        const auto sameChain = [&](uint32_t first) noexcept {
            const bool memoized = first < history.geometryMemo.size();
            if (memoized && history.geometryMemo[first] != 0u)
                return history.geometryMemo[first] == 1u;
            bool same = true;
            uint32_t geometryIndex = first;
            for (uint32_t lod = 0; lod < MaximumGpuSceneLodLevels; ++lod) {
                const bool inCurrent = geometryIndex < scene.geometries.size();
                const bool inPrevious =
                    geometryIndex < history.geometries.size();
                if (inCurrent != inPrevious) { same = false; break; }
                if (!inCurrent) break;
                const GpuSceneGeometryRecord& current =
                    scene.geometries[geometryIndex];
                const GpuSceneGeometryRecord& previous =
                    history.geometries[geometryIndex];
                const bool currentRevision =
                    geometryIndex < scene.geometryRevisions.size();
                const bool previousRevision =
                    geometryIndex < history.geometryRevisions.size();
                if (current.draw.x != previous.draw.x ||
                    current.draw.y != previous.draw.y ||
                    current.draw.z != previous.draw.z ||
                    current.draw.w != previous.draw.w ||
                    current.storage.x != previous.storage.x ||
                    current.storage.y != previous.storage.y ||
                    current.storage.z != previous.storage.z ||
                    current.storage.w != previous.storage.w ||
                    current.state.x != previous.state.x ||
                    current.state.z != previous.state.z ||
                    current.state.w != previous.state.w ||
                    currentRevision != previousRevision ||
                    (currentRevision && scene.geometryRevisions[geometryIndex] !=
                        history.geometryRevisions[geometryIndex])) {
                    same = false;
                    break;
                }
                if (current.state.z == InvalidGpuSceneIndex ||
                    current.state.z == geometryIndex)
                    break;
                geometryIndex = current.state.z;
            }
            if (memoized) history.geometryMemo[first] = same ? 1u : 2u;
            return same;
        };
        const auto membershipChanged = [&](std::span<const uint32_t> members,
            std::span<const uint32_t> previousMembers) noexcept {
            if (!comparable || members.size() != previousMembers.size() ||
                (!members.empty() && std::memcmp(members.data(),
                    previousMembers.data(),
                    members.size() * sizeof(uint32_t)) != 0))
                return true;
            for (const uint32_t primitiveIndex : members) {
                const GpuScenePrimitiveRecord& current =
                    scene.primitives[primitiveIndex];
                const GpuScenePrimitiveRecord& previous =
                    history.primitives[primitiveIndex];
                if (current.binding.x != previous.binding.x ||
                    current.binding.y != previous.binding.y ||
                    current.binding.z != previous.binding.z ||
                    current.binding.w != previous.binding.w ||
                    current.state.x != previous.state.x ||
                    current.state.y != previous.state.y ||
                    current.state.w != previous.state.w ||
                    current.revisions.x != previous.revisions.x ||
                    current.revisions.y != previous.revisions.y ||
                    current.revisions.z != previous.revisions.z ||
                    current.revisions.w != previous.revisions.w ||
                    !sameInstance(current.binding.x) ||
                    !sameChain(current.binding.y))
                    return true;
            }
            return false;
        };
        const bool shadowChanged = membershipChanged(
            scene.shadowConsumerPrimitiveIndices, history.shadowMembers);
        const bool probeChanged = membershipChanged(
            scene.probeConsumerPrimitiveIndices, history.probeMembers);
        if (shadowChanged)
            scene.shadowConsumerMembershipRevision = history.nextRevision++;
        if (probeChanged)
            scene.probeConsumerMembershipRevision = history.nextRevision++;
        // Unchanged lists compared equal on every input they read, so the
        // stored inputs stay exact for them; refresh after any change.
        if (shadowChanged || probeChanged) {
            history.valid = true;
            history.sceneEpoch = scene.sceneEpoch;
            history.shadowMembers = scene.shadowConsumerPrimitiveIndices;
            history.probeMembers = scene.probeConsumerPrimitiveIndices;
            history.primitives = scene.primitives;
            history.instanceStates.resize(scene.instances.size());
            for (size_t index = 0; index < scene.instances.size(); ++index)
                history.instanceStates[index] = scene.instances[index].state;
            history.geometries = scene.geometries;
            history.geometryRevisions = scene.geometryRevisions;
        }
        refreshGpuSceneConsumerContentWatermarks(scene);
    }

    void refreshGpuSceneConsumerContentWatermarks(
        GpuScenePackedTables& scene) noexcept {
        scene.shadowConsumerContentWatermark = gpuSceneConsumerContentWatermark(
            scene, scene.shadowConsumerPrimitiveIndices);
        scene.probeConsumerContentWatermark = gpuSceneConsumerContentWatermark(
            scene, scene.probeConsumerPrimitiveIndices);
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
