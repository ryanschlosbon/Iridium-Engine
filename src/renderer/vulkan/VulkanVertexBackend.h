#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../rhi/IRenderBackend.h"
#include "../rhi/ResourcePool.h"

#include "VkContext.h"
#include "VkSwapchain.h"
#include "DescriptorAllocator.h"

// Pipelines & Passes
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "GlassDepthPipeline.h"
#include "VkRenderPass.h"
#include "VkForwardRenderPass.h"
#include "GlassDepthRenderPass.h"
#include "VkUIRenderPass.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceAllocator.h"
#include "VulkanCommandList.h"
#include "VulkanUploadContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanFrameTargets.h"
#include "VulkanSceneDescriptors.h"

#include "utils/DeletionQueue.h"

namespace Iridium {

    // ==============================================================================
    // THE INTERNAL PAYLOADS
    // These structs only exist inside the Backend. The ECS never sees them.
    // ==============================================================================

    struct VulkanGeometryPayload {
        VulkanBufferResource vertexBuffer;
        VulkanBufferResource indexBuffer;
        uint32_t indexCount = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
    };

    struct VulkanTexturePayload {
        VulkanImageResource image;
        VkSampler sampler = VK_NULL_HANDLE;
        TextureFormat format = TextureFormat::RGBA8_UNorm;
    };

    struct VulkanMaterialPayload {
        // One descriptor set per frame-in-flight prevents GPU/CPU synchronization crashes.
        std::array<VkDescriptorSet, VulkanFrameScheduler::FramesInFlight> descriptorSets{};

        glm::vec4 baseColor;
        glm::vec4 emissiveFactor;
        float metallicFactor;
        float roughnessFactor;
        float normalScale;
        PipelineHandle pipeline;
        RenderQueue renderQueue = RenderQueue::Opaque;
        float alphaCutoff = 0.0f;
        float transmissionFactor = 0.0f;
    };

    // ==============================================================================
    // THE CONCRETE BACKEND
    // ==============================================================================

    class VulkanVertexBackend : public IRenderBackend {
    private:
        // --- 1. THE SUBSYSTEMS (Composition) ---
        // We moved all of these pointers out of Application.cpp and into here.
        std::unique_ptr<VkContext> vkContext;
        VulkanResourceAllocator resourceAllocator;
        VulkanUploadContext uploadContext;
        VulkanFrameScheduler scheduler;
        std::unique_ptr<VkSwapchain> vkSwapchain;
        DescriptorAllocator descriptorAllocator;
        VulkanPipelineLibrary pipelineLibrary;
        VulkanMeshLayouts meshLayouts;

        // G-Buffer Pass (Opaque)
        std::unique_ptr<VkRenderPassWrapper> gBufferPass;
        std::unique_ptr<VkGraphicsPipeline> gBufferPipeline;

        // --- MISSING RAW IMAGE ARRAYS ---
        VulkanFrameTargets frameTargets;

        // G-Buffer Raw Images

        // Lighting Pass Raw Images & Descriptors
        VulkanSceneDescriptors sceneDescriptors;
        TextureHandle environmentMapHandle;

        // Translucency Pass Raw Images

        // Depth Pass

        // Deferred Lighting Pass
        VkRenderPass lightingRenderPass = VK_NULL_HANDLE;
        std::unique_ptr<VkLightingPipeline> lightingPipeline;

        // Translucency Passes
        std::unique_ptr<GlassDepthRenderPass> glassDepthPass;
        std::unique_ptr<GlassDepthPipeline> glassDepthPipeline;

        std::unique_ptr<VkForwardRenderPass> forwardPass;

        // UI Pass
        std::unique_ptr<VkUIRenderPass> uiPass;

        // --- IMGUI STATE ---
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> uiSceneTextures;
        std::vector<VkDescriptorSet> uiDepthTextures;

        // Global Camera Data
        std::vector<VulkanBufferResource> uniformBuffers;
        std::vector<VkDescriptorSet> globalDescriptorSets;

        // --- 2. THE MEMORY VAULTS ---
        // This is where the Handles are mapped to the physical Vulkan memory.
        ResourcePool<VulkanGeometryPayload, GeometryHandle> geometryVault;
        ResourcePool<VulkanTexturePayload, TextureHandle> textureVault;
        ResourcePool<VulkanMaterialPayload, MaterialHandle> materialVault;


        // --- 3. RUNTIME STATE ---
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        bool initialized_ = false;
        bool cleaned_ = false;
        bool imguiInitialized_ = false;

        // Private helpers that Application.cpp no longer needs to worry about
        void createUniformBuffers();
        void initFrameTargets();
        void updateUniformBuffer(const glm::mat4& view, const glm::mat4& proj);
        void createLightingRenderPass();

    public:
        VulkanVertexBackend() = default;
        ~VulkanVertexBackend() override { cleanup(); }

        // --- IRenderBackend Interface Implementation ---
        void init(GLFWwindow* window) override;
        void cleanup() override;
        void recreateSwapchain(GLFWwindow* window) override;
        FrameStatus beginFrame() override;
        void updateCamera(const glm::mat4& view, const glm::mat4& proj) override;

        void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) override;
        void submitLightingPass(const glm::vec3& cameraPos, const glm::mat4& view, const glm::mat4& proj) override;
        void submitTransparentQueue(std::span<const DrawPacket> transparentQueue) override;
        void submitUIPass() override;

        void beginUI() override;
        void* getLitSceneTextureID() override;
        void* getGlassDepthTextureID() override;

        FrameStatus endFrame() override;

        // Resource Allocation
        GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes) override;
        void freeGeometry(GeometryHandle handle) override;

        TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes) override;
        void freeTexture(TextureHandle handle) override;

        MaterialBinding allocateMaterial(const MaterialAsset& desc) override;
        void freeMaterial(MaterialHandle handle) override;

        void setEnvironmentMap(TextureHandle hdriHandle) override;
    };

} // namespace Iridium
