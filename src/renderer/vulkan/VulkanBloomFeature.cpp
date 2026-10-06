#include "renderer/vulkan/VulkanBloomFeature.h"

#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VkContext.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanFrameTelemetry.h"
#include "renderer/vulkan/VulkanProductionGraphIds.h"
#include "utils/File.h"

#include <glm/glm.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Iridium {
namespace {

    struct BloomPushConstants {
        glm::vec4 destination{ 0.0f };   // xy size, zw 1 / size
        glm::vec4 source{ 0.0f };        // xy 1 / size of the read level, z its lod
        glm::vec4 filter{ 0.0f };        // x threshold, y knee, z output scale
        glm::uvec4 control{ 0u };        // x mode
    };
    static_assert(sizeof(BloomPushConstants) == 64);

    // bloom.comp modes.
    constexpr uint32_t ModePrefilter = 0;    // scene -> level 0 (Karis, threshold)
    constexpr uint32_t ModeDownsample = 1;   // level i-1 -> level i
    constexpr uint32_t ModeUpsample = 2;     // level i += tent(level i+1)
    constexpr uint32_t GroupSize = 8;

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) +
                " failed with VkResult " + std::to_string(static_cast<int>(result)));
    }

    VkExtent2D levelExtent(VkExtent2D base, uint32_t level) noexcept {
        return { (std::max)(base.width >> level, 1u), (std::max)(base.height >> level, 1u) };
    }

    glm::vec4 sizeAndInverse(VkExtent2D extent) noexcept {
        const float width = static_cast<float>(extent.width);
        const float height = static_cast<float>(extent.height);
        return { width, height, 1.0f / width, 1.0f / height };
    }

} // namespace

void VulkanBloomFeature::create(const VulkanFeatureContext& context) {
    context_ = &context;
}

void VulkanBloomFeature::createPipeline() {
    const VkDevice device = context_->device;
    try {
        const std::array bindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &descriptorLayout_),
            "vkCreateDescriptorSetLayout(bloom)");

        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(BloomPushConstants) };
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        requireSuccess(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(bloom)");

        const std::vector<char> code = readFile(std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/bloom_comp.spv");
        VkShaderModuleCreateInfo shaderInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader = VK_NULL_HANDLE;
        requireSuccess(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader),
            "vkCreateShaderModule(bloom)");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
        const VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, pipelineLayout_, VK_NULL_HANDLE, -1 };
        const VkResult result = vkCreateComputePipelines(device, context_->pipelineCache, 1,
            &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(device, shader, nullptr);
        requireSuccess(result, "vkCreateComputePipelines(bloom)");

        // Bilinear within a level; the level is chosen explicitly (lod).
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        requireSuccess(vkCreateSampler(device, &sampler, nullptr, &sampler_),
            "vkCreateSampler(bloom)");
        for (auto& frame : sets_)
            for (VkDescriptorSet& set : frame) set = context_->descriptors.allocate(descriptorLayout_);
    }
    catch (...) {
        destroyPipeline();
        throw;
    }
}

void VulkanBloomFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
    // A rebuild follows device idle; views of the previous plan are unused.
    releaseViews();
    bloomPass_ = ids.bloom;
    sceneColor_ = ids.resolvedSceneColor;
    // Karis Auto follows the plan: off when TAA resolves the scene colour.
    temporalResolve_ = ids.taaResolve.isValid();
    chain_ = ids.bloomChain;
    levels_ = 0;
    if (!bloomPass_.isValid()) return;
    if (pipeline_ == VK_NULL_HANDLE) createPipeline();

    const VkDevice device = context_->device;
    for (uint32_t frame = 0; frame < VulkanFrameScheduler::FramesInFlight; ++frame) {
        const VulkanImageResource& chain = context_->graph.image(frame, chain_);
        if (chain.mipLevels == 0 || chain.mipLevels > BloomMaximumLevels)
            throw std::logic_error("The bloom chain's level count is invalid");
        levels_ = chain.mipLevels;
        std::array<VkImageView, BloomMaximumLevels>& views = levelViews_[frame];
        for (uint32_t level = 0; level < levels_; ++level) {
            VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            info.image = chain.image;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = chain.format;
            info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1 };
            requireSuccess(vkCreateImageView(device, &info, nullptr, &views[level]),
                "vkCreateImageView(bloom level)");
        }
        // Set 0's scene source (binding 0) is written per frame.
        const VkDescriptorImageInfo chainSource{ sampler_, chain.view, VK_IMAGE_LAYOUT_GENERAL };
        std::array<VkDescriptorImageInfo, BloomMaximumLevels + 1> targets{};
        std::array<VkWriteDescriptorSet, 2 * (BloomMaximumLevels + 1)> writes{};
        uint32_t count = 0;
        for (uint32_t index = 0; index <= levels_; ++index) {
            const VkDescriptorSet set = sets_[frame][index];
            targets[index] = { VK_NULL_HANDLE, views[index == levels_ ? 0 : index],
                VK_IMAGE_LAYOUT_GENERAL };
            writes[count++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targets[index], nullptr, nullptr };
            if (index != 0)
                writes[count++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &chainSource, nullptr, nullptr };
        }
        vkUpdateDescriptorSets(device, count, writes.data(), 0, nullptr);
    }
}

void VulkanBloomFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
    if (!bloomPass_.isValid()) return;
    graph.registerPass(bloomPass_, { this, nullptr, &execute, "gpu.post.bloom",
        GpuRangePlacement::AfterBarriers, false });
}

void VulkanBloomFeature::onGraphReleased() {
    releaseViews();
    bloomPass_ = {};
    sceneColor_ = chain_ = {};
    levels_ = 0;
}

void VulkanBloomFeature::releaseViews() noexcept {
    if (context_ == nullptr) return;
    for (auto& frame : levelViews_) {
        for (VkImageView& view : frame) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(context_->device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
}

void VulkanBloomFeature::execute(void* owner, VulkanPassContext& context) {
    auto& self = *static_cast<VulkanBloomFeature*>(owner);
    const uint32_t frame = context.frame.frameIndex;
    VulkanRenderGraphExecutor& graph = context.graph;
    const auto& sets = self.sets_[frame];
    // The resolved colour may be a TAA history slot (parity, view set); this
    // slot's set is not in flight.
    const VkDescriptorImageInfo scene{ self.sampler_, graph.image(frame, self.sceneColor_).view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[0],
        0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &scene, nullptr, nullptr };
    vkUpdateDescriptorSets(self.context_->device, 1, &write, 0, nullptr);

    const VkExtent2D sceneExtent = self.context_->frameTargets.extent();
    const VkExtent2D chainExtent = graph.image(frame, self.chain_).extent;
    const uint32_t levels = self.levels_;
    const BloomSettings& settings = self.settings_;
    const VkCommandBuffer cmd = context.commandBuffer;
    // Each level is read by the next dispatch (and level i is rewritten by
    // its upsample): a compute write -> read/write dependency between them.
    VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    const uint32_t dispatches = 2u * levels - 1u;
    uint32_t recorded = 0;
    const auto dispatch = [&](VkDescriptorSet set, uint32_t level, VkExtent2D source,
        float sourceLod, uint32_t mode, float scale) {
        const VkExtent2D destination = levelExtent(chainExtent, level);
        BloomPushConstants push{};
        push.destination = sizeAndInverse(destination);
        const glm::vec4 read = sizeAndInverse(source);
        push.source = { read.z, read.w, sourceLod, 0.0f };
        push.filter = { settings.threshold, settings.knee, scale, 0.0f };
        const bool karis = settings.karis == BloomKarisMode::On ||
            (settings.karis == BloomKarisMode::Auto && !self.temporalResolve_);
        push.control = { mode, karis ? 1u : 0u, 0u, 0u };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.pipelineLayout_,
            0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, self.pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(push), &push);
        vkCmdDispatch(cmd, (destination.width + GroupSize - 1) / GroupSize,
            (destination.height + GroupSize - 1) / GroupSize, 1);
        if (++recorded < dispatches)
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.pipeline_);
    dispatch(sets[0], 0, sceneExtent, 0.0f, ModePrefilter, 1.0f);
    for (uint32_t level = 1; level < levels; ++level)
        dispatch(sets[level], level, levelExtent(chainExtent, level - 1),
            static_cast<float>(level - 1), ModeDownsample, 1.0f);
    // Level 0 ends as the mean of the levels' blurs.
    for (uint32_t level = levels - 1; level-- > 0;)
        dispatch(level == 0 ? sets[levels] : sets[level], level,
            levelExtent(chainExtent, level + 1), static_cast<float>(level + 1), ModeUpsample,
            level == 0 ? 1.0f / static_cast<float>(levels) : 1.0f);
    VulkanFrameTelemetry& telemetry = self.context_->telemetry;
    if (telemetry.collecting()) telemetry.counters().dispatchRecorded += recorded;
}

void VulkanBloomFeature::destroyPipeline() noexcept {
    if (context_ == nullptr) return;
    const VkDevice device = context_->device;
    for (auto& frame : sets_) {
        for (VkDescriptorSet& set : frame) {
            if (set != VK_NULL_HANDLE) context_->descriptors.free(std::span(&set, 1));
            set = VK_NULL_HANDLE;
        }
    }
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, descriptorLayout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device, sampler_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

void VulkanBloomFeature::destroy() noexcept {
    releaseViews();
    destroyPipeline();
    bloomPass_ = {};
    levels_ = 0;
    context_ = nullptr;
}

} // namespace Iridium
