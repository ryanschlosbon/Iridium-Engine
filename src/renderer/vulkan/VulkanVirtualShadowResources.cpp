#include "VulkanVirtualShadowResources.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace Iridium {
    void VulkanVirtualShadowResources::init(
        VkDevice device, VulkanResourceAllocator& allocator,
        const VkPhysicalDeviceLimits& limits,
        const VirtualShadowResourceConfig& config,
        uint32_t workingSetCount) {
        if (allocator_ != nullptr)
            throw std::logic_error(
                "Virtual shadow resources were initialized more than once");
        if (const std::string error =
                validateVirtualShadowResourceConfig(config); !error.empty())
            throw std::invalid_argument(error);
        if (workingSetCount == 0u || workingSetCount > 8u)
            throw std::invalid_argument(
                "Virtual shadow working-set count must be in [1, 8]");

        const auto atlas = buildVirtualShadowAtlasLayout(config.pageSizeTexels,
            config.borderTexels, config.physicalPageCapacity, limits.maxImageDimension2D);

        const VirtualShadowGpuWorkingSetLayout workingSetLayout =
            buildVirtualShadowGpuWorkingSetLayout(config,
                limits.minStorageBufferOffsetAlignment);

        allocator_ = &allocator;
        device_ = device;
        try {
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.minFilter = sampler.magFilter = VK_FILTER_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (vkCreateSampler(device, &sampler, nullptr, &depthSampler_) != VK_SUCCESS)
                throw std::runtime_error("Cannot create virtual-shadow depth sampler");
            physicalPool_ = allocator.createImage2D(
                { atlas.widthTexels, atlas.heightTexels },
                VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT,
                ProfileMemoryCategory::VirtualShadowPhysicalPool);
            pageTable_ = allocator.createBuffer(
                VkDeviceSize{ config.pageTableEntryCapacity } * sizeof(uint32_t),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::VirtualShadowPageTable);
            workingSets_.reserve(workingSetCount);
            for (uint32_t index = 0; index < workingSetCount; ++index) {
                workingSets_.push_back(allocator.createBuffer(
                    workingSetLayout.totalBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                    ProfileMemoryCategory::VirtualShadowWorkingSet));
            }
            readbacks_.reserve(workingSetCount);
            for (uint32_t i = 0; i < workingSetCount; ++i)
                readbacks_.push_back(allocator.createBuffer(
                    workingSetLayout.outputRequests.size + workingSetLayout.telemetry.size,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::CaptureReadback));
            std::vector<VkBuffer> buffers;
            for (const auto& workingSet : workingSets_)
                buffers.push_back(workingSet.buffer);
            markingPass_.init(device, buffers, workingSetLayout, config,
                std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders");
            depthReceiverPass_.init(device, buffers, workingSetLayout,
                std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders");
            fullViewPass_.init(device, buffers, workingSetLayout,
                std::filesystem::path(PROJECT_ROOT_DIR) / "assets/shaders");
        }
        catch (...) {
            cleanup();
            throw;
        }

        info_ = {
            .atlasLayout = atlas,
            .physicalAtlasExtent = physicalPool_.extent,
            .physicalPageCapacity = config.physicalPageCapacity,
            .pageTableEntryCapacity = config.pageTableEntryCapacity,
            .tileFootprintTexels = atlas.tileFootprintTexels,
            .requestedPhysicalPoolBytes = uint64_t{ physicalPool_.extent.width } *
                physicalPool_.extent.height * sizeof(float),
            .requestedPageTableBytes = uint64_t{ config.pageTableEntryCapacity } *
                sizeof(uint32_t),
            .workingSetLayout = workingSetLayout,
            .workingSetCount = workingSetCount,
        };
        info_.requestedWorkingSetBytes = info_.workingSetLayout.totalBytes *
            workingSetCount;
        info_.requestCapacity = config.compactedRequestCapacity;
        info_.requestedReadbackBytes = (workingSetLayout.outputRequests.size + workingSetLayout.telemetry.size) * workingSetCount;
    }

    void VulkanVirtualShadowResources::cleanup() noexcept {
        if (depthSampler_) vkDestroySampler(device_, depthSampler_, nullptr);
        depthSampler_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
        fullViewPass_.cleanup();
        depthReceiverPass_.cleanup();
        markingPass_.cleanup();
        if (allocator_ != nullptr) {
            for (auto& buffer : readbacks_) allocator_->destroy(buffer);
            readbacks_.clear();
            for (VulkanBufferResource& workingSet : workingSets_)
                allocator_->destroy(workingSet);
            workingSets_.clear();
            allocator_->destroy(pageTable_);
            allocator_->destroy(physicalPool_);
        }
        allocator_ = nullptr;
        info_ = {};
    }

} // namespace Iridium
