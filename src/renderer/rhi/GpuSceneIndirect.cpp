#include "GpuSceneIndirect.h"

#include <limits>
#include <bit>

namespace Iridium {

    void buildGpuSceneIndirectPlan(std::span<const DrawPacket> packets,
        const GpuSceneIndirectPolicy& policy, GpuSceneIndirectPlan& result,
        std::span<const GpuScenePrimitiveRecord> primitives,
        std::span<const GpuSceneGeometryRecord> geometries) {
        result.abiVersion = GpuSceneIndirectAbiVersion;
        result.fallbackReason = GpuSceneIndirectFallbackReason::None;
        result.commands.clear();
        result.packetIndices.clear();

        if (packets.empty()) return;
        if (policy.forceDirectReference) {
            result.fallbackReason = GpuSceneIndirectFallbackReason::DirectReference;
            return;
        }
        if (!policy.multiDrawIndirect ||
            !policy.drawIndirectFirstInstance ||
            !policy.drawIndirectCount ||
            policy.maxDrawIndirectCount == 0u) {
            result.fallbackReason =
                GpuSceneIndirectFallbackReason::MissingCapability;
            return;
        }
        if (packets.size() < policy.minimumCommandCount) {
            result.fallbackReason =
                GpuSceneIndirectFallbackReason::TinyWorkload;
            return;
        }
        if (packets.size() > policy.maxDrawIndirectCount ||
            packets.size() > std::numeric_limits<uint32_t>::max()) {
            result.fallbackReason =
                GpuSceneIndirectFallbackReason::CapacityExceeded;
            return;
        }

        result.commands.reserve(packets.size());
        result.packetIndices.reserve(packets.size());
        for (uint32_t index = 0;
                index < static_cast<uint32_t>(packets.size()); ++index) {
            const DrawPacket& packet = packets[index];
            const GpuSceneGeometryRecord* geometry = nullptr;
            bool validReference = true;
            if (!primitives.empty() || !geometries.empty()) {
                validReference = packet.firstInstanceTransform < primitives.size();
                if (validReference) {
                    const uint32_t geometryIndex = primitives[packet.firstInstanceTransform].binding.y;
                    validReference = geometryIndex < geometries.size();
                    if (validReference) geometry = &geometries[geometryIndex];
                }
                validReference = validReference && geometry &&
                    geometry->draw.x == packet.firstIndex && geometry->draw.y == packet.indexCount;
            }
            if (!hasGpuScenePrimitive(packet) || packet.indexCount == 0u ||
                packet.instanceCount != 1u || !validReference) {
                result.commands.clear();
                result.packetIndices.clear();
                result.fallbackReason =
                    GpuSceneIndirectFallbackReason::InvalidPacket;
                return;
            }
            result.commands.push_back({
                .indexCount = packet.indexCount,
                .instanceCount = 1u,
                .firstIndex = packet.firstIndex,
                .vertexOffset = geometry ? std::bit_cast<int32_t>(geometry->draw.z) : 0,
                .firstInstance = packet.firstInstanceTransform,
            });
            result.packetIndices.push_back(index);
        }
    }

} // namespace Iridium
