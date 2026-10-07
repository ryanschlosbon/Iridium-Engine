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
        sortOpaqueDrawKeys(keys);
        permutePackets(packets, keys,
            [](OpaqueDrawSortKey& key) -> uint32_t& { return key.packet; });
    }

    void sortOpaqueDrawKeys(std::span<OpaqueDrawSortKey> keys) {
        std::sort(keys.begin(), keys.end(),
            [](const OpaqueDrawSortKey& a, const OpaqueDrawSortKey& b) {
                if (a.opaqueSortKey != b.opaqueSortKey)
                    return a.opaqueSortKey < b.opaqueSortKey;
                if (a.geometry != b.geometry) return a.geometry < b.geometry;
                return a.firstIndex < b.firstIndex;
            });
    }

    void sortTransparentWorkDrawPackets(std::span<DrawPacket> packets,
        CompactDrawSortScratch& scratch) {
        checkPacketCount(packets.size());
        auto& keys = scratch.transparentWorkKeys;
        keys.resize(packets.size());
        for (size_t index = 0; index < packets.size(); ++index)
            keys[index] = makeTransparentWorkSortKey(packets[index],
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
        for (size_t index = 0; index < packets.size(); ++index)
            keys[index] = makeTransparentCompatibilitySortKey(packets[index],
                static_cast<uint32_t>(index));
        const std::span<const DrawPacket> unsorted = packets;
        std::sort(keys.begin(), keys.end(),
            [unsorted](const TransparentCompatibilitySortKey& lhs,
                const TransparentCompatibilitySortKey& rhs) {
                return transparentCompatibilityKeyLess(lhs, rhs, unsorted);
            });
        permutePackets(packets, keys,
            [](TransparentCompatibilitySortKey& key) -> uint32_t& {
                return key.work.packet;
            });
    }

} // namespace Iridium
