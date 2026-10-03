#pragma once

// Shared core of the GPU-driven indirect cullers (M7R R3a): the directional,
// spot, point and reflection-probe view culler (VulkanIndirectViewCuller) and
// the main-view opaque culler (VulkanOpaqueIndirectCuller).
//
// Everything a culler records or allocates goes through two function-pointer
// tables. VulkanCullerCommands is the command-buffer seam (pipeline/set binds,
// push constants, dispatches, memory barriers, indirect draws and GPU ranges);
// VulkanCullerResources is the device-object seam (buffers, descriptor sets and
// writes, frame drains). Both default to the real Vulkan calls. They are
// Vulkan-side seams, not RHI hooks: a device-free test can replace them to log
// the exact command stream.

#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneIndirect.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "core/types/RenderHandles.h"
#include "core/types/SceneEntityUuid.h"
#include "VulkanBackendExtension.h"
#include "VulkanFrameScheduler.h"
#include "VulkanResourceAllocator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

class DescriptorAllocator;

namespace Iridium {

    class CpuProfiler;

    inline constexpr uint32_t kIndirectCullerFramesInFlight =
        VulkanFrameScheduler::FramesInFlight;

    [[nodiscard]] VkIndexType toVkIndexType(IndexFormat format);

    // ---------------------------------------------------------------------
    // Command-buffer seam.
    // ---------------------------------------------------------------------
    struct VulkanCullerCommands {
        void* user = nullptr;
        void (*bindPipeline)(void* user, VkCommandBuffer cmd,
            VkPipelineBindPoint bindPoint, VkPipeline pipeline) = nullptr;
        void (*bindDescriptorSets)(void* user, VkCommandBuffer cmd,
            VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
            uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets,
            uint32_t dynamicOffsetCount,
            const uint32_t* dynamicOffsets) = nullptr;
        void (*pushConstants)(void* user, VkCommandBuffer cmd,
            VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
            uint32_t size, const void* values) = nullptr;
        void (*dispatch)(void* user, VkCommandBuffer cmd, uint32_t groupsX,
            uint32_t groupsY, uint32_t groupsZ) = nullptr;
        // One VkMemoryBarrier, no buffer or image barriers, no dependency flags.
        void (*memoryBarrier)(void* user, VkCommandBuffer cmd,
            VkPipelineStageFlags srcStages, VkPipelineStageFlags dstStages,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess) = nullptr;
        void (*bindVertexBuffer)(void* user, VkCommandBuffer cmd,
            VkBuffer buffer, VkDeviceSize offset) = nullptr;
        void (*bindIndexBuffer)(void* user, VkCommandBuffer cmd,
            VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) = nullptr;
        void (*drawIndexedIndirectCount)(void* user, VkCommandBuffer cmd,
            VkBuffer commandBuffer, VkDeviceSize commandOffset,
            VkBuffer countBuffer, VkDeviceSize countOffset,
            uint32_t maxDrawCount, uint32_t stride) = nullptr;
        VulkanGpuRangeToken (*beginGpuRange)(void* user,
            const char* name) = nullptr;
        void (*endGpuRange)(void* user, VulkanGpuRangeToken& token) = nullptr;

        // vkCmd* recording; GPU ranges through `scheduler` (may be null: no
        // ranges).
        [[nodiscard]] static VulkanCullerCommands vulkan(
            VulkanFrameScheduler* scheduler) noexcept;
    };

    // ---------------------------------------------------------------------
    // Device-object seam.
    // ---------------------------------------------------------------------
    struct VulkanCullerResources {
        void* user = nullptr;
        // Host-visible, host-coherent, persistently mapped, GPU-scene category.
        VulkanBufferResource (*createBuffer)(void* user, VkDeviceSize bytes,
            VkBufferUsageFlags usage) = nullptr;
        void (*destroyBuffer)(void* user, VulkanBufferResource& buffer) = nullptr;
        VkDescriptorSet (*allocateSet)(void* user,
            VkDescriptorSetLayout layout) = nullptr;
        void (*freeSet)(void* user, VkDescriptorSet set) = nullptr;
        // Storage-buffer descriptors at consecutive bindings from firstBinding.
        void (*writeStorageBuffers)(void* user, VkDescriptorSet set,
            uint32_t firstBinding,
            std::span<const VkDescriptorBufferInfo> buffers) = nullptr;
        void (*writeCombinedImageSampler)(void* user, VkDescriptorSet set,
            uint32_t binding, VkSampler sampler, VkImageView view,
            VkImageLayout layout) = nullptr;
        // Waits for every frame in flight (capacity growth).
        void (*waitForAllFrames)(void* user) = nullptr;
    };

    // The objects VulkanCullerResources::vulkan forwards to; owned by the
    // backend and outliving every culler.
    struct VulkanCullerDevice {
        VkDevice device = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator = nullptr;
        ::DescriptorAllocator* descriptors = nullptr;
        VulkanFrameScheduler* scheduler = nullptr;

        [[nodiscard]] VulkanCullerResources resources() noexcept;
    };

    // Device limits that gate indirect submission.
    struct VulkanIndirectCapabilities {
        bool multiDrawIndirect = false;
        bool drawIndirectFirstInstance = false;
        bool drawIndirectCount = false;
        uint32_t maxDrawIndirectCount = 0;
    };

    // ---------------------------------------------------------------------
    // Scene access (CPU mirror of one frame slot's GPU-scene tables).
    // ---------------------------------------------------------------------
    struct VulkanIndirectScene {
        std::span<const GpuSceneAffineTransform> transforms{};
        std::span<const GpuSceneInstanceRecord> instances{};
        std::span<const GpuScenePrimitiveRecord> primitives{};
        std::span<const GpuSceneGeometryRecord> geometries{};
        std::span<const GpuScenePrimitiveIdentity> identities{};
        GpuSceneCapacityRequirements published{};
    };

    struct VulkanIndirectGeometry {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        IndexFormat indexFormat = IndexFormat::UInt32;
        VkDeviceSize vertexOffset = 0;
    };

    struct VulkanIndirectMaterial {
        uint32_t alphaMode = 0;
        uint32_t doubleSided = 0;
    };

    // Resolves runtime handles to the payload fields the cullers inspect.
    struct VulkanIndirectAssetResolver {
        const void* owner = nullptr;
        bool (*geometry)(const void* owner, GeometryHandle handle,
            VulkanIndirectGeometry& geometry) = nullptr;
        bool (*material)(const void* owner, MaterialHandle handle,
            VulkanIndirectMaterial& material) = nullptr;
        // Whether a G-buffer pipeline has a GPU-scene indirect variant.
        bool (*gbufferIndirectPipeline)(const void* owner,
            PipelineHandle handle) = nullptr;
    };

    // A GPU-scene or direct caster resolved for shadow/probe visibility.
    struct VulkanResolvedCaster {
        GeometryHandle geometry;
        MaterialHandle material;
        PipelineHandle pipeline;
        glm::mat4 worldTransform{ 1.0f };
        glm::vec3 boundsSphereCenterWorld{ 0.0f };
        float boundsSphereRadiusWorld = -1.0f;
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        uint32_t gpuScenePrimitiveIndex = InvalidGpuSceneIndex;
        SceneEntityUuid owner;
    };

    // Resolves a published GPU-scene primitive for `consumerMask`; false when
    // it is unpublished, disabled for the consumer or not legacy-addressable.
    [[nodiscard]] bool resolveIndirectCaster(const VulkanIndirectScene& scene,
        uint32_t primitiveIndex, uint32_t consumerMask,
        VulkanResolvedCaster& caster) noexcept;

    // The fixed indirect reject ladder shared by the shadow and probe views.
    [[nodiscard]] GpuSceneIndirectFallbackReason evaluateIndirectPolicy(
        const GpuSceneIndirectPolicy& policy, size_t requested,
        uint32_t primitiveCapacity) noexcept;

    // Longest resident, buffer-compatible LOD prefix of `primitive`'s geometry
    // chain, capped by `maximumLevel` and the instance's content maximum. A
    // child that would rebind a buffer inside an indirect bin ends the prefix.
    [[nodiscard]] uint32_t residentLodPrefix(
        std::span<const GpuSceneGeometryRecord> geometries,
        std::span<const GpuSceneInstanceRecord> instances,
        const GpuScenePrimitiveRecord& primitive, uint32_t maximumLevel,
        const VulkanIndirectGeometry& base,
        const VulkanIndirectAssetResolver& resolver);

    // ---------------------------------------------------------------------
    // Per-frame command/count/candidate buffers of one culler.
    // ---------------------------------------------------------------------
    struct VulkanIndirectBufferSet {
        std::array<VulkanBufferResource, kIndirectCullerFramesInFlight> commands{};
        std::array<VulkanBufferResource, kIndirectCullerFramesInFlight> counts{};
        std::array<VulkanBufferResource, kIndirectCullerFramesInFlight> candidates{};

        // Per frame: commands, counts, candidates. Destroys what it made and
        // rethrows on failure.
        [[nodiscard]] static VulkanIndirectBufferSet create(
            const VulkanCullerResources& resources, uint32_t commandCapacity,
            uint32_t countCapacity, uint32_t candidateCapacity);
        void destroy(const VulkanCullerResources& resources) noexcept;
    };

    // The shared 3-binding compute set: candidates, commands, counts.
    void bindIndirectBufferSet(const VulkanCullerResources& resources,
        std::span<const VkDescriptorSet, kIndirectCullerFramesInFlight> sets,
        const VulkanIndirectBufferSet& buffers);

    [[nodiscard]] VkDescriptorSetLayout createIndirectSetLayout(VkDevice device,
        const char* subject);
    // `objectName` completes the failure messages ("failed to create
    // <objectName> pipeline layout" / "shader module" / "compute pipeline").
    [[nodiscard]] VkPipelineLayout createComputePipelineLayout(VkDevice device,
        std::span<const VkDescriptorSetLayout> setLayouts, uint32_t pushWords,
        const char* objectName);
    [[nodiscard]] VkPipeline createComputePipeline(VkDevice device,
        VkPipelineLayout layout, const char* shaderRelativePath,
        const char* objectName);

} // namespace Iridium
