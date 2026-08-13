#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Iridium {

    struct VulkanGraphAccessInfo {
        VkPipelineStageFlags stages = 0;
        VkAccessFlags access = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    [[nodiscard]] VulkanGraphAccessInfo getVulkanGraphAccessInfo(
        RenderGraph::Access access, RenderGraph::ResourceType type);
    [[nodiscard]] VkFormat toVkFormat(RenderGraph::Format format);
    [[nodiscard]] RenderGraph::Format toGraphFormat(VkFormat format);

    struct VulkanGraphBarrierIntent {
        uint32_t passOrderIndex = RenderGraph::InvalidIndex;
        uint32_t logicalResourceIndex = RenderGraph::InvalidIndex;
        uint32_t physicalSlot = RenderGraph::InvalidIndex;
        VulkanGraphAccessInfo before{};
        VulkanGraphAccessInfo after{};
    };

    struct VulkanGraphPhysicalResource {
        RenderGraph::ResourceType type = RenderGraph::ResourceType::Image;
        VulkanImageResource image;
        VulkanBufferResource buffer;

        [[nodiscard]] bool isValid() const noexcept {
            return type == RenderGraph::ResourceType::Image
                ? image.isValid()
                : buffer.isValid();
        }
    };

    class VulkanGraphResourceFactory {
    public:
        virtual ~VulkanGraphResourceFactory() = default;
        [[nodiscard]] virtual VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) = 0;
        virtual void destroy(VulkanGraphPhysicalResource& resource) noexcept = 0;
    };

    class VulkanAllocatorGraphResourceFactory final
        : public VulkanGraphResourceFactory {
    public:
        explicit VulkanAllocatorGraphResourceFactory(
            VulkanResourceAllocator& allocator,
            ProfileMemoryCategory category) noexcept
            : allocator_(&allocator), category_(category) {}

        [[nodiscard]] VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) override;
        void destroy(VulkanGraphPhysicalResource& resource) noexcept override;

    private:
        VulkanResourceAllocator* allocator_ = nullptr;
        ProfileMemoryCategory category_ = ProfileMemoryCategory::RenderGraphTransient;
    };

    class VulkanGraphResourcePool final {
    public:
        VulkanGraphResourcePool() = default;
        VulkanGraphResourcePool(const VulkanGraphResourcePool&) = delete;
        VulkanGraphResourcePool& operator=(const VulkanGraphResourcePool&) = delete;
        ~VulkanGraphResourcePool();

        void init(VulkanGraphResourceFactory& factory, uint32_t frameCount);
        void rebuild(const RenderGraph::CompiledGraph& graph);
        void onFrameFenceCompleted(uint32_t frameIndex);
        void cleanupAfterDeviceIdle() noexcept;

        [[nodiscard]] uint32_t frameCount() const noexcept {
            return static_cast<uint32_t>(active_.size());
        }
        [[nodiscard]] size_t activeResourceCount(uint32_t frameIndex) const;
        [[nodiscard]] size_t retiredResourceCount(uint32_t frameIndex) const;
        [[nodiscard]] uint64_t requestedBytes() const noexcept;
        [[nodiscard]] uint64_t committedBytes() const noexcept;
        [[nodiscard]] const VulkanGraphPhysicalResource& resource(
            uint32_t frameIndex, uint32_t physicalSlot) const;

    private:
        void destroyResources(std::vector<VulkanGraphPhysicalResource>& resources) noexcept;

        VulkanGraphResourceFactory* factory_ = nullptr;
        std::vector<std::vector<VulkanGraphPhysicalResource>> active_;
        std::vector<std::vector<VulkanGraphPhysicalResource>> retired_;
    };

    struct VulkanGraphStats {
        bool enabled = false;
        uint64_t topologyHash = 0;
        uint32_t passCount = 0;
        uint32_t logicalResourceCount = 0;
        uint32_t physicalSlotCount = 0;
        uint32_t barrierCount = 0;
        uint32_t frameCount = 0;
        uint64_t requestedBytes = 0;
        uint64_t committedBytes = 0;
        uint64_t rebuildCount = 0;
        uint64_t cacheMissCount = 0;
    };

    class VulkanRenderGraphExecutor final {
    public:
        VulkanRenderGraphExecutor() = default;
        VulkanRenderGraphExecutor(const VulkanRenderGraphExecutor&) = delete;
        VulkanRenderGraphExecutor& operator=(const VulkanRenderGraphExecutor&) = delete;

        void init(VulkanResourceAllocator& allocator, uint32_t frameCount,
            ProfileMemoryCategory category = ProfileMemoryCategory::RenderGraphTransient);
        void init(VulkanGraphResourceFactory& factory, uint32_t frameCount);
        void rebuild(RenderGraph::CompiledGraph graph);
        void onFrameFenceCompleted(uint32_t frameIndex);
        [[nodiscard]] bool validateFrame(uint32_t frameIndex) noexcept;
        void beginFrameExecution(uint32_t frameIndex);
        void beginPass(VkCommandBuffer commandBuffer, std::string_view passName);
        void skipPass(std::string_view passName);
        void finishFrameExecution();
        void transitionImage(VkCommandBuffer commandBuffer,
            std::string_view logicalName, RenderGraph::Access access);
        void cleanupAfterDeviceIdle() noexcept;

        [[nodiscard]] const std::vector<VulkanGraphBarrierIntent>& barriers() const noexcept {
            return barriers_;
        }
        [[nodiscard]] VulkanGraphStats stats() const noexcept;
        [[nodiscard]] const VulkanImageResource& imageResource(
            uint32_t frameIndex, std::string_view logicalName) const;
        [[nodiscard]] const VulkanBufferResource& bufferResource(
            uint32_t frameIndex, std::string_view logicalName) const;

    private:
        std::optional<VulkanAllocatorGraphResourceFactory> allocatorFactory_;
        VulkanGraphResourcePool resources_;
        RenderGraph::CompiledGraphCache cache_;
        std::vector<VulkanGraphBarrierIntent> barriers_;
        uint64_t topologyHash_ = 0;
        uint32_t passCount_ = 0;
        uint32_t logicalResourceCount_ = 0;
        uint32_t physicalSlotCount_ = 0;
        uint64_t rebuildCount_ = 0;
        uint64_t cacheMissCount_ = 0;
        std::vector<std::vector<RenderGraph::Access>> frameAccess_;
        uint32_t executingFrame_ = RenderGraph::InvalidIndex;
        uint32_t nextPass_ = 0;

        [[nodiscard]] const RenderGraph::CompiledGraph& executingGraph() const;
        void transitionPhysicalResource(VkCommandBuffer commandBuffer,
            uint32_t physicalSlot, RenderGraph::Access access);
    };

} // namespace Iridium
