#pragma once

#include "VulkanReflectionProbeCaptureTargets.h"
#include "VulkanResourceAllocator.h"
#include "renderer/lighting/ReflectionProbeCapture.h"
#include "renderer/rhi/Mesh.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

class DescriptorAllocator;

namespace Iridium {

    struct alignas(16) VulkanReflectionProbeCaptureFaceData {
        glm::mat4 worldToClip{ 1.0f };
        glm::mat4 clipToWorld{ 1.0f };
        glm::vec4 capturePositionNear{};
        glm::uvec4 metadata{}; // active lights, capture sky, resolution, face
    };

    static_assert(sizeof(VulkanReflectionProbeCaptureFaceData) == 160);

    struct VulkanReflectionProbeCaptureReadback {
        VulkanBufferResource buffer;
        uint64_t radianceBytes = 0;
        uint64_t prefilteredBytes = 0;
    };

    class VulkanReflectionProbeCapturePass final {
    public:
        static constexpr uint32_t MaximumFaceRecords =
            4u * kReflectionProbeCaptureFaceCount;

        void init(VkDevice device, VkPhysicalDevice physicalDevice,
            VulkanResourceAllocator& allocator,
            ::DescriptorAllocator& descriptors,
            VkDescriptorSetLayout materialLayout,
            VkDescriptorSetLayout samplerLayout,
            VkDescriptorSetLayout sceneLayout);
        void cleanup() noexcept;

        [[nodiscard]] VkRenderPass renderPass() const noexcept {
            return renderPass_;
        }
        [[nodiscard]] VkPipelineLayout graphicsLayout() const noexcept {
            return graphicsLayout_;
        }
        [[nodiscard]] VkPipeline pipeline(bool alphaMasked,
            bool doubleSided) const noexcept;
        [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }

        void writeFace(uint32_t frameIndex, uint32_t recordIndex,
            const ReflectionProbeCaptureFace& face, glm::vec3 position,
            float nearPlane, uint32_t activeLightCount, bool captureSky,
            uint32_t resolution);
        void beginFace(VkCommandBuffer commandBuffer,
            const VulkanReflectionProbeCaptureStaging& target,
            uint32_t faceIndex, uint32_t frameIndex, uint32_t recordIndex,
            VkDescriptorSet sceneDescriptor) const;
        void bindFaceDescriptors(VkCommandBuffer commandBuffer,
            uint32_t frameIndex, uint32_t recordIndex,
            VkDescriptorSet sceneDescriptor) const;
        void endFace(VkCommandBuffer commandBuffer) const;

        [[nodiscard]] std::vector<VkDescriptorSet> recordPrefilter(
            VkCommandBuffer commandBuffer,
            const VulkanReflectionProbeCaptureStaging& target,
            uint32_t sampleCount);
        [[nodiscard]] VulkanReflectionProbeCaptureReadback recordReadback(
            VkCommandBuffer commandBuffer,
            const VulkanReflectionProbeCaptureStaging& target);
        void releaseDescriptors(
            std::span<const VkDescriptorSet> descriptors) noexcept;

    private:
        [[nodiscard]] VkShaderModule createShaderModule(
            const char* relativePath) const;
        [[nodiscard]] VkPipeline createGraphicsPipeline(bool sky,
            bool alphaMasked, bool doubleSided) const;
        [[nodiscard]] VkDeviceSize dynamicOffset(
            uint32_t recordIndex) const;

        VkDevice device_ = VK_NULL_HANDLE;
        VulkanResourceAllocator* allocator_ = nullptr;
        ::DescriptorAllocator* descriptors_ = nullptr;
        VkRenderPass renderPass_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout captureLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout graphicsLayout_ = VK_NULL_HANDLE;
        VkPipeline skyPipeline_ = VK_NULL_HANDLE;
        std::array<VkPipeline, 4> pipelines_{};
        VkDescriptorSetLayout filterLayout_ = VK_NULL_HANDLE;
        VkPipelineLayout filterPipelineLayout_ = VK_NULL_HANDLE;
        VkPipeline filterPipeline_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
        VkDeviceSize faceDataStride_ = 0;
        std::array<VulkanBufferResource, 2> faceBuffers_{};
        std::array<VkDescriptorSet, 2> faceDescriptors_{};
    };

} // namespace Iridium
