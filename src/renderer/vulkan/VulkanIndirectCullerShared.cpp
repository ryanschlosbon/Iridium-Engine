#include "VulkanIndirectCullerShared.h"

#include "DescriptorAllocator.h"
#include "renderer/rhi/Mesh.h"
#include "utils/File.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <vector>

namespace Iridium {

    VkIndexType toVkIndexType(IndexFormat format) {
        switch (format) {
        case IndexFormat::UInt16:
            return VK_INDEX_TYPE_UINT16;
        case IndexFormat::UInt32:
            return VK_INDEX_TYPE_UINT32;
        }
        throw std::invalid_argument("Unsupported geometry index format.");
    }

    namespace {
        void cmdBindPipeline(void*, VkCommandBuffer cmd,
            VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
            vkCmdBindPipeline(cmd, bindPoint, pipeline);
        }
        void cmdBindDescriptorSets(void*, VkCommandBuffer cmd,
            VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
            uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets,
            uint32_t dynamicOffsetCount, const uint32_t* dynamicOffsets) {
            vkCmdBindDescriptorSets(cmd, bindPoint, layout, firstSet, setCount,
                sets, dynamicOffsetCount, dynamicOffsets);
        }
        void cmdPushConstants(void*, VkCommandBuffer cmd, VkPipelineLayout layout,
            VkShaderStageFlags stages, uint32_t offset, uint32_t size,
            const void* values) {
            vkCmdPushConstants(cmd, layout, stages, offset, size, values);
        }
        void cmdDispatch(void*, VkCommandBuffer cmd, uint32_t x, uint32_t y,
            uint32_t z) {
            vkCmdDispatch(cmd, x, y, z);
        }
        void cmdMemoryBarrier(void*, VkCommandBuffer cmd,
            VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
            VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0u,
                1u, &barrier, 0u, nullptr, 0u, nullptr);
        }
        void cmdBindVertexBuffer(void*, VkCommandBuffer cmd, VkBuffer buffer,
            VkDeviceSize offset) {
            vkCmdBindVertexBuffers(cmd, 0u, 1u, &buffer, &offset);
        }
        void cmdBindIndexBuffer(void*, VkCommandBuffer cmd, VkBuffer buffer,
            VkDeviceSize offset, VkIndexType indexType) {
            vkCmdBindIndexBuffer(cmd, buffer, offset, indexType);
        }
        void cmdDrawIndexedIndirectCount(void*, VkCommandBuffer cmd,
            VkBuffer commandBuffer, VkDeviceSize commandOffset,
            VkBuffer countBuffer, VkDeviceSize countOffset,
            uint32_t maxDrawCount, uint32_t stride) {
            vkCmdDrawIndexedIndirectCount(cmd, commandBuffer, commandOffset,
                countBuffer, countOffset, maxDrawCount, stride);
        }
        VulkanGpuRangeToken schedulerBeginRange(void* user, const char* name) {
            return user != nullptr
                ? static_cast<VulkanFrameScheduler*>(user)->beginGpuRange(name)
                : VulkanGpuRangeToken{};
        }
        void schedulerEndRange(void* user, VulkanGpuRangeToken& token) {
            if (user != nullptr)
                static_cast<VulkanFrameScheduler*>(user)->endGpuRange(token);
        }

        VulkanCullerDevice& device(void* user) {
            return *static_cast<VulkanCullerDevice*>(user);
        }
        VulkanBufferResource deviceCreateBuffer(void* user, VkDeviceSize bytes,
            VkBufferUsageFlags usage) {
            return device(user).allocator->createBuffer(bytes, usage,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                true, ProfileMemoryCategory::GpuScene);
        }
        void deviceDestroyBuffer(void* user, VulkanBufferResource& buffer) {
            device(user).allocator->destroy(buffer);
        }
        VkDescriptorSet deviceAllocateSet(void* user, VkDescriptorSetLayout layout) {
            return device(user).descriptors->allocate(layout);
        }
        void deviceFreeSet(void* user, VkDescriptorSet set) {
            device(user).descriptors->free(set);
        }
        void deviceWriteStorageBuffers(void* user, VkDescriptorSet set,
            uint32_t firstBinding, std::span<const VkDescriptorBufferInfo> buffers) {
            std::array<VkWriteDescriptorSet, 8> writes{};
            if (buffers.size() > writes.size())
                throw std::invalid_argument("too many storage-buffer descriptors");
            for (uint32_t index = 0; index < buffers.size(); ++index) {
                writes[index] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[index].dstSet = set;
                writes[index].dstBinding = firstBinding + index;
                writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[index].descriptorCount = 1u;
                writes[index].pBufferInfo = &buffers[index];
            }
            vkUpdateDescriptorSets(device(user).device,
                static_cast<uint32_t>(buffers.size()), writes.data(), 0u, nullptr);
        }
        void deviceWriteCombinedImageSampler(void* user, VkDescriptorSet set,
            uint32_t binding, VkSampler sampler, VkImageView view,
            VkImageLayout layout) {
            const VkDescriptorImageInfo info{ sampler, view, layout };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1u;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device(user).device, 1u, &write, 0u, nullptr);
        }
        bool deviceSlotInFlight(void* user, uint32_t slot) {
            return device(user).scheduler->slotInFlight(slot);
        }
        void deviceRetireBuffer(void* user, const VulkanBufferResource& buffer) {
            device(user).scheduler->retire(buffer);
        }
    }

    VulkanCullerCommands VulkanCullerCommands::vulkan(
        VulkanFrameScheduler* scheduler) noexcept {
        return {
            .user = scheduler,
            .bindPipeline = cmdBindPipeline,
            .bindDescriptorSets = cmdBindDescriptorSets,
            .pushConstants = cmdPushConstants,
            .dispatch = cmdDispatch,
            .memoryBarrier = cmdMemoryBarrier,
            .bindVertexBuffer = cmdBindVertexBuffer,
            .bindIndexBuffer = cmdBindIndexBuffer,
            .drawIndexedIndirectCount = cmdDrawIndexedIndirectCount,
            .beginGpuRange = schedulerBeginRange,
            .endGpuRange = schedulerEndRange,
        };
    }

    VulkanCullerResources VulkanCullerDevice::resources() noexcept {
        return {
            .user = this,
            .createBuffer = deviceCreateBuffer,
            .destroyBuffer = deviceDestroyBuffer,
            .allocateSet = deviceAllocateSet,
            .freeSet = deviceFreeSet,
            .writeStorageBuffers = deviceWriteStorageBuffers,
            .writeCombinedImageSampler = deviceWriteCombinedImageSampler,
            .slotInFlight = deviceSlotInFlight,
            .retireBuffer = deviceRetireBuffer,
        };
    }

    bool resolveIndirectCaster(const VulkanIndirectScene& scene,
        uint32_t primitiveIndex, uint32_t consumerMask,
        VulkanResolvedCaster& caster) noexcept {
        if (consumerMask == 0u ||
            primitiveIndex >= scene.published.primitives ||
            primitiveIndex >= scene.primitives.size() ||
            primitiveIndex >= scene.identities.size())
            return false;
        const GpuScenePrimitiveRecord& primitive =
            scene.primitives[primitiveIndex];
        if ((primitive.state.w & consumerMask) != consumerMask ||
            primitive.binding.x >= scene.published.instances ||
            primitive.binding.x >= scene.instances.size() ||
            primitive.binding.y >= scene.published.geometries ||
            primitive.binding.y >= scene.geometries.size())
            return false;
        const GpuSceneInstanceRecord& instance =
            scene.instances[primitive.binding.x];
        if ((instance.state.z & GpuSceneInstanceEnabled) == 0u ||
            (instance.state.w & consumerMask) != consumerMask ||
            instance.references.x >= scene.published.transforms ||
            instance.references.x >= scene.transforms.size())
            return false;
        const GpuSceneGeometryRecord& geometry =
            scene.geometries[primitive.binding.y];
        if ((geometry.storage.w & GpuSceneGeometryLegacyRhiHandle) == 0u ||
            geometry.storage.x == InvalidGpuSceneIndex ||
            geometry.draw.y == 0u)
            return false;
        caster = {
            .geometry = GeometryHandle{ geometry.storage.x },
            .material = MaterialHandle{ primitive.binding.z },
            .pipeline = PipelineHandle{ primitive.binding.w },
            .worldTransform = unpackGpuSceneAffine(
                scene.transforms[instance.references.x]),
            .boundsSphereCenterWorld = {
                instance.worldBoundsSphere.x,
                instance.worldBoundsSphere.y,
                instance.worldBoundsSphere.z,
            },
            .boundsSphereRadiusWorld = instance.worldBoundsSphere.w,
            .indexCount = geometry.draw.y,
            .firstIndex = geometry.draw.x,
            .owner = scene.identities[primitiveIndex].owner,
        };
        return caster.geometry.isValid() && caster.material.isValid() &&
            caster.pipeline.isValid();
    }

    glm::mat4 resolveIndirectCasterPreviousTransform(const VulkanIndirectScene& scene,
        uint32_t primitiveIndex, const glm::mat4& current) noexcept {
        if (primitiveIndex >= scene.primitives.size()) return current;
        const uint32_t instanceIndex = scene.primitives[primitiveIndex].binding.x;
        if (instanceIndex >= scene.instances.size()) return current;
        const uint32_t previous = scene.instances[instanceIndex].references.y;
        if (previous >= scene.published.transforms || previous >= scene.transforms.size())
            return current;
        return unpackGpuSceneAffine(scene.transforms[previous]);
    }

    GpuSceneIndirectFallbackReason evaluateIndirectPolicy(
        const GpuSceneIndirectPolicy& policy, size_t requested,
        uint32_t primitiveCapacity) noexcept {
        if (policy.forceDirectReference)
            return GpuSceneIndirectFallbackReason::DirectReference;
        if (!policy.multiDrawIndirect || !policy.drawIndirectFirstInstance ||
            !policy.drawIndirectCount || policy.maxDrawIndirectCount == 0u)
            return GpuSceneIndirectFallbackReason::MissingCapability;
        if (requested < policy.minimumCommandCount)
            return GpuSceneIndirectFallbackReason::TinyWorkload;
        if (requested > policy.maxDrawIndirectCount ||
            requested > primitiveCapacity)
            return GpuSceneIndirectFallbackReason::CapacityExceeded;
        return GpuSceneIndirectFallbackReason::None;
    }

    uint32_t residentLodPrefix(std::span<const GpuSceneGeometryRecord> geometries,
        std::span<const GpuSceneInstanceRecord> instances,
        const GpuScenePrimitiveRecord& primitive, uint32_t maximumLevel,
        const VulkanIndirectGeometry& base,
        const VulkanIndirectAssetResolver& resolver) {
        const uint32_t instanceIndex = primitive.binding.x;
        const uint32_t contentMaximumLod = instanceIndex < instances.size()
            ? gpuSceneInstanceMaximumLod(instances[instanceIndex].state.z) : 0u;
        const uint32_t effectiveMaximumLod =
            (std::min)(maximumLevel, contentMaximumLod);
        uint32_t maximumLod = 0u;
        uint32_t current = primitive.binding.y;
        for (uint32_t lod = 1u; lod <= effectiveMaximumLod; ++lod) {
            const uint32_t next = geometries[current].state.z;
            if (next >= geometries.size()) break;
            const GpuSceneGeometryRecord& child = geometries[next];
            VulkanIndirectGeometry childPayload{};
            const bool resident =
                (child.storage.w & GpuSceneGeometryLegacyRhiHandle) != 0u &&
                resolver.geometry(resolver.owner,
                    GeometryHandle{ child.storage.x }, childPayload);
            // Resident prefix only: never rebind a buffer inside an indirect bin.
            if (!resident ||
                childPayload.vertexBuffer != base.vertexBuffer ||
                childPayload.indexBuffer != base.indexBuffer ||
                childPayload.indexFormat != base.indexFormat ||
                child.draw.w != static_cast<uint32_t>(childPayload.indexFormat) ||
                std::bit_cast<int32_t>(child.draw.z) < 0 ||
                static_cast<uint64_t>(child.draw.z) * sizeof(Vertex) !=
                    childPayload.vertexOffset)
                break;
            maximumLod = lod;
            current = next;
        }
        return maximumLod;
    }

    VulkanIndirectBufferSet VulkanIndirectBufferSet::create(
        const VulkanCullerResources& resources, uint32_t commandCapacity,
        uint32_t countCapacity, uint32_t candidateCapacity) {
        VulkanIndirectBufferSet set{};
        try {
            for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight;
                    ++frame) {
                set.commands[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(commandCapacity) *
                        sizeof(GpuSceneIndexedIndirectCommand),
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                set.counts[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(countCapacity) * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
                set.candidates[frame] = resources.createBuffer(resources.user,
                    static_cast<uint64_t>(candidateCapacity) *
                        sizeof(GpuSceneIndirectCandidate),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            }
        }
        catch (...) {
            set.destroy(resources);
            throw;
        }
        return set;
    }

    void VulkanIndirectBufferSet::destroy(
        const VulkanCullerResources& resources) noexcept {
        for (VulkanBufferResource& buffer : commands)
            resources.destroyBuffer(resources.user, buffer);
        for (VulkanBufferResource& buffer : counts)
            resources.destroyBuffer(resources.user, buffer);
        for (VulkanBufferResource& buffer : candidates)
            resources.destroyBuffer(resources.user, buffer);
    }

    void VulkanIndirectBufferSet::destroySlot(
        const VulkanCullerResources& resources, uint32_t slot) noexcept {
        resources.destroyBuffer(resources.user, commands[slot]);
        resources.destroyBuffer(resources.user, counts[slot]);
        resources.destroyBuffer(resources.user, candidates[slot]);
    }

    void VulkanIndirectBufferSet::takeSlot(VulkanIndirectBufferSet& source,
        uint32_t slot) noexcept {
        commands[slot] = source.commands[slot];
        counts[slot] = source.counts[slot];
        candidates[slot] = source.candidates[slot];
        source.commands[slot] = {};
        source.counts[slot] = {};
        source.candidates[slot] = {};
    }

    void bindIndirectBufferSlot(const VulkanCullerResources& resources,
        VkDescriptorSet set, const VulkanIndirectBufferSet& buffers,
        uint32_t slot) {
        const std::array<VkDescriptorBufferInfo, 3> infos{ {
            { buffers.candidates[slot].buffer, 0,
                buffers.candidates[slot].size },
            { buffers.commands[slot].buffer, 0,
                buffers.commands[slot].size },
            { buffers.counts[slot].buffer, 0, buffers.counts[slot].size },
        } };
        resources.writeStorageBuffers(resources.user, set, 0u, infos);
    }

    void bindIndirectBufferSet(const VulkanCullerResources& resources,
        std::span<const VkDescriptorSet, kIndirectCullerFramesInFlight> sets,
        const VulkanIndirectBufferSet& buffers) {
        for (uint32_t frame = 0; frame < kIndirectCullerFramesInFlight; ++frame)
            bindIndirectBufferSlot(resources, sets[frame], buffers, frame);
    }

    VkDescriptorSetLayout createIndirectSetLayout(VkDevice device,
        const char* subject) {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[binding].descriptorCount = 1u;
            bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &layout) !=
                VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create ") + subject +
                " indirect descriptor layout");
        return layout;
    }

    VkPipelineLayout createComputePipelineLayout(VkDevice device,
        std::span<const VkDescriptorSetLayout> setLayouts, uint32_t pushWords,
        const char* objectName) {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = pushWords * sizeof(uint32_t);
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = 1u;
        layoutInfo.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) !=
                VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create ") +
                objectName + " pipeline layout");
        return layout;
    }

    VkPipeline createComputePipeline(VkDevice device, VkPipelineCache pipelineCache,
        VkPipelineLayout layout, const char* shaderRelativePath,
        const char* objectName) {
        const std::vector<char> code = readFile(std::string(PROJECT_ROOT_DIR) +
            shaderRelativePath);
        VkShaderModuleCreateInfo moduleInfo{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (code.empty() || code.size() % sizeof(uint32_t) != 0u ||
            vkCreateShaderModule(device, &moduleInfo, nullptr, &module) !=
                VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create ") +
                objectName + " shader module");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0u, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr };
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(device, pipelineCache,
            1u, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string("failed to create ") +
                objectName + " compute pipeline");
        return pipeline;
    }

} // namespace Iridium
