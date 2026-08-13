#include "VulkanMeshLayouts.h"

#include "renderer/rhi/Mesh.h"

#include <array>
#include <stdexcept>

namespace Iridium {

    void VulkanMeshLayouts::init(VkDevice device,
        VkDescriptorSetLayout lightingSetLayout,
        VkDescriptorSetLayout indexedMaterialSetLayout,
        VkDescriptorSetLayout indexedSamplerSetLayout) {
        if (device_ != VK_NULL_HANDLE) {
            throw std::logic_error("VulkanMeshLayouts is already initialized");
        }
        if (device == VK_NULL_HANDLE || lightingSetLayout == VK_NULL_HANDLE) {
            throw std::invalid_argument("VulkanMeshLayouts requires valid Vulkan layouts and device");
        }

        device_ = device;

        try {
            VkDescriptorSetLayoutBinding globalBinding{};
            globalBinding.binding = 0;
            globalBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            globalBinding.descriptorCount = 1;
            globalBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo globalLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            globalLayoutInfo.bindingCount = 1;
            globalLayoutInfo.pBindings = &globalBinding;

            if (vkCreateDescriptorSetLayout(device_, &globalLayoutInfo, nullptr, &globalSetLayout_) != VK_SUCCESS) {
                throw std::runtime_error("failed to create global descriptor set layout");
            }

            if (indexedMaterialSetLayout == VK_NULL_HANDLE ||
                indexedSamplerSetLayout == VK_NULL_HANDLE) {
                throw std::invalid_argument(
                    "Indexed material and sampler layouts are required");
            }
            materialSetLayout_ = indexedMaterialSetLayout;
            samplerSetLayout_ = indexedSamplerSetLayout;

            VkPushConstantRange meshPushConstants{};
            meshPushConstants.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            meshPushConstants.offset = 0;
            meshPushConstants.size = sizeof(CanonicalMeshPushConstants);

            std::array<VkDescriptorSetLayout, 3> gBufferSetLayouts{
                globalSetLayout_, materialSetLayout_, samplerSetLayout_ };
            VkPipelineLayoutCreateInfo gBufferLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            gBufferLayoutInfo.setLayoutCount = 3u;
            gBufferLayoutInfo.pSetLayouts = gBufferSetLayouts.data();
            gBufferLayoutInfo.pushConstantRangeCount = 1;
            gBufferLayoutInfo.pPushConstantRanges = &meshPushConstants;

            if (vkCreatePipelineLayout(device_, &gBufferLayoutInfo, nullptr, &gBufferPipelineLayout_) != VK_SUCCESS) {
                throw std::runtime_error("failed to create G-buffer pipeline layout");
            }

            std::array<VkDescriptorSetLayout, 4> forwardSetLayouts{
                globalSetLayout_, materialSetLayout_,
                samplerSetLayout_, lightingSetLayout };
            VkPipelineLayoutCreateInfo forwardLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            forwardLayoutInfo.setLayoutCount = 4u;
            forwardLayoutInfo.pSetLayouts = forwardSetLayouts.data();
            forwardLayoutInfo.pushConstantRangeCount = 1;
            forwardLayoutInfo.pPushConstantRanges = &meshPushConstants;

            if (vkCreatePipelineLayout(device_, &forwardLayoutInfo, nullptr, &forwardPipelineLayout_) != VK_SUCCESS) {
                throw std::runtime_error("failed to create forward pipeline layout");
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    void VulkanMeshLayouts::cleanup() noexcept {
        if (device_ == VK_NULL_HANDLE) {
            return;
        }

        if (forwardPipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, forwardPipelineLayout_, nullptr);
            forwardPipelineLayout_ = VK_NULL_HANDLE;
        }
        if (gBufferPipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, gBufferPipelineLayout_, nullptr);
            gBufferPipelineLayout_ = VK_NULL_HANDLE;
        }
        materialSetLayout_ = VK_NULL_HANDLE;
        samplerSetLayout_ = VK_NULL_HANDLE;
        if (globalSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, globalSetLayout_, nullptr);
            globalSetLayout_ = VK_NULL_HANDLE;
        }

        device_ = VK_NULL_HANDLE;
    }

} // namespace Iridium
