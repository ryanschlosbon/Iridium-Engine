#pragma once

// Qualification and test only (M7R R5c.5): the consumer membership revision
// that publishGpuSceneConsumerMembership computed until R5c.5, bit for bit, an
// FNV-1a hash taken one byte at a time over the scene epoch, the consumer mask,
// the list size and, per member, its primitive record (binding, state x/y/w,
// revisions), its instance's state x/z/w and its geometry LOD chain (index,
// draw, storage, state x/z/w, record revision). The production revision is now
// a counter that advances exactly when this hash's input changes; the caster
// revision oracle and the publisher tests compare change frames against it.

#include "renderer/rhi/GpuScene.h"

#include <cstdint>
#include <span>

namespace Iridium {

    [[nodiscard]] inline uint64_t legacyGpuSceneMembershipHash(
        const GpuScenePackedTables& scene, uint32_t consumerMask,
        std::span<const uint32_t> primitiveIndices) noexcept {
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
    }

} // namespace Iridium
