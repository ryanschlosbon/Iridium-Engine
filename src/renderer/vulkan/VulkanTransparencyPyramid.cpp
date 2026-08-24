#include "renderer/vulkan/VulkanTransparencyPyramid.h"

#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "utils/File.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    struct TransparencyPyramidPush {
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t sourceMip = 0;
        uint32_t initialize = 0;
    };

    static_assert(sizeof(TransparencyPyramidPush) == 16);

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) +
                " failed with VkResult " +
                std::to_string(static_cast<int>(result)));
    }

} // namespace

void VulkanTransparencyPyramid::init(VkDevice device,
    ::DescriptorAllocator& descriptors, VkDescriptorSetLayout globalLayout) {
    if (device == VK_NULL_HANDLE || globalLayout == VK_NULL_HANDLE)
        throw std::invalid_argument(
            "Transparency pyramid requires a device and global layout");
    if (device_ != VK_NULL_HANDLE)
        throw std::logic_error("Transparency pyramid initialized twice");
    device_ = device;
    descriptors_ = &descriptors;
    try {
        const std::array bindings{
            VkDescriptorSetLayoutBinding{ 0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 2,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 3,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo descriptorInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device_, &descriptorInfo,
            nullptr, &descriptorLayout_),
            "vkCreateDescriptorSetLayout(transparency pyramid)");

        const std::array layouts{ globalLayout, descriptorLayout_ };
        const VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(TransparencyPyramidPush) };
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
        layoutInfo.pSetLayouts = layouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        requireSuccess(vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
            &pipelineLayout_),
            "vkCreatePipelineLayout(transparency pyramid)");

        const std::vector<char> code = readFile(
            std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/transparency_pyramid_comp.spv");
        VkShaderModuleCreateInfo shaderInfo{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader = VK_NULL_HANDLE;
        requireSuccess(vkCreateShaderModule(device_, &shaderInfo, nullptr,
            &shader), "vkCreateShaderModule(transparency pyramid)");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
        const VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, pipelineLayout_, VK_NULL_HANDLE, -1 };
        const VkResult pipelineResult = vkCreateComputePipelines(device_,
            VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, shader, nullptr);
        requireSuccess(pipelineResult,
            "vkCreateComputePipelines(transparency pyramid)");
    }
    catch (...) {
        cleanup();
        throw;
    }
}

void VulkanTransparencyPyramid::rebuild(const VulkanFrameTargets& targets) {
    if (device_ == VK_NULL_HANDLE || descriptors_ == nullptr ||
        descriptorLayout_ == VK_NULL_HANDLE)
        throw std::logic_error("Transparency pyramid is not initialized");
    clearDescriptors();
    if (targets.size() == 0)
        throw std::invalid_argument(
            "Transparency pyramid requires at least one frame target");
    if (targets.get(0).refractionColorPyramid.view == VK_NULL_HANDLE &&
        targets.get(0).refractionDepthPyramid.view == VK_NULL_HANDLE)
        return;
    frameDescriptors_.resize(targets.size());
    try {
        for (size_t frame = 0; frame < targets.size(); ++frame) {
            const VulkanFrameContextTargets& target = targets.get(frame);
            const uint32_t mipLevels = target.refractionColorPyramid.mipLevels;
            if (mipLevels == 0 ||
                target.refractionDepthPyramid.mipLevels != mipLevels ||
                target.refractionColorMipViews.size() != mipLevels ||
                target.refractionDepthMipViews.size() != mipLevels)
                throw std::invalid_argument(
                    "Transparency pyramid mip views are inconsistent");
            auto& sets = frameDescriptors_[frame];
            sets.reserve(mipLevels);
            for (uint32_t mip = 0; mip < mipLevels; ++mip) {
                const VkDescriptorSet set = descriptors_->allocate(
                    descriptorLayout_);
                sets.push_back(set);
                const bool initialize = mip == 0;
                const VkDescriptorImageInfo sourceColor{
                    initialize ? targets.sampler() : targets.pyramidSampler(),
                    initialize ? target.litScene.view :
                        target.refractionColorPyramid.view,
                    initialize ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                        VK_IMAGE_LAYOUT_GENERAL };
                const VkDescriptorImageInfo sourceDepth{
                    initialize ? targets.sampler() :
                        targets.depthPyramidSampler(),
                    initialize ? target.depth.view :
                        target.refractionDepthPyramid.view,
                    initialize ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                        VK_IMAGE_LAYOUT_GENERAL };
                const VkDescriptorImageInfo outputColor{ VK_NULL_HANDLE,
                    target.refractionColorMipViews[mip],
                    VK_IMAGE_LAYOUT_GENERAL };
                const VkDescriptorImageInfo outputDepth{ VK_NULL_HANDLE,
                    target.refractionDepthMipViews[mip],
                    VK_IMAGE_LAYOUT_GENERAL };
                const std::array images{ sourceColor, sourceDepth,
                    outputColor, outputDepth };
                std::array<VkWriteDescriptorSet, 4> writes{};
                for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding] = {
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set,
                        binding, 0, 1,
                        binding < 2
                            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        &images[binding], nullptr, nullptr };
                }
                vkUpdateDescriptorSets(device_,
                    static_cast<uint32_t>(writes.size()), writes.data(),
                    0, nullptr);
            }
        }
    }
    catch (...) {
        clearDescriptors();
        throw;
    }
}

void VulkanTransparencyPyramid::clearDescriptors() noexcept {
    if (descriptors_ != nullptr) {
        for (auto& sets : frameDescriptors_) {
            if (!sets.empty())
                descriptors_->free(std::span<const VkDescriptorSet>(sets));
        }
    }
    frameDescriptors_.clear();
}

void VulkanTransparencyPyramid::cleanup() noexcept {
    clearDescriptors();
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
    descriptors_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

uint32_t VulkanTransparencyPyramid::record(VkCommandBuffer commandBuffer,
    uint32_t frameIndex, VkDescriptorSet globalSet,
    const VulkanFrameTargets& targets) const {
    if (commandBuffer == VK_NULL_HANDLE || globalSet == VK_NULL_HANDLE ||
        pipeline_ == VK_NULL_HANDLE || frameIndex >= frameDescriptors_.size())
        throw std::invalid_argument(
            "Transparency pyramid record request is invalid");
    const VulkanFrameContextTargets& target = targets.get(frameIndex);
    const auto& sets = frameDescriptors_[frameIndex];
    if (sets.size() != target.refractionColorPyramid.mipLevels)
        throw std::logic_error(
            "Transparency pyramid descriptors do not match targets");

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_);
    for (uint32_t mip = 0; mip < sets.size(); ++mip) {
        const std::array descriptors{ globalSet, sets[mip] };
        vkCmdBindDescriptorSets(commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0,
            static_cast<uint32_t>(descriptors.size()), descriptors.data(),
            0, nullptr);
        const TransparencyPyramidPush push{
            (std::max)(target.refractionColorPyramid.extent.width >> mip, 1u),
            (std::max)(target.refractionColorPyramid.extent.height >> mip, 1u),
            mip == 0 ? 0u : mip - 1u,
            mip == 0 ? 1u : 0u,
        };
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer, (push.outputWidth + 7u) / 8u,
            (push.outputHeight + 7u) / 8u, 1u);
        if (mip + 1u < sets.size()) {
            std::array<VkImageMemoryBarrier, 2> barriers{};
            for (VkImageMemoryBarrier& barrier : barriers) {
                barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1 };
            }
            barriers[0].image = target.refractionColorPyramid.image;
            barriers[1].image = target.refractionDepthPyramid.image;
            vkCmdPipelineBarrier(commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                0, nullptr, 0, nullptr,
                static_cast<uint32_t>(barriers.size()), barriers.data());
        }
    }
    return static_cast<uint32_t>(sets.size());
}

} // namespace Iridium
