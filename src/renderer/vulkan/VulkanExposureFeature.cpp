#include "renderer/vulkan/VulkanExposureFeature.h"

#include "renderer/lighting/LightingReference.h"
#include "renderer/vulkan/DescriptorAllocator.h"
#include "renderer/vulkan/VkContext.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanFrameTelemetry.h"
#include "renderer/vulkan/VulkanProductionGraphIds.h"
#include "utils/File.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Iridium {
namespace {

    struct HistogramPushConstants {
        glm::vec4 extent{ 0.0f };   // xy size, zw 1 / size
        glm::vec4 range{ 0.0f };    // x min log2 luminance, y bins per log2 unit, z centre weight
    };
    static_assert(sizeof(HistogramPushConstants) == 32);

    struct AdaptPushConstants {
        glm::vec4 range{ 0.0f };        // x min log2 luminance, y log2 per bin, z EV100 offset
        glm::vec4 limits{ 0.0f };       // x low, y high percentile, z min, w max EV100
        glm::vec4 adaptation{ 0.0f };   // x up, y down EV/s, z delta seconds, w compensation EV
        glm::uvec4 control{ 0u };       // x rows, y previous valid
    };
    static_assert(sizeof(AdaptPushConstants) == 64);

    constexpr uint32_t AdaptGroupSize = 1024;

    // EV100 of a scene-linear luminance L: log2(L / PhotometricToSceneScale *
    // 100 / K), K = 12.5 (reflected-light meter calibration).
    const float Ev100Offset = static_cast<float>(
        std::log2(1.0 / LightingReference::PhotometricToSceneScale) + std::log2(100.0 / 12.5));
    // The histogram is configured in EV100; the shaders work in log2 luminance.
    float log2LuminanceOfEv100(float ev100) noexcept { return ev100 - Ev100Offset; }

    void requireSuccess(VkResult result, const char* operation) {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string(operation) +
                " failed with VkResult " + std::to_string(static_cast<int>(result)));
    }

    VkPipeline createComputePipeline(VkDevice device, VkPipelineCache cache,
        VkPipelineLayout layout, const char* spirv) {
        const std::vector<char> code = readFile(std::string(PROJECT_ROOT_DIR) + spirv);
        VkShaderModuleCreateInfo shaderInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shaderInfo.codeSize = code.size();
        shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader = VK_NULL_HANDLE;
        requireSuccess(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader),
            "vkCreateShaderModule(exposure)");
        const VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr };
        const VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateComputePipelines(device, cache, 1, &pipelineInfo,
            nullptr, &pipeline);
        vkDestroyShaderModule(device, shader, nullptr);
        requireSuccess(result, "vkCreateComputePipelines(exposure)");
        return pipeline;
    }

    VkDescriptorSetLayout createSetLayout(VkDevice device,
        std::span<const VkDescriptorType> types) {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (uint32_t index = 0; index < types.size(); ++index)
            bindings[index] = { index, types[index], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = static_cast<uint32_t>(types.size());
        info.pBindings = bindings.data();
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        requireSuccess(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout),
            "vkCreateDescriptorSetLayout(exposure)");
        return layout;
    }

    VkPipelineLayout createPipelineLayout(VkDevice device, VkDescriptorSetLayout set,
        uint32_t pushBytes) {
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes };
        VkPipelineLayoutCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.setLayoutCount = 1;
        info.pSetLayouts = &set;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        requireSuccess(vkCreatePipelineLayout(device, &info, nullptr, &layout),
            "vkCreatePipelineLayout(exposure)");
        return layout;
    }

    VkDescriptorBufferInfo wholeBuffer(VkBuffer buffer) noexcept {
        return { buffer, 0, VK_WHOLE_SIZE };
    }

} // namespace

void VulkanExposureFeature::create(const VulkanFeatureContext& context) {
    context_ = &context;
    try {
        // The fallback state (multiplier 1) the output and TAA sets bind when
        // no adapted state exists; their shaders never read it in Manual mode.
        fallback_ = context.allocator.createBuffer(ExposureStateBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            true, ProfileMemoryCategory::OtherUnclassified);
        const float neutral[4]{ 0.0f, 1.0f, 0.0f, 0.0f };
        context.allocator.write(fallback_, 0, std::as_bytes(std::span(neutral)));
        if (mode_ == ExposureMode::Auto) createPipelines();
    }
    catch (...) {
        destroy();
        throw;
    }
}

void VulkanExposureFeature::createPipelines() {
    const VkDevice device = context_->device;
    const VkPhysicalDeviceLimits& limits = context_->vk.getPhysicalDeviceProperties().limits;
    if (limits.maxComputeWorkGroupInvocations < AdaptGroupSize ||
        limits.maxComputeWorkGroupSize[0] < AdaptGroupSize)
        throw std::runtime_error("Auto-exposure needs 1024-invocation compute workgroups");
    constexpr std::array histogramTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    constexpr std::array adaptTypes{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    histogramLayout_ = createSetLayout(device, histogramTypes);
    adaptLayout_ = createSetLayout(device, adaptTypes);
    histogramPipelineLayout_ = createPipelineLayout(device, histogramLayout_,
        sizeof(HistogramPushConstants));
    adaptPipelineLayout_ = createPipelineLayout(device, adaptLayout_,
        sizeof(AdaptPushConstants));
    histogramPipeline_ = createComputePipeline(device, context_->pipelineCache,
        histogramPipelineLayout_, "assets/shaders/exposure_histogram_comp.spv");
    adaptPipeline_ = createComputePipeline(device, context_->pipelineCache,
        adaptPipelineLayout_, "assets/shaders/exposure_adapt_comp.spv");

    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    requireSuccess(vkCreateSampler(device, &sampler, nullptr, &pointSampler_),
        "vkCreateSampler(exposure)");
    for (VkDescriptorSet& set : histogramSets_) set = context_->descriptors.allocate(histogramLayout_);
    for (VkDescriptorSet& set : adaptSets_) set = context_->descriptors.allocate(adaptLayout_);
}

void VulkanExposureFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
    histogramPass_ = ids.exposureHistogram;
    adaptPass_ = ids.exposureAdapt;
    sceneColor_ = ids.resolvedSceneColor;
    rows_ = ids.exposureHistogramRows;
    metering_ = ids.exposureMetering;
    previous_ = ids.exposurePrevious;
    current_ = ids.exposureCurrent;
    if (adaptPass_.isValid() && adaptPipeline_ == VK_NULL_HANDLE)
        throw std::logic_error("The graph declares auto-exposure but the mode is Manual");
}

void VulkanExposureFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
    if (!adaptPass_.isValid()) return;
    graph.registerPass(histogramPass_, { this, nullptr, &executeHistogram,
        "gpu.post.exposure.histogram", GpuRangePlacement::AfterBarriers, false });
    graph.registerPass(adaptPass_, { this, nullptr, &executeAdapt,
        "gpu.post.exposure.adapt", GpuRangePlacement::AfterBarriers, false });
}

void VulkanExposureFeature::onGraphReleased() {
    histogramPass_ = adaptPass_ = {};
    sceneColor_ = rows_ = metering_ = previous_ = current_ = {};
}

VulkanExposureFeature::PreviousState VulkanExposureFeature::previousState(
    uint32_t frameIndex) const {
    if (!previous_.isValid()) return { fallback_.buffer, false };
    const VulkanRenderGraphExecutor& graph = context_->graph;
    return { graph.buffer(frameIndex, previous_).buffer, graph.historyValid(previous_) };
}

void VulkanExposureFeature::executeHistogram(void* owner, VulkanPassContext& context) {
    auto& self = *static_cast<VulkanExposureFeature*>(owner);
    const uint32_t frame = context.frame.frameIndex;
    VulkanRenderGraphExecutor& graph = context.graph;
    // This slot's set is not in flight; the resolved colour may be a TAA
    // history slot (parity, view set), so it is rebound every frame.
    const VkDescriptorImageInfo scene{ self.pointSampler_,
        graph.image(frame, self.sceneColor_).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorBufferInfo rows = wholeBuffer(graph.buffer(frame, self.rows_).buffer);
    const std::array writes{
        VkWriteDescriptorSet{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            self.histogramSets_[frame], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &scene, nullptr, nullptr },
        VkWriteDescriptorSet{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            self.histogramSets_[frame], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &rows, nullptr },
    };
    vkUpdateDescriptorSets(self.context_->device, static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr);

    const VkExtent2D extent = self.context_->frameTargets.extent();
    const AutoExposureSettings& settings = self.settings_;
    const float minimum = log2LuminanceOfEv100(settings.histogramMinEv100);
    const float span = settings.histogramMaxEv100 - settings.histogramMinEv100;
    HistogramPushConstants push{};
    push.extent = { static_cast<float>(extent.width), static_cast<float>(extent.height),
        1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height) };
    push.range = { minimum, static_cast<float>(ExposureHistogramBins) / span,
        settings.centreWeight, 0.0f };

    const VkCommandBuffer cmd = context.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.histogramPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        self.histogramPipelineLayout_, 0, 1, &self.histogramSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, self.histogramPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd, (extent.width + ExposureHistogramTile - 1) / ExposureHistogramTile,
        (extent.height + ExposureHistogramTile - 1) / ExposureHistogramTile, 1);
    VulkanFrameTelemetry& telemetry = self.context_->telemetry;
    if (telemetry.collecting()) ++telemetry.counters().dispatchRecorded;
}

void VulkanExposureFeature::executeAdapt(void* owner, VulkanPassContext& context) {
    auto& self = *static_cast<VulkanExposureFeature*>(owner);
    const uint32_t frame = context.frame.frameIndex;
    VulkanRenderGraphExecutor& graph = context.graph;
    // The History halves are this frame's parity in the view's set.
    const std::array buffers{
        wholeBuffer(graph.buffer(frame, self.rows_).buffer),
        wholeBuffer(graph.buffer(frame, self.previous_).buffer),
        wholeBuffer(graph.buffer(frame, self.current_).buffer),
        wholeBuffer(graph.buffer(frame, self.metering_).buffer),
    };
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding)
        writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            self.adaptSets_[frame], binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            nullptr, &buffers[binding], nullptr };
    vkUpdateDescriptorSets(self.context_->device, static_cast<uint32_t>(writes.size()),
        writes.data(), 0, nullptr);

    const VkExtent2D extent = self.context_->frameTargets.extent();
    const AutoExposureSettings& settings = self.settings_;
    AdaptPushConstants push{};
    push.range = { log2LuminanceOfEv100(settings.histogramMinEv100),
        (settings.histogramMaxEv100 - settings.histogramMinEv100) /
            static_cast<float>(ExposureHistogramBins), Ev100Offset, 0.0f };
    push.limits = { settings.lowPercentile, settings.highPercentile,
        settings.minimumEv100, settings.maximumEv100 };
    const float seconds = std::isfinite(self.staged_.deltaSeconds)
        ? self.staged_.deltaSeconds : 0.0f;
    push.adaptation = { settings.speedUpEvPerSecond, settings.speedDownEvPerSecond,
        seconds, self.staged_.compensationEv };
    push.control = { exposureHistogramRowCount(extent.width, extent.height),
        graph.historyValid(self.previous_) ? 1u : 0u, 0u, 0u };

    const VkCommandBuffer cmd = context.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self.adaptPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        self.adaptPipelineLayout_, 0, 1, &self.adaptSets_[frame], 0, nullptr);
    vkCmdPushConstants(cmd, self.adaptPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(push), &push);
    vkCmdDispatch(cmd, 1, 1, 1);
    VulkanFrameTelemetry& telemetry = self.context_->telemetry;
    if (telemetry.collecting()) ++telemetry.counters().dispatchRecorded;
}

void VulkanExposureFeature::destroy() noexcept {
    if (context_ != nullptr) {
        const VkDevice device = context_->device;
        for (auto* sets : { &histogramSets_, &adaptSets_ }) {
            for (VkDescriptorSet& set : *sets) {
                if (set != VK_NULL_HANDLE) context_->descriptors.free(std::span(&set, 1));
                set = VK_NULL_HANDLE;
            }
        }
        for (VkPipeline pipeline : { histogramPipeline_, adaptPipeline_ })
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        for (VkPipelineLayout layout : { histogramPipelineLayout_, adaptPipelineLayout_ })
            if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
        for (VkDescriptorSetLayout layout : { histogramLayout_, adaptLayout_ })
            if (layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, layout, nullptr);
        if (pointSampler_ != VK_NULL_HANDLE) vkDestroySampler(device, pointSampler_, nullptr);
        if (fallback_.buffer != VK_NULL_HANDLE) context_->allocator.destroy(fallback_);
    }
    histogramPipeline_ = adaptPipeline_ = VK_NULL_HANDLE;
    histogramPipelineLayout_ = adaptPipelineLayout_ = VK_NULL_HANDLE;
    histogramLayout_ = adaptLayout_ = VK_NULL_HANDLE;
    pointSampler_ = VK_NULL_HANDLE;
    fallback_ = {};
    histogramPass_ = adaptPass_ = {};
    context_ = nullptr;
}

} // namespace Iridium
