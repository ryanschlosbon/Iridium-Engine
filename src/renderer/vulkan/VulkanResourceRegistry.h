#pragma once

// M7R R3c.0: geometry/texture/material vaults, the sampler cache, the
// indexed material texture table and the canonical material buffers, moved
// out of VulkanVertexBackend unchanged. The backend's IRenderBackend
// resource methods forward here; feature owners resolve handles through it.

#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/ResourcePool.h"

#include "VulkanFrameScheduler.h"
#include "VulkanIndexedTextureTable.h"
#include "VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Iridium {

    class CpuProfiler;
    class VulkanPipelineLibrary;
    class VulkanUploadContext;

    struct VulkanGeometryPayload {
        VulkanBufferResource vertexBuffer;
        VulkanBufferResource indexBuffer;
        VulkanBufferResource arenaUInt16IndexBuffer;
        VulkanBufferResource arenaUInt32IndexBuffer;
        VkDeviceSize vertexOffset = 0;
        uint32_t indexCount = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
        bool arenaAllocation = false;
        bool ownsArenaBuffers = false;
    };

    struct VulkanTexturePayload {
        VulkanImageResource image;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t samplerCacheIndex = UINT32_MAX;
        VkDescriptorSet editorDescriptor = VK_NULL_HANDLE;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
        uint32_t width = 0;
        uint32_t height = 0;
        bool retired = false;
    };

    struct VulkanMaterialPayload {
        PipelineHandle pipeline;
        PipelineHandle mirroredPipeline;
        RenderQueue renderQueue = RenderQueue::Opaque;
        PackedGpuMaterial packed{};
        uint64_t packedRevision = 0;
        std::array<uint64_t, VulkanFrameScheduler::FramesInFlight>
            uploadedPackedRevisions{};
    };

    class VulkanResourceRegistry final {
    public:
        using GeometryVault = ResourcePool<VulkanGeometryPayload, GeometryHandle>;
        using TextureVault = ResourcePool<VulkanTexturePayload, TextureHandle>;
        using MaterialVault = ResourcePool<VulkanMaterialPayload, MaterialHandle>;

        struct Services {
            VkDevice device = VK_NULL_HANDLE;
            VulkanResourceAllocator* allocator = nullptr;
            VulkanUploadContext* uploads = nullptr;
            VulkanFrameScheduler* scheduler = nullptr;
            VulkanPipelineLibrary* pipelines = nullptr;
            CpuProfiler* profiler = nullptr;
            // The backend's open-frame flag (growth is frame-boundary only).
            const bool* frameOpen = nullptr;
        };

        VulkanResourceRegistry() = default;
        VulkanResourceRegistry(const VulkanResourceRegistry&) = delete;
        VulkanResourceRegistry& operator=(const VulkanResourceRegistry&) = delete;

        void init(const Services& services);
        // Releases an editor-bridge descriptor when a retired texture is
        // finally destroyed; the registry itself has no editor dependency.
        void setEditorDescriptorRelease(void* owner,
            void (*release)(void* owner, VkDescriptorSet descriptor)) noexcept {
            editorReleaseOwner_ = owner;
            editorRelease_ = release;
        }

        [[nodiscard]] GeometryVault& geometries() noexcept { return geometryVault_; }
        [[nodiscard]] const GeometryVault& geometries() const noexcept { return geometryVault_; }
        [[nodiscard]] TextureVault& textures() noexcept { return textureVault_; }
        [[nodiscard]] const TextureVault& textures() const noexcept { return textureVault_; }
        [[nodiscard]] MaterialVault& materials() noexcept { return materialVault_; }
        [[nodiscard]] const MaterialVault& materials() const noexcept { return materialVault_; }
        [[nodiscard]] VulkanIndexedTextureTable& textureTable() noexcept {
            return indexedTextureTable_;
        }
        [[nodiscard]] const VulkanIndexedTextureTable& textureTable() const noexcept {
            return indexedTextureTable_;
        }

        GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes);
        void freeGeometry(GeometryHandle handle);
        GeometryArenaAllocation allocateGeometryArena(uint32_t vertexStride,
            std::span<const std::byte> vertexBytes,
            const GeometryArenaData& arena);
        void freeGeometryArena(std::span<const GeometryHandle> primitiveGeometry);

        TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes);
        void freeTexture(TextureHandle handle);

        MaterialBinding allocateCanonicalMaterial(const CanonicalMaterialAsset& asset);
        void updateCanonicalMaterial(MaterialHandle handle,
            const PackedGpuMaterial& material);
        void freeMaterial(MaterialHandle handle);

        // Canonical material table (one host-visible buffer per frame slot).
        void setMaterialTableMaximumCapacity(uint32_t maximumCapacity) noexcept {
            canonicalMaterialMaximumCapacity_ = maximumCapacity;
        }
        // M7R R4c.2: a slot that is not in flight swaps at once; an in-flight
        // slot keeps its table until swapRetiredSlot (no drain).
        void createCanonicalMaterialBuffers(uint32_t capacity);
        // At `slot`'s retirement: installs its parked material table and
        // rebinds the slot's material set. True when the slot changed.
        bool swapRetiredSlot(uint32_t slot);
        [[nodiscard]] bool slotSwapPending(uint32_t slot) const noexcept {
            return pendingMaterialSlots_[slot];
        }
        void ensureCanonicalMaterialCapacity(uint32_t requiredCapacity);
        void uploadCanonicalMaterialsForFrame(uint32_t frameIndex);
        [[nodiscard]] uint32_t materialTableCapacity() const noexcept {
            return canonicalMaterialCapacity_;
        }
        [[nodiscard]] uint32_t materialTableMaximumCapacity() const noexcept {
            return canonicalMaterialMaximumCapacity_;
        }
        // Binds the frame's indexed material sets at set 1 of `layout`.
        void bindMaterialDescriptors(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, VkPipelineLayout layout) const;

        [[nodiscard]] uint32_t acquireSampler(const SamplerDesc& desc);
        void releaseSampler(uint32_t cacheIndex) noexcept;
        [[nodiscard]] uint64_t liveSamplerCount() const noexcept;
        [[nodiscard]] uint64_t cachedSamplerCount() const noexcept {
            return samplerCache_.size();
        }
        [[nodiscard]] uint64_t retiredTextureCount() const noexcept {
            return retiredTextureCount_;
        }

        // After device idle: destroys every payload's buffers and images, the
        // sampler cache and the material buffers (the table is cleaned up
        // separately, after the layouts that reference it).
        void destroyResources() noexcept;
        void reset() noexcept;

    private:
        struct CachedSampler {
            SamplerDesc desc{};
            VkSampler sampler = VK_NULL_HANDLE;
            uint32_t referenceCount = 0;
        };

        void cleanupSamplerCache() noexcept;
        // Deletion-queue callback of freeTexture (R4c.1).
        static void releaseRetiredTexture(void* user,
            const VulkanDeletionArguments& arguments);
        [[nodiscard]] bool frameOpen() const noexcept {
            return frameOpen_ != nullptr && *frameOpen_;
        }

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        VulkanUploadContext* uploads_ = nullptr;
        VulkanFrameScheduler* scheduler_ = nullptr;
        VulkanPipelineLibrary* pipelines_ = nullptr;
        CpuProfiler* profiler_ = nullptr;
        const bool* frameOpen_ = nullptr;
        void* editorReleaseOwner_ = nullptr;
        void (*editorRelease_)(void* owner, VkDescriptorSet descriptor) = nullptr;

        GeometryVault geometryVault_;
        TextureVault textureVault_;
        MaterialVault materialVault_;
        std::vector<CachedSampler> samplerCache_;
        uint64_t retiredTextureCount_ = 0;
        VulkanIndexedTextureTable indexedTextureTable_;
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            canonicalMaterialBuffers_{};
        std::array<VulkanBufferResource, VulkanFrameScheduler::FramesInFlight>
            pendingMaterialBuffers_{};
        std::array<bool, VulkanFrameScheduler::FramesInFlight>
            pendingMaterialSlots_{};
        uint32_t canonicalMaterialCapacity_ = 0;
        uint32_t canonicalMaterialMaximumCapacity_ = 0;
    };

} // namespace Iridium
