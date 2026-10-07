#include "renderer/rhi/GpuSceneIndirect.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <bit>
#include <vector>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition "\n"; \
    ++failures; } } while (false)

    DrawPacket packet(uint32_t primitive, uint32_t firstIndex = 0u) {
        DrawPacket value{};
        value.indexCount = 36u;
        value.firstIndex = firstIndex;
        value.executionFlags = DrawPacketGpuScenePrimitive;
        value.firstInstanceTransform = primitive;
        return value;
    }

    GpuSceneIndirectPolicy supported(uint32_t minimum = 1u) {
        return { true, true, true, 1024u, minimum };
    }
}

int main() {
    {
        const GpuSceneShadowIndirectCandidate candidate{
            17u, 3u, 41u, 9u };
        const auto words = std::bit_cast<std::array<uint32_t, 4>>(candidate);
        const std::array<uint32_t, 4> expected{ 17u, 3u, 41u, 9u };
        CHECK(GpuSceneShadowIndirectAbiVersion == 1u);
        CHECK(words == expected);
    }
    {
        const std::vector packets{ packet(0u, 12u), packet(1u, 48u) };
        std::vector<GpuScenePrimitiveRecord> primitives(2);
        primitives[0].binding.y = 0;
        primitives[1].binding.y = 1;
        std::vector<GpuSceneGeometryRecord> geometries(2);
        geometries[0].draw = { 12, 36, 4096, 0 };
        geometries[1].draw = { 48, 36, std::bit_cast<uint32_t>(-12), 1 };
        GpuSceneIndirectPlan plan;
        buildGpuSceneIndirectPlan(packets, supported(), plan, primitives, geometries);
        CHECK(plan.usesIndirect());
        CHECK(plan.commands[0].vertexOffset == 4096);
        CHECK(plan.commands[1].vertexOffset == -12);
        primitives[1].binding.y = 99;
        buildGpuSceneIndirectPlan(packets, supported(), plan, primitives, geometries);
        CHECK(plan.fallbackReason == GpuSceneIndirectFallbackReason::InvalidPacket);
        CHECK(plan.commands.empty());
        primitives[1].binding.y = 0; // Valid index, wrong draw range.
        buildGpuSceneIndirectPlan(packets, supported(), plan, primitives, geometries);
        CHECK(plan.fallbackReason == GpuSceneIndirectFallbackReason::InvalidPacket);
    }
    {
        const std::vector packets{ packet(7u, 12u), packet(3u, 48u) };
        GpuSceneIndirectPlan plan;
        buildGpuSceneIndirectPlan(packets, supported(), plan);
        CHECK(plan.abiVersion == GpuSceneIndirectAbiVersion);
        CHECK(plan.usesIndirect());
        CHECK(plan.commands.size() == 2u);
        CHECK(plan.commands[0].firstInstance == 7u);
        CHECK(plan.commands[0].firstIndex == 12u);
        CHECK(plan.commands[1].firstInstance == 3u);
        CHECK(plan.packetIndices == std::vector<uint32_t>({ 0u, 1u }));
        auto direct = supported();
        direct.forceDirectReference = true;
        buildGpuSceneIndirectPlan(packets, direct, plan);
        CHECK(!plan.usesIndirect());
        CHECK(plan.commands.empty());
        CHECK(plan.packetIndices.empty());
        CHECK(plan.fallbackReason == GpuSceneIndirectFallbackReason::DirectReference);
    }
    {
        std::vector packets{ packet(0u) };
        GpuSceneIndirectPlan plan;
        buildGpuSceneIndirectPlan(packets, supported(8u), plan);
        CHECK(!plan.usesIndirect());
        CHECK(plan.fallbackReason ==
            GpuSceneIndirectFallbackReason::TinyWorkload);
    }
    {
        std::vector packets{ packet(0u), packet(1u) };
        packets[1].executionFlags = 0u;
        GpuSceneIndirectPlan plan;
        buildGpuSceneIndirectPlan(packets, supported(), plan);
        CHECK(plan.commands.empty());
        CHECK(plan.fallbackReason ==
            GpuSceneIndirectFallbackReason::InvalidPacket);
    }
    {
        std::vector packets{ packet(0u), packet(1u) };
        GpuSceneIndirectPlan plan;
        auto policy = supported();
        policy.drawIndirectCount = false;
        buildGpuSceneIndirectPlan(packets, policy, plan);
        CHECK(plan.fallbackReason ==
            GpuSceneIndirectFallbackReason::MissingCapability);
        policy = supported();
        policy.maxDrawIndirectCount = 1u;
        buildGpuSceneIndirectPlan(packets, policy, plan);
        CHECK(plan.fallbackReason ==
            GpuSceneIndirectFallbackReason::CapacityExceeded);
    }
    {
        GpuSceneIndirectPlan plan;
        buildGpuSceneIndirectPlan({}, supported(), plan);
        CHECK(!plan.usesIndirect());
        CHECK(plan.fallbackReason == GpuSceneIndirectFallbackReason::None);
    }
    {
        // M7R R5c.4b: the record plan equals the packet plan of the parity
        // packets those records produced, including every fallback reason
        // and a direct packet in the queue.
        std::vector<GpuScenePrimitiveRecord> primitives(3);
        std::vector<GpuSceneGeometryRecord> geometries(3);
        for (uint32_t index = 0; index < 3u; ++index) {
            primitives[index].binding = { index, 2u - index, 5u, 6u };
            geometries[index].draw = { 3u * index, 6u + index,
                std::bit_cast<uint32_t>(static_cast<int32_t>(index) - 1), 1u };
        }
        const std::vector<uint32_t> order{ 2u, 0u, 1u };
        std::vector<DrawPacket> packets;
        for (const uint32_t primitive : order) {
            DrawPacket value = packet(primitive,
                geometries[primitives[primitive].binding.y].draw.x);
            value.indexCount = geometries[primitives[primitive].binding.y].draw.y;
            packets.push_back(value);
        }
        const auto same = [&](const GpuSceneIndirectPolicy& policy,
                size_t directs) {
            std::vector<DrawPacket> queue = packets;
            for (size_t index = 0; index < directs; ++index)
                queue.push_back(DrawPacket{ .indexCount = 3u });
            GpuSceneIndirectPlan fromPackets, fromRecords;
            buildGpuSceneIndirectPlan(queue, policy, fromPackets, primitives,
                geometries);
            buildGpuSceneIndirectPlan(order, directs, policy, fromRecords,
                primitives, geometries);
            return fromPackets.fallbackReason == fromRecords.fallbackReason &&
                fromPackets.packetIndices == fromRecords.packetIndices &&
                fromPackets.commands.size() == fromRecords.commands.size() &&
                std::equal(fromPackets.commands.begin(), fromPackets.commands.end(),
                    fromRecords.commands.begin(),
                    [](const auto& a, const auto& b) {
                        return std::bit_cast<std::array<uint32_t, 5>>(a) ==
                            std::bit_cast<std::array<uint32_t, 5>>(b);
                    });
        };
        CHECK(same(supported(), 0u));
        CHECK(same(supported(), 1u));
        CHECK(same(supported(4u), 0u));
        CHECK(same(supported(4u), 1u));
        auto policy = supported();
        policy.forceDirectReference = true;
        CHECK(same(policy, 0u));
        policy = supported();
        policy.multiDrawIndirect = false;
        CHECK(same(policy, 0u));
        policy = supported();
        policy.maxDrawIndirectCount = 3u;
        CHECK(same(policy, 0u));
        CHECK(same(policy, 1u));
    }
    if (failures == 0) std::cout << "GpuSceneIndirectTests passed\n";
    return failures == 0 ? 0 : 1;
}
