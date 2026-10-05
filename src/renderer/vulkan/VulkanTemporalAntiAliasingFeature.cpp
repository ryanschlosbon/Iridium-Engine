#include "renderer/vulkan/VulkanTemporalAntiAliasingFeature.h"

#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanFrameTelemetry.h"
#include "renderer/vulkan/VulkanMeshLayouts.h"
#include "renderer/vulkan/VulkanProductionGraphIds.h"
#include "utils/File.h"

#include <glm/glm.hpp>

#include <stdexcept>
#include <string>

namespace Iridium {
namespace {

    struct TaaPushConstants {
        glm::vec4 extent{ 0.0f };     // xy size, zw 1 / size
        glm::vec4 exposure{ 0.0f };   // x current, y history, z history valid, w flags
        glm::vec4 feedback{ 0.0f };   // x min history weight, y max, z motion px, w gamma
        glm::vec4 tuning{ 0.0f };     // x reconstruction sharpness
    };
    static_assert(sizeof(TaaPushConstants) == 64);

    constexpr uint32_t TaaGroupSize = 8;

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) +
                " failed with VkResult " + std::to_string(static_cast<int>(result)));
    }

    VkSampler createSampler(VkDevice device, VkFilter filter) {
        VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter = filter;
        info.minFilter = filter;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        requireSuccess(vkCreateSampler(device, &info, nullptr, &sampler),
            "vkCreateSampler(temporal AA)");
        return sampler;
    }

} // namespace

void VulkanTemporalAntiAliasingFeature::create(const VulkanFeatureContext& context) {
    context_ = &context;
    const VkDevice device = context.device;
    try {
        const std::array bindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        requireSuccess(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &descriptorLayout_),
            "vkCreateDescriptorSetLayout(temporal AA)");

        const std::array layouts{ context.meshLayouts.getGlobalSetLayout(), descriptorLayout_ };
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TaaPushConstants) };
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
        layoutInfo.pSetLayouts = layouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        requireSuccess(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout(temporal AA)");

        const std::vector<char> code = readFile(std::string(PROJECT_ROOT_DIR) +
            "assets/shaders/taa_resolve_comp.spv");
        VkShaderModuleCreateInfo shaderInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader = VK_NULL_HANDLE;
        requireSuccess(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader),
            "vkCreateShaderModule(temporal AA)");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
        const VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, pipelineLayout_, VK_NULL_HANDLE, -1 };
        const VkResult result = vkCreateComputePipelines(device, context.pipelineCache,
            1, &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(device, shader, nullptr);
        requireSuccess(result, "vkCreateComputePipelines(temporal AA)");

        linearSampler_ = createSampler(device, VK_FILTER_LINEAR);
        pointSampler_ = createSampler(device, VK_FILTER_NEAREST);
        for (VkDescriptorSet& set : sets_) set = context.descriptors.allocate(descriptorLayout_);
    }
    catch (...) {
        destroy();
        throw;
    }
}

void VulkanTemporalAntiAliasingFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
    resolvePass_ = ids.taaResolve;
    historyPrevious_ = ids.taaHistoryPrevious;
    historyCurrent_ = ids.taaHistoryCurrent;
    sceneColor_ = ids.sceneColor;
    depth_ = ids.depth;
    velocity_ = ids.gbufferVelocity;
    lastHistoryValid_ = false;
}

void VulkanTemporalAntiAliasingFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
    if (!resolvePass_.isValid()) return;
    graph.registerPass(resolvePass_, { this, nullptr, &executeResolve,
        "gpu.temporal.taa", GpuRangePlacement::AfterBarriers, false });
}

void VulkanTemporalAntiAliasingFeature::onGraphReleased() {
    resolvePass_ = {};
}

void VulkanTemporalAntiAliasingFeature::writeDescriptors(uint32_t frameIndex,
    VkImageView previous, VkImageView current) const {
    const VulkanRenderGraphExecutor& graph = context_->graph;
    const VkDescriptorImageInfo images[5]{
        { pointSampler_, graph.image(frameIndex, sceneColor_).view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { pointSampler_, graph.image(frameIndex, depth_).view,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
        { pointSampler_, graph.image(frameIndex, velocity_).view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { linearSampler_, previous, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { VK_NULL_HANDLE, current, VK_IMAGE_LAYOUT_GENERAL },
    };
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            sets_[frameIndex], binding, 0, 1,
            binding < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &images[binding], nullptr, nullptr };
    }
    vkUpdateDescriptorSets(context_->device, static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr);
}

void VulkanTemporalAntiAliasingFeature::executeResolve(void* owner, VulkanPassContext& context) {
    auto& self = *static_cast<VulkanTemporalAntiAliasingFeature*>(owner);
    const uint32_t frame = context.frame.frameIndex;
    VulkanRenderGraphExecutor& graph = context.graph;
    // This frame slot's set is not in flight (its fence retired); the history
    // views are this frame's parity in the view's history set.
    self.writeDescriptors(frame, graph.image(frame, self.historyPrevious_).view,
        graph.image(frame, self.historyCurrent_).view);
    const bool historyValid = graph.historyValid(self.historyPrevious_);
    self.lastHistoryValid_ = historyValid;

    const VkExtent2D extent = self.context_->frameTargets.extent();
    TaaPushConstants push{};
    push.extent = { static_cast<float>(extent.width), static_cast<float>(extent.height),
        1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height) };
    const TemporalUpscaleInputs& request = self.staged_.request;
    // A reset (cut, teleport) also discards history the graph still holds
    // as valid for this view set (the key invalidates on the next turn).
    const bool useHistory = historyValid && !request.resetHistory;
    push.exposure = { request.exposure, request.previousExposure,
        useHistory ? 1.0f : 0.0f, 0.0f };
    const TemporalAntiAliasingTuning& settings = request.nativeTaa;
    push.feedback = { settings.minimumHistoryWeight, settings.maximumHistoryWeight,
        settings.motionPixelsForMinimum, settings.varianceGamma };
    push.tuning = { settings.reconstructionSharpness, settings.staticVarianceGamma,
        settings.stillHistoryWeight, 0.0f };

    const VkCommandBuffer cmd = context.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.pipeline_);
    const std::array sets{ self.staged_.globalSet, self.sets_[frame] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.pipelineLayout_,
        0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, self.pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(push), &push);
    vkCmdDispatch(cmd, (extent.width + TaaGroupSize - 1) / TaaGroupSize,
        (extent.height + TaaGroupSize - 1) / TaaGroupSize, 1);
    VulkanFrameTelemetry& telemetry = self.context_->telemetry;
    if (telemetry.collecting()) ++telemetry.counters().dispatchRecorded;
}

void VulkanTemporalAntiAliasingFeature::destroy() noexcept {
    if (context_ != nullptr) {
        const VkDevice device = context_->device;
        for (VkDescriptorSet& set : sets_) {
            if (set != VK_NULL_HANDLE) context_->descriptors.free(std::span(&set, 1));
            set = VK_NULL_HANDLE;
        }
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline_, nullptr);
        if (pipelineLayout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
        if (descriptorLayout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, descriptorLayout_, nullptr);
        if (linearSampler_ != VK_NULL_HANDLE) vkDestroySampler(device, linearSampler_, nullptr);
        if (pointSampler_ != VK_NULL_HANDLE) vkDestroySampler(device, pointSampler_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
    linearSampler_ = pointSampler_ = VK_NULL_HANDLE;
    resolvePass_ = {};
    context_ = nullptr;
}

} // namespace Iridium
