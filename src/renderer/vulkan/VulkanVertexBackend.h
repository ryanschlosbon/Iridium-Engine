#pragma once
#include "../rhi/IRenderBackend.h"
#include "../rhi/ResourcePool.h"

#include "VkContext.h"
#include "VkSwapchain.h"
#include "VkCommandManager.h"
#include "VkSyncObjects.h"
#include "DescriptorAllocator.h"

// Pipelines & Passes
#include "VkGraphicsPipeline.h"
#include "VkLightingPipeline.h"
#include "VkForwardPipeline.h"
#include "GlassDepthPipeline.h"
#include "VkRenderPass.h"
#include "VkForwardRenderPass.h"
#include "GlassDepthRenderPass.h"
#include "VkFramebuffer.h"
#include "VkUIRenderPass.h"
#include "PipelineCache.h"

#include "utils/DeletionQueue.h"

namespace Iridium {

    // ==============================================================================
    // THE INTERNAL PAYLOADS
    // These structs only exist inside the Backend. The ECS never sees them.
    // ==============================================================================

    struct VulkanGeometryPayload {
        VkBuffer vertexBuffer;
        VkDeviceMemory vertexBufferMemory;
        VkBuffer indexBuffer;
        VkDeviceMemory indexBufferMemory;
        uint32_t indexCount;
    };

    struct VulkanTexturePayload {
        VkImage image;
        VkDeviceMemory memory;
        VkImageView view;
        VkSampler sampler;
        bool isHDRI = false;
    };

    struct VulkanMaterialPayload {
        // A material is ultimately just the Vulkan descriptor sets bound to the pipeline.
        // We need one set per frame-in-flight to prevent GPU/CPU synchronization crashes.
        std::vector<VkDescriptorSet> descriptorSets;

        glm::vec4 baseColor;
        float metallicFactor;
        float roughnessFactor;
        float emissiveFactor;

        VkPipeline pipeline = VK_NULL_HANDLE;
        BlendMode blendMode = BlendMode::Opaque;
    };

    // ==============================================================================
    // THE CONCRETE BACKEND
    // ==============================================================================

    class VulkanVertexBackend : public IRenderBackend {
    private:
        // --- 1. THE SUBSYSTEMS (Composition) ---
        // We moved all of these pointers out of Application.cpp and into here.
        VkContext* vkContext = nullptr;
        VkSwapchain* vkSwapchain = nullptr;
        VkCommandManager* vkCommandManager = nullptr;
        VkSyncObjects* vkSyncObjects = nullptr;
        DescriptorAllocator descriptorAllocator;
        PipelineCache pipelineCache;

        // G-Buffer Pass (Opaque)
        VkRenderPassWrapper* gBufferPass = nullptr;
        VkFramebufferWrapper* gBufferFramebuffers = nullptr;
        VkGraphicsPipeline* gBufferPipeline = nullptr;

        // --- MISSING RAW IMAGE ARRAYS ---
        VkSampler gBufferSampler = VK_NULL_HANDLE;

        // G-Buffer Raw Images
        std::vector<VkImage> gNormalImages, gAlbedoImages;
        std::vector<VkDeviceMemory> gNormalImageMemories, gAlbedoImageMemories;
        std::vector<VkImageView> gNormalImageViews, gAlbedoImageViews;

        // Lighting Pass Raw Images & Descriptors
        std::vector<VkDescriptorSet> lightingDescriptorSets;
        std::vector<VkImage> litSceneImages;
        std::vector<VkDeviceMemory> litSceneImageMemories;
        std::vector<VkImageView> litSceneImageViews;

        // Translucency Pass Raw Images
        std::vector<VkImage> opaqueSceneCopyImages, glassDepthImages;
        std::vector<VkDeviceMemory> opaqueSceneCopyMemories, glassDepthMemories;
        std::vector<VkImageView> opaqueSceneCopyViews, glassDepthViews;

        // Depth Pass
        std::vector<VkImage> gDepthImages;
        std::vector<VkDeviceMemory> gDepthImageMemories;
        std::vector<VkImageView> gDepthImageViews;

        // Deferred Lighting Pass
        VkRenderPass lightingRenderPass = VK_NULL_HANDLE;
        VkLightingPipeline* lightingPipeline = nullptr;
        std::vector<VkFramebuffer> lightingFramebuffers;

        // Translucency Passes
        GlassDepthRenderPass* glassDepthPass = nullptr;
        GlassDepthPipeline* glassDepthPipeline = nullptr;
        std::vector<VkFramebuffer> glassDepthFramebuffers;

        VkForwardRenderPass* forwardPass = nullptr;
        VkForwardPipeline* forwardPipeline = nullptr;
        std::vector<VkFramebuffer> forwardFramebuffers;

        // UI Pass
        VkUIRenderPass* uiPass = nullptr;
        std::vector<VkFramebuffer> uiFramebuffers;

        // --- IMGUI STATE ---
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> uiSceneTextures;
        std::vector<VkDescriptorSet> uiDepthTextures;

        // Global Camera Data
        std::vector<VkBuffer> uniformBuffers;
        std::vector<VkDeviceMemory> uniformBuffersMemory;
        std::vector<void*> uniformBuffersMapped;
        std::vector<VkDescriptorSet> globalDescriptorSets;

        // --- 2. THE MEMORY VAULTS ---
        // This is where the Handles are mapped to the physical Vulkan memory.
        ResourcePool<VulkanGeometryPayload, GeometryHandle> geometryVault;
        ResourcePool<VulkanTexturePayload, TextureHandle> textureVault;
        ResourcePool<VulkanMaterialPayload, MaterialHandle> materialVault;


        // --- 3. RUNTIME STATE ---
        uint32_t currentFrame = 0;
        uint32_t currentImageIndex = 0;
        VkCommandBuffer currentCmd = VK_NULL_HANDLE;
        std::array<DeletionQueue, VkSyncObjects::MAX_FRAMES_IN_FLIGHT> frameDeletionQueues;

        // Private helpers that Application.cpp no longer needs to worry about
        void createOffscreenRenderTargets();
        void createUniformBuffers();
        void updateUniformBuffer(const glm::mat4& view, const glm::mat4& proj);
        void createLightingRenderPass();
        void destroyOffscreenRenderTargets();

    public:
        VulkanVertexBackend() = default;
        ~VulkanVertexBackend() override = default;

        // --- IRenderBackend Interface Implementation ---
        void init(GLFWwindow* window) override;
        void cleanup() override;
        void recreateSwapchain(GLFWwindow* window) override;
        bool beginFrame() override;
        void updateCamera(const glm::mat4& view, const glm::mat4& proj) override;

        virtual void submitOpaqueQueue(const std::vector<DrawPacket>& opaqueQueue, 
            const std::vector<DrawPacket>& selectionQueue, bool isWireframe) override;
        void submitLightingPass(const glm::vec3& cameraPos, const glm::mat4& view, const glm::mat4& proj) override;
        void submitGlassDepthPass(const std::vector<DrawPacket>& transparentQueue) override;
        void submitTransparentQueue(const std::vector<DrawPacket>& transparentQueue) override;
        void submitUIPass() override;

        void beginUI() override;
        void* getLitSceneTextureID() override;
        void* getGlassDepthTextureID() override;

        void endFrame() override;

        // Resource Allocation
        GeometryHandle allocateGeometry(const void* vertexData, size_t vertexSize,
            const void* indexData, size_t indexSize) override;
        void freeGeometry(GeometryHandle handle) override;

        TextureHandle allocateTexture(uint32_t width, uint32_t height, int channels,
            const void* pixelData, bool isHDRI = false) override;
        void freeTexture(TextureHandle handle) override;

        MaterialHandle allocateMaterial(const MaterialAsset& desc) override;
        void freeMaterial(MaterialHandle handle) override;

        void setEnvironmentMap(TextureHandle hdriHandle) override;
    };

} // namespace Iridium