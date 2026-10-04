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

    void buildGpuSceneIndirectPlan(std::span<const uint32_t> primitiveIndices,
        size_t directPacketCount, const GpuSceneIndirectPolicy& policy,
        GpuSceneIndirectPlan& result,
        std::span<const GpuScenePrimitiveRecord> primitives,
        std::span<const GpuSceneGeometryRecord> geometries) {
        result.abiVersion = GpuSceneIndirectAbiVersion;
        result.fallbackReason = GpuSceneIndirectFallbackReason::None;
        result.commands.clear();
        result.packetIndices.clear();

        // The packet plan's checks, in its order, over the same entry count.
        const size_t count = primitiveIndices.size() + directPacketCount;
        if (count == 0u) return;
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
        if (count < policy.minimumCommandCount) {
            result.fallbackReason =
                GpuSceneIndirectFallbackReason::TinyWorkload;
            return;
        }
        if (count > policy.maxDrawIndirectCount ||
            count > std::numeric_limits<uint32_t>::max()) {
            result.fallbackReason =
                GpuSceneIndirectFallbackReason::CapacityExceeded;
            return;
        }
        if (directPacketCount != 0u) {
            result.fallbackReason = GpuSceneIndirectFallbackReason::InvalidPacket;
            return;
        }

        result.commands.reserve(primitiveIndices.size());
        result.packetIndices.reserve(primitiveIndices.size());
        for (uint32_t index = 0;
                index < static_cast<uint32_t>(primitiveIndices.size()); ++index) {
            const uint32_t primitiveIndex = primitiveIndices[index];
            const GpuSceneGeometryRecord* geometry = nullptr;
            if (primitiveIndex < primitives.size() &&
                primitives[primitiveIndex].binding.y < geometries.size())
                geometry = &geometries[primitives[primitiveIndex].binding.y];
            if (geometry == nullptr || geometry->draw.y == 0u) {
                result.commands.clear();
                result.packetIndices.clear();
                result.fallbackReason =
                    GpuSceneIndirectFallbackReason::InvalidPacket;
                return;
            }
            result.commands.push_back({
                .indexCount = geometry->draw.y,
                .instanceCount = 1u,
                .firstIndex = geometry->draw.x,
                .vertexOffset = std::bit_cast<int32_t>(geometry->draw.z),
                .firstInstance = primitiveIndex,
            });
            result.packetIndices.push_back(index);
        }
    }

} // namespace Iridium
