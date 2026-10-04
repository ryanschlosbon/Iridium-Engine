#include "qualification/vulkan/VulkanIndirectStreamDigest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>

namespace Iridium {

    namespace {
        // Event tags in the dispatch log. Values are part of the digest.
        enum class Event : uint32_t {
            GpuRange = 1,
            Barrier = 2,
            BindPipeline = 3,
            BindSets = 4,
            Push = 5,
            Dispatch = 6,
            Note = 7,
            Draw = 8,
            HostWrite = 9,
        };

        // The stream's own buffers are recorded by role; anything else by
        // stable index offset past the roles.
        constexpr uint32_t CommandBufferRole = 0xffff0001u;
        constexpr uint32_t CountBufferRole = 0xffff0002u;

        std::string hex(uint64_t value) {
            char text[17]{};
            std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(value));
            return text;
        }
    }

    const char* VulkanIndirectStreamDigest::viewName(View view) noexcept {
        switch (view) {
        case View::DirectionalShadow: return "directional";
        case View::SpotShadow: return "spot";
        case View::PointShadow: return "point";
        case View::ReflectionProbe: return "probe";
        case View::Opaque: return "opaque";
        }
        return "unknown";
    }

    void VulkanIndirectStreamDigest::Hash::bytes(const void* data,
        size_t size) noexcept {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            value ^= bytes[index];
            value *= 1099511628211ull;
        }
    }

    VulkanIndirectStreamDigest::OpenStream* VulkanIndirectStreamDigest::open(
        View view, uint32_t slot) noexcept {
        const auto viewIndex = static_cast<size_t>(view);
        if (viewIndex >= open_.size() || slot >= FramesInFlight) return nullptr;
        OpenStream& stream = open_[viewIndex][slot];
        if (!stream.active) {
            ++totals_[viewIndex].orphanEvents;
            return nullptr;
        }
        return &stream;
    }

    uint32_t VulkanIndirectStreamDigest::stableIndex(Handle kind,
        uint64_t handle) {
        if (handle == 0u) return 0xffffffffu;
        auto& map = handles_[static_cast<size_t>(kind)];
        const auto [entry, inserted] = map.try_emplace(handle,
            static_cast<uint32_t>(map.size()));
        (void)inserted;
        return entry->second;
    }

    void VulkanIndirectStreamDigest::beginStream(View view, uint32_t slot,
        VkBuffer commandBuffer, VkBuffer countBuffer) {
        const auto viewIndex = static_cast<size_t>(view);
        if (viewIndex >= open_.size() || slot >= FramesInFlight) return;
        OpenStream& stream = open_[viewIndex][slot];
        if (stream.active) ++totals_[viewIndex].orphanEvents; // never retired
        stream = OpenStream{};
        stream.active = true;
        stream.sequence = nextSequence_[viewIndex]++;
        stream.commandBuffer = commandBuffer;
        stream.countBuffer = countBuffer;
    }

    void VulkanIndirectStreamDigest::hostWrite(View view, uint32_t slot,
        VulkanIndirectStreamHostTarget target, std::span<const std::byte> bytes) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->host.u32(static_cast<uint32_t>(Event::HostWrite));
        stream->host.u32(static_cast<uint32_t>(target));
        stream->host.u64(bytes.size());
        stream->host.bytes(bytes.data(), bytes.size());
    }

    void VulkanIndirectStreamDigest::gpuRange(View view, uint32_t slot,
        const char* name) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        const std::string_view text = name != nullptr ? name : "";
        stream->log.u32(static_cast<uint32_t>(Event::GpuRange));
        stream->log.u64(text.size());
        stream->log.bytes(text.data(), text.size());
    }

    void VulkanIndirectStreamDigest::barrier(View view, uint32_t slot,
        VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
        VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::Barrier));
        stream->log.u32(srcStages);
        stream->log.u32(dstStages);
        stream->log.u32(srcAccess);
        stream->log.u32(dstAccess);
    }

    void VulkanIndirectStreamDigest::bindPipeline(View view, uint32_t slot,
        VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::BindPipeline));
        stream->log.u32(static_cast<uint32_t>(bindPoint));
        stream->log.u32(stable(Handle::Pipeline, pipeline));
    }

    void VulkanIndirectStreamDigest::bindDescriptorSets(View view, uint32_t slot,
        VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
        uint32_t firstSet, std::span<const VkDescriptorSet> sets,
        std::span<const uint32_t> dynamicOffsets) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::BindSets));
        stream->log.u32(static_cast<uint32_t>(bindPoint));
        stream->log.u32(stable(Handle::Layout, layout));
        stream->log.u32(firstSet);
        stream->log.u32(static_cast<uint32_t>(sets.size()));
        for (const VkDescriptorSet set : sets)
            stream->log.u32(stable(Handle::Set, set));
        stream->log.u32(static_cast<uint32_t>(dynamicOffsets.size()));
        for (const uint32_t offset : dynamicOffsets) stream->log.u32(offset);
    }

    void VulkanIndirectStreamDigest::pushConstants(View view, uint32_t slot,
        VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
        std::span<const std::byte> bytes) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::Push));
        stream->log.u32(stable(Handle::Layout, layout));
        stream->log.u32(stages);
        stream->log.u32(offset);
        stream->log.u64(bytes.size());
        stream->log.bytes(bytes.data(), bytes.size());
    }

    void VulkanIndirectStreamDigest::dispatch(View view, uint32_t slot,
        uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::Dispatch));
        stream->log.u32(groupsX);
        stream->log.u32(groupsY);
        stream->log.u32(groupsZ);
        ++stream->dispatches;
    }

    void VulkanIndirectStreamDigest::note(View view, uint32_t slot,
        uint32_t tag, std::span<const uint64_t> values) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        stream->log.u32(static_cast<uint32_t>(Event::Note));
        stream->log.u32(tag);
        stream->log.u64(values.size());
        for (const uint64_t value : values) stream->log.u64(value);
    }

    void VulkanIndirectStreamDigest::indirectDraw(View view, uint32_t slot,
        VkPipeline pipeline, VkBuffer vertexBuffer, VkBuffer indexBuffer,
        VkIndexType indexType, uint32_t pushWord, VkBuffer commandBuffer,
        VkDeviceSize commandOffset, VkBuffer countBuffer,
        VkDeviceSize countOffset, uint32_t maxDrawCount) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        const auto role = [&](VkBuffer buffer) {
            if (buffer == stream->commandBuffer) return CommandBufferRole;
            if (buffer == stream->countBuffer) return CountBufferRole;
            return stable(Handle::Buffer, buffer);
        };
        stream->log.u32(static_cast<uint32_t>(Event::Draw));
        stream->log.u32(stable(Handle::Pipeline, pipeline));
        stream->log.u32(stable(Handle::Buffer, vertexBuffer));
        stream->log.u32(stable(Handle::Buffer, indexBuffer));
        stream->log.u32(static_cast<uint32_t>(indexType));
        stream->log.u32(pushWord);
        stream->log.u32(role(commandBuffer));
        stream->log.u64(commandOffset);
        stream->log.u32(role(countBuffer));
        stream->log.u64(countOffset);
        stream->log.u32(maxDrawCount);
        ++stream->draws;
    }

    void VulkanIndirectStreamDigest::retireStream(View view, uint32_t slot,
        const VulkanIndirectStreamReadback& readback) {
        OpenStream* stream = open(view, slot);
        if (stream == nullptr) return;
        // A command's primitive is identified by its instance's world
        // transform: GPU-scene slot assignment follows entity identities,
        // which are not stable across processes.
        const auto primitiveKey = [&](uint32_t primitiveIndex) {
            Hash key;
            if (primitiveIndex >= readback.primitives.size()) {
                key.u32(0xffffffffu);
                return key.value;
            }
            const uint32_t instanceIndex =
                readback.primitives[primitiveIndex].binding.x;
            if (instanceIndex >= readback.instances.size()) {
                key.u32(0xfffffffeu);
                return key.value;
            }
            const uint32_t transformIndex =
                readback.instances[instanceIndex].references.x;
            if (transformIndex >= readback.transforms.size()) {
                key.u32(0xfffffffdu);
                return key.value;
            }
            key.bytes(&readback.transforms[transformIndex],
                sizeof(GpuSceneAffineTransform));
            return key.value;
        };
        Hash device;
        Hash deviceSlots;
        uint64_t packedOffset = 0;
        for (size_t region = 0; region < readback.countCapacities.size();
                ++region) {
            const uint32_t capacity = readback.countCapacities[region];
            const uint32_t count = readback.counts != nullptr
                ? readback.counts[region] : 0u;
            const uint64_t offset = readback.commandOffsets.empty()
                ? packedOffset
                : (region < readback.commandOffsets.size()
                    ? readback.commandOffsets[region] : 0u);
            packedOffset += capacity;
            const uint32_t submitted = (std::min)(count, capacity);
            regionScratch_.clear();
            if (readback.commands != nullptr) {
                for (uint32_t index = 0; index < submitted; ++index) {
                    const GpuSceneIndexedIndirectCommand& command =
                        readback.commands[offset + index];
                    regionScratch_.push_back(
                        { primitiveKey(command.firstInstance), command });
                }
            }
            const auto fields = [](const KeyedCommand& value) {
                return std::tie(value.command.indexCount,
                    value.command.instanceCount, value.command.firstIndex,
                    value.command.vertexOffset);
            };
            const auto hashRegion = [&](Hash& hash, bool slots) {
                hash.u64(region);
                hash.u32(count);
                hash.u32(capacity);
                hash.u64(offset);
                for (const KeyedCommand& value : regionScratch_) {
                    hash.u64(slots ? value.command.firstInstance : value.key);
                    hash.u32(value.command.indexCount);
                    hash.u32(value.command.instanceCount);
                    hash.u32(value.command.firstIndex);
                    hash.u32(static_cast<uint32_t>(value.command.vertexOffset));
                }
            };
            std::sort(regionScratch_.begin(), regionScratch_.end(),
                [&](const KeyedCommand& left, const KeyedCommand& right) {
                    return std::tuple_cat(std::tie(left.key), fields(left)) <
                        std::tuple_cat(std::tie(right.key), fields(right));
                });
            hashRegion(device, false);
            std::sort(regionScratch_.begin(), regionScratch_.end(),
                [&](const KeyedCommand& left, const KeyedCommand& right) {
                    return std::tuple_cat(std::tie(left.command.firstInstance),
                            fields(left)) <
                        std::tuple_cat(std::tie(right.command.firstInstance),
                            fields(right));
                });
            hashRegion(deviceSlots, true);
        }

        StreamRecord record{
            .view = view,
            .slot = slot,
            .sequence = stream->sequence,
            .dispatches = stream->dispatches,
            .draws = stream->draws,
            .logHash = stream->log.value,
            .hostHash = stream->host.value,
            .deviceHash = device.value,
            .deviceSlotHash = deviceSlots.value,
        };
        Hash digest;
        digest.u64(record.logHash);
        digest.u64(record.hostHash);
        digest.u64(record.deviceHash);
        record.digest = digest.value;
        ViewTotals& totals = totals_[static_cast<size_t>(view)];
        totals.digest.u64(record.sequence);
        totals.digest.u64(record.digest);
        ++totals.streams;
        stream->active = false;
        retired_.push_back(record);
        if (output_ != nullptr) {
            *output_ << "IRIDIUM_INDIRECT_STREAM_DIGEST {\"view\":\""
                << viewName(view) << "\",\"slot\":" << slot
                << ",\"sequence\":" << record.sequence
                << ",\"dispatches\":" << record.dispatches
                << ",\"draws\":" << record.draws
                << ",\"log\":\"" << hex(record.logHash)
                << "\",\"host\":\"" << hex(record.hostHash)
                << "\",\"device\":\"" << hex(record.deviceHash)
                << "\",\"device_slots\":\"" << hex(record.deviceSlotHash)
                << "\",\"digest\":\"" << hex(record.digest) << "\"}\n";
        }
    }

    void VulkanIndirectStreamDigest::finish() {
        if (finished_) return;
        finished_ = true;
        Hash all;
        uint64_t streams = 0;
        uint64_t orphans = 0;
        uint64_t unretired = 0;
        for (size_t view = 0; view < totals_.size(); ++view) {
            uint64_t viewUnretired = 0;
            for (const OpenStream& stream : open_[view])
                viewUnretired += stream.active ? 1u : 0u;
            const ViewTotals& totals = totals_[view];
            all.u64(totals.streams);
            all.u64(totals.digest.value);
            streams += totals.streams;
            orphans += totals.orphanEvents;
            unretired += viewUnretired;
            if (output_ != nullptr) {
                *output_ << "IRIDIUM_INDIRECT_STREAM_DIGEST {\"aggregate\":true,"
                    << "\"view\":\"" << viewName(static_cast<View>(view))
                    << "\",\"streams\":" << totals.streams
                    << ",\"orphan_events\":" << totals.orphanEvents
                    << ",\"unretired\":" << viewUnretired
                    << ",\"digest\":\"" << hex(totals.digest.value) << "\"}\n";
            }
        }
        if (output_ != nullptr) {
            *output_ << "IRIDIUM_INDIRECT_STREAM_DIGEST {\"aggregate\":true,"
                << "\"view\":\"all\",\"streams\":" << streams
                << ",\"orphan_events\":" << orphans
                << ",\"unretired\":" << unretired
                << ",\"digest\":\"" << hex(all.value) << "\"}\n";
            output_->flush();
        }
    }

} // namespace Iridium
