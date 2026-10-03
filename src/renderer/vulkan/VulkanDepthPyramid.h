#pragma once
#include "renderer/rhi/DepthPyramid.h"
#include "renderer/rhi/GpuSceneIndirect.h"
#include "VulkanResourceAllocator.h"
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
class DescriptorAllocator;
namespace Iridium {
class VulkanFrameTargets;
// Two persistent view-owned images are both the build destinations and the
// histories sampled by the next submission for that view. A submission samples
// the preceding contents before overwriting the same image later on the ordered
// graphics queue. Rebuild/cleanup require the caller's existing all-frame
// retirement boundary.
class VulkanDepthPyramid final {
public:
    static constexpr uint32_t HistoryViewCount = 2;

    using PublishedHistory = DepthPyramidHistoryPublication;

    void init(VkDevice device, ::DescriptorAllocator& descriptors,
        VulkanResourceAllocator& resources,
        VkDescriptorSetLayout globalLayout,
        VkDescriptorSetLayout gpuSceneLayout);
    void rebuild(const VulkanFrameTargets& targets);
    void clearDescriptors() noexcept;
    void cleanup() noexcept;
    uint32_t record(VkCommandBuffer command, uint32_t frame,
        uint32_t view, const DepthPyramidHistoryOwner& owner,
        uint64_t submissionSerial);
    uint32_t recordQueries(VkCommandBuffer command, uint32_t frame,
        uint32_t view, VkBuffer queries, VkDeviceSize queryBytes,
        VkBuffer results, VkDeviceSize resultBytes,
        uint32_t queryCount);
    uint32_t recordGpuSceneQueries(VkCommandBuffer command, uint32_t frame,
        uint32_t view, VkDescriptorSet globalSet,
        VkDescriptorSet gpuSceneSet, VkBuffer candidates,
        VkDeviceSize candidateBytes, VkBuffer results,
        VkDeviceSize resultBytes, uint32_t candidateCount,
        uint32_t transformCount, uint32_t instanceCount,
        uint32_t primitiveCount, uint32_t geometryCount);
    void recordHistoryReadback(VkCommandBuffer command, uint32_t view,
        VkBuffer destination, VkDeviceSize destinationOffset);
    void onFrameFenceCompleted(uint32_t frame,
        uint64_t completedSubmissionSerial) noexcept;
    [[nodiscard]] const PublishedHistory& publishedHistory(
        uint32_t view) const;
    [[nodiscard]] const PublishedHistory& queuedHistory(
        uint32_t view) const;
    [[nodiscard]] VkImageView historyImageView(uint32_t view) const;
    // The retained view's history pyramid (graph import
    // "depth.occlusion-pyramid.history", executor-owned: M7R R3b.9).
    [[nodiscard]] const VulkanImageResource& historyImage(uint32_t view) const {
        return historyImages_.at(view);
    }
    [[nodiscard]] VkSampler historySampler() const noexcept { return sampler_; }
private:
    VkDevice device_{};
    ::DescriptorAllocator* descriptorAllocator_{};
    VulkanResourceAllocator* resourceAllocator_{};
    VkDescriptorSetLayout descriptorLayout_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    VkDescriptorSetLayout queryDescriptorLayout_{};
    VkPipelineLayout queryLayout_{};
    VkPipeline queryPipeline_{};
    VkDescriptorSetLayout gpuSceneQueryDescriptorLayout_{};
    VkPipelineLayout gpuSceneQueryLayout_{};
    VkPipeline gpuSceneQueryPipeline_{};
    VkSampler sampler_{};
    struct Frame {
        VkExtent2D extent{};
        struct View {
            std::vector<VkDescriptorSet> sets;
        };
        std::array<View, HistoryViewCount> views;
    };
    std::vector<Frame> frames_;
    std::vector<VkDescriptorSet> querySets_;
    std::vector<VkDescriptorSet> gpuSceneQuerySets_;
    std::array<VulkanImageResource, HistoryViewCount> historyImages_{};
    std::array<std::vector<VkImageView>, HistoryViewCount> historyMipViews_{};
    DepthPyramidHistoryPublicationTracker publicationTracker_;
    uint64_t imageGeneration_ = 0;
};
}
