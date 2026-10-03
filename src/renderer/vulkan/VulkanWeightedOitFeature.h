#pragma once

// M7R R3c.3: explicit WeightedOIT as a feature owner. Owns the accumulation
// and resolve passes and pipelines, the per-slot instance streams and their
// capacity, and the deterministic order seed; registers the callbacks of
// "transparent.oit.accumulate" and "transparent.oit.resolve". Since R3c.9 it
// also owns the WeightedOIT residency (the topology that declares the passes,
// switched by the backend's transparency topology change) and the per-frame
// packet/instance-capacity decision.

#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/RenderDebugView.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"

#include "VulkanFeatureContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanRenderGraphExecutor.h"
#include "VulkanResourceAllocator.h"
#include "VulkanWeightedOitPass.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>

namespace Iridium {

    class VulkanWeightedOitFeature final : public IVulkanFeature {
    public:
        static constexpr uint32_t FrameCount = VulkanFrameScheduler::FramesInFlight;

        // Per-frame inputs (submitForwardQueues), valid until the drain.
        struct FrameInputs {
            std::span<const DrawPacket> sortedSurfaceQueue{};
            std::span<const glm::mat4> instanceTransforms{};
            // WeightedOIT is resident, every instance fits, and the queue
            // holds at least one WeightedOIT packet.
            bool execute = false;
            VkDescriptorSet globalSet = VK_NULL_HANDLE;
            VkDescriptorSet sceneSet = VK_NULL_HANDLE;
            RenderDebugView debugView = RenderDebugView::Final;
        };

        // The frame's WeightedOIT demand (observe).
        struct FrameDecision {
            uint64_t packetCount = 0;
            // Resident and every instance fits the instance stream.
            bool executionEnabled = false;
        };

        VulkanWeightedOitFeature() = default;
        VulkanWeightedOitFeature(const VulkanWeightedOitFeature&) = delete;
        VulkanWeightedOitFeature& operator=(const VulkanWeightedOitFeature&) = delete;

        void configure(uint64_t orderSeed) noexcept { orderSeed_ = orderSeed; }
        [[nodiscard]] uint64_t orderSeed() const noexcept { return orderSeed_; }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Descriptors after the frame targets rebuild.
        void rebuildDescriptors();
        void clearDescriptors() noexcept { pass_.clearDescriptors(); }

        // Instance-stream capacity (topology changes only).
        void setInstanceCapacity(uint32_t capacity);
        [[nodiscard]] uint32_t instanceCapacity() const noexcept {
            return instanceCapacity_;
        }

        // Residency (topology changes publish or restore it).
        [[nodiscard]] TransparencyPyramidResidency& residency() noexcept { return residency_; }
        [[nodiscard]] const TransparencyPyramidResidency& residency() const noexcept {
            return residency_;
        }
        // Per frame (submitForwardQueues): validates the sorted queue's
        // WeightedOIT packets, observes the residency demand and records the
        // fallback counters.
        [[nodiscard]] FrameDecision observe(std::span<const DrawPacket> sortedSurfaceQueue,
            std::span<const glm::mat4> instanceTransforms);

        // Drain point: prepares the slot's instance stream when executing,
        // then runs accumulate and resolve (both skipped otherwise).
        void record(const FrameInputs& inputs);

    private:
        void prepareInstances(uint32_t frameIndex);
        void recordAccumulation(VulkanPassContext& context);
        void recordResolve(VulkanPassContext& context);
        static bool active(void* owner, const VulkanFrameRecordContext& frame);
        static void executeAccumulation(void* owner, VulkanPassContext& context);
        static void executeResolve(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        VulkanWeightedOitPass pass_;
        std::array<VulkanBufferResource, FrameCount> instanceBuffers_{};
        uint32_t instanceCapacity_ = 0u;
        uint64_t orderSeed_ = 0;
        TransparencyPyramidResidency residency_;
        RenderGraph::PassId accumulatePass_{};
        RenderGraph::PassId resolvePass_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        FrameInputs staged_{};
        uint32_t preparedInstanceCount_ = 0u;
    };

} // namespace Iridium
