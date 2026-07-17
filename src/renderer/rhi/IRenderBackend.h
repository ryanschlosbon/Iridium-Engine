#pragma once
#include "DrawPacket.h"
#include "PipelineTypes.h"
#include "TextureTypes.h"
#include "RhiResourceTypes.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <glm/glm.hpp>

// Forward declare GLFWwindow so we don't need to include the heavy GLFW header
// inside our clean abstraction layer.
struct GLFWwindow;

namespace Iridium {

    class IRenderBackend {
    public:
        // A virtual destructor is MANDATORY for C++ interfaces to ensure child classes 
        // (like VulkanVertexBackend) correctly fire their own destructors.
        virtual ~IRenderBackend() = default;

        // ==============================================================================
        // 1. SYSTEM LIFECYCLE
        // ==============================================================================
        virtual void init(GLFWwindow* window) = 0;
        virtual void cleanup() = 0;
        virtual void recreateSwapchain(GLFWwindow* window) = 0;

        // ==============================================================================
        // 2. THE FRAME PIPELINE
        // ==============================================================================

        // Prepares swapchains, acquires the next image, and resets command buffers
        virtual FrameStatus beginFrame() = 0;

        virtual void updateCamera(const glm::mat4& view, const glm::mat4& proj) = 0;

        // Pass 1: Draws opaque meshes to the G-Buffer (Normal, Albedo, Depth)
        virtual void submitOpaqueQueue(std::span<const DrawPacket> opaqueQueue,
            std::span<const DrawPacket> selectionQueue, bool isWireframe) = 0;
        
        // Pass 2: Evaluates the G-Buffer using the HDRI and outputs the lit scene
        virtual void submitLightingPass(const glm::vec3& cameraPos, const glm::mat4& view, const glm::mat4& proj) = 0;

        // Pass 3: Sorts and draws the glass objects back-to-front over the lit scene
        virtual void submitTransparentQueue(std::span<const DrawPacket> transparentQueue) = 0;

        // Pass 4: Draws the ImGui editor and final UI elements to the swapchain
        virtual void submitUIPass() = 0;

        virtual void beginUI() = 0;

        // ImGui uses 'void*' for its abstract texture IDs
        virtual void* getLitSceneTextureID() = 0;
        virtual void* getGlassDepthTextureID() = 0;

        // Submits the command buffers to the GPU and presents to the monitor
        virtual FrameStatus endFrame() = 0;

        // ==============================================================================
        // 3. RESOURCE MANAGEMENT (The Vault Doors)
        // ==============================================================================
        // Notice how none of these use VkBuffer, VkImage, or VkDescriptorSet.
        // We pass pure data in, and we get a lightweight Handle back.

        // --- GEOMETRY ---
        virtual GeometryHandle allocateGeometry(const GeometryDesc& desc,
            std::span<const std::byte> vertexBytes,
            std::span<const std::byte> indexBytes) = 0;
        virtual void freeGeometry(GeometryHandle handle) = 0;

        // --- TEXTURES ---
        virtual TextureHandle allocateTexture(const TextureDesc& desc,
            std::span<const std::byte> pixelBytes) = 0;
        virtual void freeTexture(TextureHandle handle) = 0;

        // --- MATERIALS ---
        // The backend uses these texture handles to construct the API-specific descriptors
        virtual MaterialBinding allocateMaterial(const MaterialAsset& desc) = 0;
        virtual void freeMaterial(MaterialHandle handle) = 0;

        virtual void setEnvironmentMap(TextureHandle hdriHandle) = 0;
    };

} // namespace Iridium
