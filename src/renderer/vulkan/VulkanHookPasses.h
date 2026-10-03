#pragma once

// M7R R3c.4: the extension hook passes (R2.7) as registered callbacks. Owns
// the runPassHook semantics of the validation readback hooks (the pass is
// skipped unless an attached extension wants the hook; otherwise its GPU
// range wraps the barriers and the extension work), and the two capture
// hooks: "scene-color-capture-hook" (scene-linear captures) and
// "final-capture-hook" (final-output captures and the editor bridge's
// retained views, which consume the pass through a VulkanFinalCaptureConsumer;
// R3c.10).

#include "VulkanBackendExtension.h"
#include "VulkanFeatureContext.h"
#include "VulkanRenderGraphExecutor.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Iridium {

    // Records the retained editor views from the final output: `prepare`
    // initializes newly created view images, `copy` copies the output.
    struct VulkanFinalCaptureConsumer {
        void* owner = nullptr;
        void (*prepare)(void* owner, VkCommandBuffer commandBuffer) = nullptr;
        void (*copy)(void* owner, VkCommandBuffer commandBuffer) = nullptr;
    };

    class VulkanHookPasses final : public IVulkanFeature {
    public:
        // The validation readback hooks, in graph order.
        enum class PassHook : uint8_t {
            DepthPyramidValidation,
            Ordinary2Validation,
            Hero4Validation,
            Cinematic8Validation,
            Count,
        };

        VulkanHookPasses() = default;
        VulkanHookPasses(const VulkanHookPasses&) = delete;
        VulkanHookPasses& operator=(const VulkanHookPasses&) = delete;

        void setFinalCaptureConsumer(const VulkanFinalCaptureConsumer& consumer) noexcept {
            finalConsumer_ = consumer;
        }

        // IVulkanFeature
        void create(const VulkanFeatureContext& context) override;
        void onGraphRebuilt(const VulkanProductionGraphIds& ids) override;
        void registerPasses(VulkanRenderGraphExecutor& graph) override;
        void destroy() noexcept override;

        // Drain points. A hook whose pass the topology does not declare does
        // nothing (an undeclared scene-color capture that is wanted throws).
        void runPassHook(PassHook hook, const VulkanHookContext& context);
        void runSceneColorCapture(VkCommandBuffer commandBuffer,
            const VulkanCaptureHookPayload& source);
        void runFinalCapture(VkCommandBuffer commandBuffer,
            const VulkanCaptureHookPayload& source, bool retainedViews);

    private:
        struct PassHookSlot {
            VulkanHookPasses* self = nullptr;
            RenderGraph::PassId pass{};
            const char* gpuRange = nullptr;
            VulkanHookContext staged{};
        };

        static bool passHookActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executePassHook(void* owner, VulkanPassContext& context);
        static bool sceneCaptureActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeSceneCapture(void* owner, VulkanPassContext& context);
        static bool finalCaptureActive(void* owner, const VulkanFrameRecordContext& frame);
        static void executeFinalCapture(void* owner, VulkanPassContext& context);

        const VulkanFeatureContext* context_ = nullptr;
        std::array<PassHookSlot, static_cast<size_t>(PassHook::Count)> passHooks_{};
        RenderGraph::PassId sceneCapturePass_{};
        RenderGraph::PassId finalCapturePass_{};
        VulkanFinalCaptureConsumer finalConsumer_{};

        // Staged for the frame's callbacks (see VulkanFeatureContext.h).
        VulkanHookContext stagedScene_{};
        VulkanCaptureHookPayload stagedSceneSource_{};
        VulkanHookContext stagedFinal_{};
        VulkanCaptureHookPayload stagedFinalSource_{};
        bool stagedFinalWanted_ = false;
        bool stagedRetainedViews_ = false;
    };

} // namespace Iridium
