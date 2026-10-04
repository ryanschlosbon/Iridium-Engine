#include "renderer/rhi/CompactDrawSort.h"

#include <algorithm>
#include <stdexcept>

namespace Iridium {
namespace {

    constexpr uint32_t PlacedPacket = UINT32_MAX;

    void checkPacketCount(size_t count) {
        if (count >= PlacedPacket)
            throw std::length_error("draw packet queue exceeds 32-bit indices");
    }

    // packets[j] = original packets[index(keys[j])], in place, one cycle of the
    // permutation at a time: each packet moves once and no second packet
    // buffer is needed. Consumes the keys' indices.
    template<typename Key, typename IndexOf>
    void permutePackets(std::span<DrawPacket> packets, std::vector<Key>& keys,
        IndexOf indexOf) {
        for (size_t start = 0; start < keys.size(); ++start) {
            uint32_t& startSource = indexOf(keys[start]);
            if (startSource == PlacedPacket) continue;
            if (startSource == start) {
                startSource = PlacedPacket;
                continue;
            }
            const DrawPacket held = packets[start];
            size_t target = start;
            for (;;) {
                uint32_t& source = indexOf(keys[target]);
                const uint32_t from = source;
                source = PlacedPacket;
                if (from == start) {
                    packets[target] = held;
                    break;
                }
                packets[target] = packets[from];
                target = from;
            }
        }
    }

    TransparentWorkSortKey transparentWorkKey(const DrawPacket& packet,
        uint32_t index) noexcept {
        return {
            .priority = packet.transparency.priority,
            .workFlags = packet.transparentWorkFlags,
            .farDepth = packet.transparentFarDepth,
            .nearDepth = packet.transparentNearDepth,
            .packet = index,
        };
    }

    // transparentWorkLess over keys; identity from the unsorted packets.
    bool transparentWorkKeyLess(const TransparentWorkSortKey& lhs,
        const TransparentWorkSortKey& rhs,
        std::span<const DrawPacket> packets) noexcept {
        if (lhs.priority != rhs.priority)
            return lhs.priority < rhs.priority;
        const bool lhsValid = (lhs.workFlags &
            TransparentWorkIntervalValid) != 0;
        const bool rhsValid = (rhs.workFlags &
            TransparentWorkIntervalValid) != 0;
        if (lhsValid != rhsValid) return lhsValid;
        if (lhsValid) {
            const bool lhsIntersects = (lhs.workFlags &
                TransparentWorkCameraIntersecting) != 0;
            const bool rhsIntersects = (rhs.workFlags &
                TransparentWorkCameraIntersecting) != 0;
            if (lhsIntersects != rhsIntersects) return !lhsIntersects;
            if (lhs.farDepth != rhs.farDepth)
                return lhs.farDepth > rhs.farDepth;
            if (lhs.nearDepth != rhs.nearDepth)
                return lhs.nearDepth > rhs.nearDepth;
        }
        return transparentWorkIdentity(packets[lhs.packet]) <
            transparentWorkIdentity(packets[rhs.packet]);
    }

} // namespace

    void sortOpaqueDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch) {
        checkPacketCount(packets.size());
        auto& keys = scratch.opaqueKeys;
        keys.resize(packets.size());
        for (size_t index = 0; index < packets.size(); ++index) {
            const DrawPacket& packet = packets[index];
            keys[index] = {
                .opaqueSortKey = packet.opaqueSortKey,
                .geometry = packet.geometry,
                .firstIndex = packet.firstIndex,
                .packet = static_cast<uint32_t>(index),
            };
        }
        std::sort(keys.begin(), keys.end(),
            [](const OpaqueDrawSortKey& a, const OpaqueDrawSortKey& b) {
                if (a.opaqueSortKey != b.opaqueSortKey)
                    return a.opaqueSortKey < b.opaqueSortKey;
                if (a.geometry != b.geometry) return a.geometry < b.geometry;
                return a.firstIndex < b.firstIndex;
            });
        permutePackets(packets, keys,
            [](OpaqueDrawSortKey& key) -> uint32_t& { return key.packet; });
    }

    void sortTransparentWorkDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch) {
        checkPacketCount(packets.size());
        auto& keys = scratch.transparentWorkKeys;
        keys.resize(packets.size());
        for (size_t index = 0; index < packets.size(); ++index)
            keys[index] = transparentWorkKey(packets[index],
                static_cast<uint32_t>(index));
        const std::span<const DrawPacket> unsorted = packets;
        std::sort(keys.begin(), keys.end(),
            [unsorted](const TransparentWorkSortKey& a,
                const TransparentWorkSortKey& b) {
                return transparentWorkKeyLess(a, b, unsorted);
            });
        permutePackets(packets, keys,
            [](TransparentWorkSortKey& key) -> uint32_t& { return key.packet; });
    }

    void sortTransparentCompatibilityDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch) {
        checkPacketCount(packets.size());
        auto& keys = scratch.transparentCompatibilityKeys;
        keys.resize(packets.size());
        for (size_t index = 0; index < packets.size(); ++index) {
            const DrawPacket& packet = packets[index];
            keys[index] = {
                .work = transparentWorkKey(packet, static_cast<uint32_t>(index)),
                .distanceToCamera = packet.distanceToCamera,
                .pipeline = packet.pipeline,
                .material = packet.material,
                .geometry = packet.geometry,
                .executionMode = packet.transparencyExecutionMode,
            };
        }
        const std::span<const DrawPacket> unsorted = packets;
        std::sort(keys.begin(), keys.end(),
            [unsorted](const TransparentCompatibilitySortKey& lhs,
                const TransparentCompatibilitySortKey& rhs) {
                if (lhs.executionMode != rhs.executionMode)
                    return lhs.executionMode < rhs.executionMode;
                if (lhs.executionMode == TransparencyExecutionMode::Classified)
                    return transparentWorkKeyLess(lhs.work, rhs.work, unsorted);
                if (lhs.distanceToCamera != rhs.distanceToCamera)
                    return lhs.distanceToCamera > rhs.distanceToCamera;
                if (lhs.pipeline != rhs.pipeline) return lhs.pipeline < rhs.pipeline;
                if (lhs.material != rhs.material) return lhs.material < rhs.material;
                return lhs.geometry < rhs.geometry;
            });
        permutePackets(packets, keys,
            [](TransparentCompatibilitySortKey& key) -> uint32_t& {
                return key.work.packet;
            });
    }

} // namespace Iridium
