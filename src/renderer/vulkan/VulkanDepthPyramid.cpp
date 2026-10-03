#include "VulkanDepthPyramid.h"
#include "DescriptorAllocator.h"
#include "VulkanFrameTargets.h"
#include "utils/File.h"
#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
namespace Iridium {
namespace {
void require(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Depth pyramid Vulkan failure: " + std::to_string(result));
}
}
void VulkanDepthPyramid::init(VkDevice device, ::DescriptorAllocator& descriptors,
    VulkanResourceAllocator& resources, VkDescriptorSetLayout globalLayout,
    VkDescriptorSetLayout gpuSceneLayout) {
    if (!device || device_ || !globalLayout || !gpuSceneLayout)
        throw std::logic_error("Invalid depth pyramid initialization");
    device_ = device;
    descriptorAllocator_ = &descriptors;
    resourceAllocator_ = &resources;
    try {
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = 2; set.pBindings = bindings.data();
        require(vkCreateDescriptorSetLayout(device_, &set, nullptr, &descriptorLayout_));
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &descriptorLayout_;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        require(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_));
        const auto code = readFile(std::string(PROJECT_ROOT_DIR) + "assets/shaders/depth_pyramid_comp.spv");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = code.size(); module.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader{}; require(vkCreateShaderModule(device_, &module, nullptr, &shader));
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        compute.layout = layout_;
        const auto result = vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline_);
        vkDestroyShaderModule(device_,shader,nullptr); require(result);

        const std::array queryBindings{
            VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = static_cast<uint32_t>(queryBindings.size());
        set.pBindings = queryBindings.data();
        require(vkCreateDescriptorSetLayout(device_, &set, nullptr,
            &queryDescriptorLayout_));
        const VkPushConstantRange queryPush{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
        layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &queryDescriptorLayout_;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &queryPush;
        require(vkCreatePipelineLayout(device_, &layout, nullptr, &queryLayout_));
        const auto queryCode = readFile(std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/depth_pyramid_query_comp.spv");
        module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = queryCode.size();
        module.pCode = reinterpret_cast<const uint32_t*>(queryCode.data());
        shader = VK_NULL_HANDLE;
        require(vkCreateShaderModule(device_, &module, nullptr, &shader));
        compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        compute.layout = queryLayout_;
        const auto queryResult = vkCreateComputePipelines(device_,
            VK_NULL_HANDLE,1,&compute,nullptr,&queryPipeline_);
        vkDestroyShaderModule(device_,shader,nullptr); require(queryResult);

        const std::array gpuSceneQueryBindings{
            VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            VkDescriptorSetLayoutBinding{2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set.bindingCount = static_cast<uint32_t>(gpuSceneQueryBindings.size());
        set.pBindings = gpuSceneQueryBindings.data();
        require(vkCreateDescriptorSetLayout(device_, &set, nullptr,
            &gpuSceneQueryDescriptorLayout_));
        const std::array<VkDescriptorSetLayout,3> gpuSceneQueryLayouts{
            globalLayout, gpuSceneLayout, gpuSceneQueryDescriptorLayout_};
        const VkPushConstantRange gpuSceneQueryPush{
            VK_SHADER_STAGE_COMPUTE_BIT,0,8u * sizeof(uint32_t)};
        layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = static_cast<uint32_t>(
            gpuSceneQueryLayouts.size());
        layout.pSetLayouts = gpuSceneQueryLayouts.data();
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &gpuSceneQueryPush;
        require(vkCreatePipelineLayout(device_, &layout, nullptr,
            &gpuSceneQueryLayout_));
        const auto gpuSceneQueryCode = readFile(
            std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/depth_pyramid_gpu_scene_query_comp.spv");
        module = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = gpuSceneQueryCode.size();
        module.pCode = reinterpret_cast<const uint32_t*>(
            gpuSceneQueryCode.data());
        shader = VK_NULL_HANDLE;
        require(vkCreateShaderModule(device_, &module, nullptr, &shader));
        compute = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        compute.layout = gpuSceneQueryLayout_;
        const auto gpuSceneQueryResult = vkCreateComputePipelines(device_,
            VK_NULL_HANDLE,1,&compute,nullptr,&gpuSceneQueryPipeline_);
        vkDestroyShaderModule(device_,shader,nullptr);
        require(gpuSceneQueryResult);
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.minFilter = sampler.magFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        require(vkCreateSampler(device_, &sampler, nullptr, &sampler_));
    } catch (...) { cleanup(); throw; }
}
void VulkanDepthPyramid::rebuild(const VulkanFrameTargets& targets) {
    if (!device_ || !resourceAllocator_)
        throw std::logic_error("Depth pyramid not initialized");
    clearDescriptors();
    try {
        const VkExtent2D extent = targets.extent();
        if (!extent.width || !extent.height || extent.width > 65535 ||
            extent.height > 65535) {
            throw std::invalid_argument(
                "Depth pyramid extent outside safe integer reduction range");
        }
        const uint32_t mipCount = depthPyramidMipCount(
            {extent.width, extent.height});
        for (uint32_t viewIndex = 0; viewIndex < HistoryViewCount;
            ++viewIndex) {
            VulkanImageResource& history = historyImages_[viewIndex];
            history = resourceAllocator_->createImage2D(extent,
                VK_FORMAT_R32_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                ProfileMemoryCategory::RenderGraphHistory, mipCount);
            std::vector<VkImageView>& mipViews = historyMipViews_[viewIndex];
            mipViews.resize(mipCount, VK_NULL_HANDLE);
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                VkImageViewCreateInfo view{
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = history.image;
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = VK_FORMAT_R32_SFLOAT;
                view.subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,1};
                require(vkCreateImageView(device_, &view, nullptr,
                    &mipViews[mip]));
            }
        }
        frames_.resize(targets.size());
        querySets_.reserve(targets.size());
        gpuSceneQuerySets_.reserve(targets.size());
        for (uint32_t frame = 0; frame < targets.size(); ++frame)
            querySets_.push_back(descriptorAllocator_->allocate(
                queryDescriptorLayout_));
        for (uint32_t frame = 0; frame < targets.size(); ++frame)
            gpuSceneQuerySets_.push_back(descriptorAllocator_->allocate(
                gpuSceneQueryDescriptorLayout_));
        for (uint32_t frame = 0; frame < frames_.size(); ++frame) {
            Frame& output = frames_[frame];
            output.extent = extent;
            for (uint32_t viewIndex = 0; viewIndex < HistoryViewCount;
                ++viewIndex) {
                Frame::View& outputView = output.views[viewIndex];
                outputView.sets.reserve(mipCount);
                const std::vector<VkImageView>& mipViews =
                    historyMipViews_[viewIndex];
                for (uint32_t mip = 0; mip < mipCount; ++mip) {
                    const auto set = descriptorAllocator_->allocate(
                        descriptorLayout_);
                    outputView.sets.push_back(set);
                    const std::array<VkDescriptorImageInfo,2> images{{
                        {sampler_,mip ? mipViews[mip-1] :
                            targets.get(frame).depth.view,
                            mip ? VK_IMAGE_LAYOUT_GENERAL :
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                        {VK_NULL_HANDLE,mipViews[mip],VK_IMAGE_LAYOUT_GENERAL}}};
                    std::array<VkWriteDescriptorSet,2> writes{};
                    for (uint32_t binding = 0; binding < writes.size();
                        ++binding) {
                        writes[binding] = {
                            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        writes[binding].dstSet = set;
                        writes[binding].dstBinding = binding;
                        writes[binding].descriptorCount = 1;
                        writes[binding].descriptorType = binding
                            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        writes[binding].pImageInfo = &images[binding];
                    }
                    vkUpdateDescriptorSets(device_,
                        static_cast<uint32_t>(writes.size()), writes.data(),
                        0, nullptr);
                }
            }
        }
        if (++imageGeneration_ == 0u) ++imageGeneration_;
        publicationTracker_.reset(HistoryViewCount,
            static_cast<uint32_t>(frames_.size()), imageGeneration_);
    } catch (...) { clearDescriptors(); throw; }
}
void VulkanDepthPyramid::clearDescriptors() noexcept {
    for (auto set : gpuSceneQuerySets_) descriptorAllocator_->free(set);
    gpuSceneQuerySets_.clear();
    for (auto set : querySets_) descriptorAllocator_->free(set);
    querySets_.clear();
    for (auto& frame : frames_) {
        for (auto& view : frame.views) {
            for (auto set : view.sets) descriptorAllocator_->free(set);
        }
    }
    frames_.clear();
    if (resourceAllocator_) {
        for (uint32_t viewIndex = 0; viewIndex < HistoryViewCount;
            ++viewIndex) {
            for (VkImageView view : historyMipViews_[viewIndex]) {
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device_, view, nullptr);
            }
            historyMipViews_[viewIndex].clear();
            resourceAllocator_->destroy(historyImages_[viewIndex]);
        }
    }
    publicationTracker_.clear();
}
void VulkanDepthPyramid::cleanup() noexcept {
    clearDescriptors();
    if (device_) {
        vkDestroySampler(device_,sampler_,nullptr); vkDestroyPipeline(device_,pipeline_,nullptr);
        vkDestroyPipeline(device_,queryPipeline_,nullptr);
        vkDestroyPipeline(device_,gpuSceneQueryPipeline_,nullptr);
        vkDestroyPipelineLayout(device_,layout_,nullptr); vkDestroyDescriptorSetLayout(device_,descriptorLayout_,nullptr);
        vkDestroyPipelineLayout(device_,queryLayout_,nullptr);
        vkDestroyDescriptorSetLayout(device_,queryDescriptorLayout_,nullptr);
        vkDestroyPipelineLayout(device_,gpuSceneQueryLayout_,nullptr);
        vkDestroyDescriptorSetLayout(device_,gpuSceneQueryDescriptorLayout_,nullptr);
    }
    device_={}; descriptorAllocator_=nullptr; resourceAllocator_=nullptr;
    sampler_={}; pipeline_={}; layout_={}; descriptorLayout_={};
    queryPipeline_={}; queryLayout_={}; queryDescriptorLayout_={};
    gpuSceneQueryPipeline_={}; gpuSceneQueryLayout_={};
    gpuSceneQueryDescriptorLayout_={};
}
uint32_t VulkanDepthPyramid::record(VkCommandBuffer command, uint32_t frame,
    uint32_t view, const DepthPyramidHistoryOwner& owner,
    uint64_t submissionSerial) {
    if (!command || frame >= frames_.size() ||
        view >= HistoryViewCount || submissionSerial == 0u ||
        !historyImages_[view].isValid()) {
        throw std::invalid_argument("Invalid depth pyramid frame");
    }
    const auto& output = frames_[frame];
    const auto& outputView = output.views[view];
    const VulkanImageResource& image = historyImages_[view];
    // M7R R3b.9: the history is an executor-owned graph import. The build
    // pass's begin barrier (to GENERAL) and the next reader's barrier (to
    // SHADER_READ_ONLY or TRANSFER_SRC) are the executor's; only the
    // mip-to-mip ordering stays here.
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    const uint32_t forwardDepth = 0;
    vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&forwardDepth);
    for (uint32_t mip=0; mip<outputView.sets.size(); ++mip) {
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&outputView.sets[mip],0,nullptr);
        const uint32_t width = (std::max)(output.extent.width >> mip,1u);
        const uint32_t height = (std::max)(output.extent.height >> mip,1u);
        vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    publicationTracker_.schedule(frame, view, owner,
        {output.extent.width, output.extent.height},
        DeviceDepthConvention::ForwardZeroToOne, submissionSerial);
    return static_cast<uint32_t>(outputView.sets.size());
}

uint32_t VulkanDepthPyramid::recordQueries(VkCommandBuffer command,
    uint32_t frame, uint32_t view, VkBuffer queries,
    VkDeviceSize queryBytes, VkBuffer results, VkDeviceSize resultBytes,
    uint32_t queryCount) {
    if (!command || frame >= querySets_.size() ||
        view >= historyImages_.size() || !queries || !results ||
        queryBytes < static_cast<VkDeviceSize>(queryCount) *
            sizeof(DepthPyramidDeviceQuery) ||
        resultBytes < static_cast<VkDeviceSize>(queryCount) *
            sizeof(DepthPyramidDeviceResult)) {
        throw std::invalid_argument("Invalid depth-pyramid query dispatch");
    }
    if (queryCount == 0u) return 0u;
    const VkDescriptorImageInfo image{sampler_, historyImages_[view].view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkDescriptorBufferInfo,2> buffers{{
        {queries,0,queryBytes},{results,0,resultBytes}}};
    std::array<VkWriteDescriptorSet,3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = querySets_[frame]; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image;
    for (uint32_t binding = 1; binding < writes.size(); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = querySets_[frame];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &buffers[binding - 1u];
    }
    vkUpdateDescriptorSets(device_,static_cast<uint32_t>(writes.size()),
        writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,queryPipeline_);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,queryLayout_,
        0,1,&querySets_[frame],0,nullptr);
    const std::array<uint32_t,3> parameters{
        queryCount,0u,DepthPyramidAbiVersion};
    vkCmdPushConstants(command,queryLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,
        sizeof(parameters),parameters.data());
    vkCmdDispatch(command,(queryCount+63u)/64u,1u,1u);
    return 1u;
}

uint32_t VulkanDepthPyramid::recordGpuSceneQueries(VkCommandBuffer command,
    uint32_t frame, uint32_t view, VkDescriptorSet globalSet,
    VkDescriptorSet gpuSceneSet, VkBuffer candidates,
    VkDeviceSize candidateBytes, VkBuffer results,
    VkDeviceSize resultBytes, uint32_t candidateCount,
    uint32_t transformCount, uint32_t instanceCount,
    uint32_t primitiveCount, uint32_t geometryCount) {
    if (!command || frame >= gpuSceneQuerySets_.size() ||
        view >= historyImages_.size() || !globalSet || !gpuSceneSet ||
        !candidates || !results || candidateCount == 0u ||
        candidateBytes < static_cast<VkDeviceSize>(candidateCount) *
            sizeof(GpuSceneIndirectCandidate) ||
        resultBytes < static_cast<VkDeviceSize>(candidateCount) *
            sizeof(DepthPyramidDeviceResult)) {
        throw std::invalid_argument(
            "Invalid GPU-scene depth-pyramid query dispatch");
    }
    const VkDescriptorBufferInfo candidateInfo{
        candidates,0,candidateBytes};
    const VkDescriptorBufferInfo resultInfo{results,0,resultBytes};
    const VkDescriptorImageInfo image{sampler_, historyImages_[view].view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet,3> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = gpuSceneQuerySets_[frame];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &candidateInfo;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = gpuSceneQuerySets_[frame];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &resultInfo;
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = gpuSceneQuerySets_[frame];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &image;
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
        writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,
        gpuSceneQueryPipeline_);
    const std::array<VkDescriptorSet,3> sets{
        globalSet,gpuSceneSet,gpuSceneQuerySets_[frame]};
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,
        gpuSceneQueryLayout_,0,static_cast<uint32_t>(sets.size()),
        sets.data(),0,nullptr);
    const std::array<uint32_t,8> parameters{
        candidateCount,transformCount,instanceCount,primitiveCount,
        geometryCount,0u,DepthPyramidAbiVersion,0u};
    vkCmdPushConstants(command,gpuSceneQueryLayout_,
        VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),parameters.data());
    vkCmdDispatch(command,(candidateCount+63u)/64u,1u,1u);
    return 1u;
}

void VulkanDepthPyramid::recordHistoryReadback(VkCommandBuffer command,
    uint32_t view, VkBuffer destination, VkDeviceSize destinationOffset) {
    if (!command || !destination || view >= historyImages_.size() ||
        !historyImages_[view].isValid()) {
        throw std::invalid_argument("Invalid depth-pyramid history readback");
    }
    // R3b.9: recorded inside depth.occlusion-pyramid.validation-readback-hook,
    // whose declared TransferSource read puts the history in TRANSFER_SRC.
    const VulkanImageResource& history = historyImages_[view];
    for (uint32_t mip = 0; mip < history.mipLevels; ++mip) {
        const DepthPyramidExtent extent = depthPyramidMipExtent(
            {history.extent.width, history.extent.height}, mip);
        VkBufferImageCopy copy{};
        copy.bufferOffset = destinationOffset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(command, history.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &copy);
        destinationOffset += static_cast<VkDeviceSize>(extent.width) *
            extent.height * sizeof(float);
    }
}

void VulkanDepthPyramid::onFrameFenceCompleted(uint32_t frame,
    uint64_t completedSubmissionSerial) noexcept {
    publicationTracker_.complete(frame, completedSubmissionSerial);
}

const VulkanDepthPyramid::PublishedHistory&
VulkanDepthPyramid::publishedHistory(uint32_t view) const {
    return publicationTracker_.published(view);
}

const VulkanDepthPyramid::PublishedHistory&
VulkanDepthPyramid::queuedHistory(uint32_t view) const {
    return publicationTracker_.queued(view);
}

VkImageView VulkanDepthPyramid::historyImageView(uint32_t view) const {
    if (view >= historyImages_.size() || !historyImages_[view].isValid())
        return VK_NULL_HANDLE;
    return historyImages_[view].view;
}
}
