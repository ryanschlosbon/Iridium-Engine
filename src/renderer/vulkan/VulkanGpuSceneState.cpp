#include "VulkanGpuSceneState.h"

#include "renderer/rhi/MaterialTableCapacity.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Iridium {

    void VulkanGpuSceneState::init(VkDevice device,
        VulkanResourceAllocator& allocator, VulkanFrameScheduler& scheduler,
        CpuProfiler* profiler, const bool& frameOpen,
        uint64_t maxStorageBufferRange) {
        device_ = device;
        allocator_ = &allocator;
        scheduler_ = &scheduler;
        profiler_ = profiler;
        frameOpen_ = &frameOpen;
        const uint64_t storageRange = maxStorageBufferRange;
        maximumCapacity_ = {
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneAffineTransform)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneInstanceRecord)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuScenePrimitiveRecord)),
            static_cast<uint32_t>(storageRange /
                sizeof(GpuSceneGeometryRecord)),
        };
        if (maximumCapacity_.transforms == 0 ||
            maximumCapacity_.instances == 0 ||
            maximumCapacity_.primitives == 0 ||
            maximumCapacity_.geometries == 0) {
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold GPU-scene records");
        }
    }

    void VulkanGpuSceneState::createBuffers(
        const GpuSceneCapacityRequirements& capacity) {
        if ((*frameOpen_)) {
            throw std::logic_error(
                "GPU-scene buffers may grow only at a frame boundary");
        }
        if (capacity.transforms == 0 || capacity.instances == 0 ||
            capacity.primitives == 0 || capacity.geometries == 0 ||
            capacity.transforms > maximumCapacity_.transforms ||
            capacity.instances > maximumCapacity_.instances ||
            capacity.primitives > maximumCapacity_.primitives ||
            capacity.geometries > maximumCapacity_.geometries) {
            throw std::invalid_argument(
                "GPU-scene capacity is outside the device storage limit");
        }
        using Buffers = std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight>;
        Buffers transforms{}, instances{}, primitives{}, geometries{};
        const auto create = [this](VulkanBufferResource& destination,
            uint64_t count, uint64_t stride) {
            destination = allocator_->createBuffer(count * stride,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::GpuScene);
            std::memset(destination.mapped, 0,
                static_cast<size_t>(count * stride));
        };
        try {
            for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
                create(transforms[frame], capacity.transforms,
                    sizeof(GpuSceneAffineTransform));
                create(instances[frame], capacity.instances,
                    sizeof(GpuSceneInstanceRecord));
                create(primitives[frame], capacity.primitives,
                    sizeof(GpuScenePrimitiveRecord));
                create(geometries[frame], capacity.geometries,
                    sizeof(GpuSceneGeometryRecord));
            }
        }
        catch (...) {
            for (Buffers* buffers : { &transforms, &instances,
                    &primitives, &geometries })
                for (VulkanBufferResource& buffer : *buffers)
                    allocator_->destroy(buffer);
            throw;
        }
        // R4c.2: no drain. A slot that is not in flight swaps now; an
        // in-flight slot parks its replacement (an older parked one was never
        // used) until its retirement. Its next publication follows the swap.
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            destroySlot({ &pendingTransformBuffers_, &pendingInstanceBuffers_,
                &pendingPrimitiveBuffers_, &pendingGeometryBuffers_ }, frame);
            pendingSlots_[frame] = scheduler_->slotInFlight(frame);
            if (pendingSlots_[frame]) {
                pendingTransformBuffers_[frame] = transforms[frame];
                pendingInstanceBuffers_[frame] = instances[frame];
                pendingPrimitiveBuffers_[frame] = primitives[frame];
                pendingGeometryBuffers_[frame] = geometries[frame];
                continue;
            }
            destroySlot({ &transformBuffers_, &instanceBuffers_,
                &primitiveBuffers_, &geometryBuffers_ }, frame);
            transformBuffers_[frame] = transforms[frame];
            instanceBuffers_[frame] = instances[frame];
            primitiveBuffers_[frame] = primitives[frame];
            geometryBuffers_[frame] = geometries[frame];
            if (descriptorSets_[frame] != VK_NULL_HANDLE) bindSlot(frame);
        }
        capacity_ = capacity;
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
            uploadedTransformRevisions_[frame].assign(
                capacity.transforms, 0);
            uploadedInstanceRevisions_[frame].assign(
                capacity.instances, 0);
            uploadedPrimitiveRevisions_[frame].assign(
                capacity.primitives, 0);
            uploadedGeometryRevisions_[frame].assign(
                capacity.geometries, 0);
            auto& mirror = cpuMirrors_[frame];
            mirror.transforms.resize(capacity.transforms);
            mirror.instances.resize(capacity.instances);
            mirror.primitives.resize(capacity.primitives);
            mirror.geometries.resize(capacity.geometries);
            mirror.primitiveIdentities.resize(capacity.primitives);
        }
        uploadRanges_.reserve((std::max)({ capacity.transforms,
            capacity.instances, capacity.primitives, capacity.geometries }));
    }

    void VulkanGpuSceneState::destroySlot(std::array<FrameBuffers*, 4> tables,
        uint32_t frame) noexcept {
        for (FrameBuffers* buffers : tables)
            allocator_->destroy((*buffers)[frame]);
    }

    bool VulkanGpuSceneState::swapRetiredSlot(uint32_t slot) {
        if (!pendingSlots_[slot]) return false;
        destroySlot({ &transformBuffers_, &instanceBuffers_,
            &primitiveBuffers_, &geometryBuffers_ }, slot);
        transformBuffers_[slot] = pendingTransformBuffers_[slot];
        instanceBuffers_[slot] = pendingInstanceBuffers_[slot];
        primitiveBuffers_[slot] = pendingPrimitiveBuffers_[slot];
        geometryBuffers_[slot] = pendingGeometryBuffers_[slot];
        pendingTransformBuffers_[slot] = {};
        pendingInstanceBuffers_[slot] = {};
        pendingPrimitiveBuffers_[slot] = {};
        pendingGeometryBuffers_[slot] = {};
        pendingSlots_[slot] = false;
        if (descriptorSets_[slot] != VK_NULL_HANDLE) bindSlot(slot);
        return true;
    }

    void VulkanGpuSceneState::bindBuffers() {
        for (uint32_t frame = 0;
                frame < VulkanFrameScheduler::FramesInFlight; ++frame)
            bindSlot(frame);
    }

    void VulkanGpuSceneState::bindSlot(uint32_t frame) {
        {
            const std::array<VkDescriptorBufferInfo, 4> infos{{
                { transformBuffers_[frame].buffer, 0,
                    transformBuffers_[frame].size },
                { instanceBuffers_[frame].buffer, 0,
                    instanceBuffers_[frame].size },
                { primitiveBuffers_[frame].buffer, 0,
                    primitiveBuffers_[frame].size },
                { geometryBuffers_[frame].buffer, 0,
                    geometryBuffers_[frame].size },
            }};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding] = {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[binding].dstSet = descriptorSets_[frame];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorType =
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[binding].descriptorCount = 1u;
                writes[binding].pBufferInfo = &infos[binding];
            }
            vkUpdateDescriptorSets(device_,
                static_cast<uint32_t>(writes.size()), writes.data(),
                0u, nullptr);
        }
    }

    void VulkanGpuSceneState::prepare(
        const GpuSceneCapacityRequirements& requirements) {
        const auto grow = [](uint32_t current, uint32_t required,
            uint32_t maximum) {
            required = (std::max)(required, 1u);
            return required <= current ? current : nextMaterialTableCapacity(
                current, required, maximum);
        };
        GpuSceneCapacityRequirements next{
            grow(capacity_.transforms, requirements.transforms,
                maximumCapacity_.transforms),
            grow(capacity_.instances, requirements.instances,
                maximumCapacity_.instances),
            grow(capacity_.primitives, requirements.primitives,
                maximumCapacity_.primitives),
            grow(capacity_.geometries, requirements.geometries,
                maximumCapacity_.geometries),
        };
        if (next.transforms != capacity_.transforms ||
            next.instances != capacity_.instances ||
            next.primitives != capacity_.primitives ||
            next.geometries != capacity_.geometries)
            createBuffers(next);
    }

    void VulkanGpuSceneState::validatePublication(
        const GpuScenePackedTables& scene) const {
        if (scene.abiVersion != GpuSceneAbiVersion ||
            scene.transforms.size() > capacity_.transforms ||
            scene.instances.size() > capacity_.instances ||
            scene.primitives.size() > capacity_.primitives ||
            scene.geometries.size() > capacity_.geometries ||
            scene.transformRevisions.size() != scene.transforms.size() ||
            scene.instanceRevisions.size() != scene.instances.size() ||
            scene.primitiveRevisions.size() != scene.primitives.size() ||
            scene.geometryRevisions.size() != scene.geometries.size() ||
            scene.primitiveIdentities.size() != scene.primitives.size()) {
            throw std::out_of_range(
                "GPU-scene publication is outside prepared capacity or ABI");
        }
    }

    void VulkanGpuSceneState::publish(const GpuScenePackedTables& scene,
        uint32_t frame) {
        publishedEpoch_ = scene.sceneEpoch == 0u
            ? 1u : scene.sceneEpoch;
        publishedCounts_ = {
            static_cast<uint32_t>(scene.transforms.size()),
            static_cast<uint32_t>(scene.instances.size()),
            static_cast<uint32_t>(scene.primitives.size()),
            static_cast<uint32_t>(scene.geometries.size()),
        };
        uploadTelemetry_ = {};
        const auto upload = [this](auto records,
            std::span<const uint64_t> revisions,
            std::vector<uint64_t>& uploaded,
            VulkanBufferResource& destination, auto& cpuMirror, uint32_t& tableRanges) {
            buildGpuSceneUploadRanges(revisions, uploaded,
                uploadRanges_);
            tableRanges = static_cast<uint32_t>(uploadRanges_.size());
            uploadTelemetry_.ranges += tableRanges;
            using Record = typename decltype(records)::value_type;
            for (const GpuSceneRecordRange range : uploadRanges_) {
                const auto source = records.subspan(
                    range.firstRecord, range.recordCount);
                allocator_->write(destination,
                    static_cast<VkDeviceSize>(range.firstRecord) *
                        sizeof(Record), std::as_bytes(source));
                std::copy(source.begin(), source.end(), cpuMirror.begin() + range.firstRecord);
                std::copy_n(revisions.begin() + range.firstRecord,
                    range.recordCount,
                    uploaded.begin() + range.firstRecord);
                uploadTelemetry_.bytes +=
                    static_cast<uint64_t>(range.recordCount) * sizeof(Record);
            }
        };
        upload(std::span(scene.transforms), scene.transformRevisions,
            uploadedTransformRevisions_[frame],
            transformBuffers_[frame],
            cpuMirrors_[frame].transforms,
            uploadTelemetry_.transformRanges);
        upload(std::span(scene.instances), scene.instanceRevisions,
            uploadedInstanceRevisions_[frame],
            instanceBuffers_[frame],
            cpuMirrors_[frame].instances,
            uploadTelemetry_.instanceRanges);
        upload(std::span(scene.primitives), scene.primitiveRevisions,
            uploadedPrimitiveRevisions_[frame],
            primitiveBuffers_[frame],
            cpuMirrors_[frame].primitives,
            uploadTelemetry_.primitiveRanges);
        for (const GpuSceneRecordRange range : uploadRanges_)
            std::copy_n(scene.primitiveIdentities.begin() +
                    range.firstRecord,
                range.recordCount,
                cpuMirrors_[frame].primitiveIdentities.begin() +
                    range.firstRecord);
        upload(std::span(scene.geometries), scene.geometryRevisions,
            uploadedGeometryRevisions_[frame],
            geometryBuffers_[frame],
            cpuMirrors_[frame].geometries,
            uploadTelemetry_.geometryRanges);
        if (profiler_ != nullptr) {
            uint64_t mirrorBytes = 0;
            for (const auto& mirror : cpuMirrors_)
                mirrorBytes += mirror.transforms.capacity() * sizeof(GpuSceneAffineTransform) +
                    mirror.instances.capacity() * sizeof(GpuSceneInstanceRecord) +
                    mirror.primitives.capacity() * sizeof(GpuScenePrimitiveRecord) +
                    mirror.geometries.capacity() * sizeof(GpuSceneGeometryRecord) +
                    mirror.primitiveIdentities.capacity() *
                        sizeof(GpuScenePrimitiveIdentity);
            profiler_->recordCounter("gpu_scene.cpu_mirror.capacity_bytes", mirrorBytes,
                ProfileCounterStatus::Exact, ProfileCounterUnit::Bytes);
        }
    }

    VulkanIndirectScene VulkanGpuSceneState::indirectScene(
        uint32_t frame) const noexcept {
        const CpuMirror& scene = cpuMirrors_[frame];
        return {
            .transforms = scene.transforms,
            .instances = scene.instances,
            .primitives = scene.primitives,
            .geometries = scene.geometries,
            .identities = scene.primitiveIdentities,
            .published = publishedCounts_,
        };
    }

    void VulkanGpuSceneState::destroy() noexcept {
        for (FrameBuffers* buffers : { &transformBuffers_, &instanceBuffers_,
                &primitiveBuffers_, &geometryBuffers_,
                &pendingTransformBuffers_, &pendingInstanceBuffers_,
                &pendingPrimitiveBuffers_, &pendingGeometryBuffers_ })
            for (VulkanBufferResource& buffer : *buffers)
                allocator_->destroy(buffer);
        pendingSlots_ = {};
    }

    void VulkanGpuSceneState::reset() noexcept {
        capacity_ = {};
        maximumCapacity_ = {};
        publishedCounts_ = {};
        cpuMirrors_ = {};
        uploadTelemetry_ = {};
    }

} // namespace Iridium
