#pragma once

// The qualification side of the Vulkan backend (M7R R2.7): frame captures,
// Ordinary2 / deep-layered / depth-pyramid capture validation readbacks, the
// reflection-probe capture-target startup validator, and the indirect/VSM
// oracle (VulkanIndirectOracle). Attached through the backend factory; the
// backend reaches it only through IVulkanBackendExtension.

#include "qualification/vulkan/VulkanIndirectOracle.h"
#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <optional>
#include <vector>

namespace Iridium {

    class VulkanQualificationExtension final : public IVulkanBackendExtension,
        public IVulkanLegacyQualificationRequests {
    public:
        VulkanQualificationExtension() = default;
        VulkanQualificationExtension(const VulkanQualificationExtension&) = delete;
        VulkanQualificationExtension& operator=(
            const VulkanQualificationExtension&) = delete;
        ~VulkanQualificationExtension() override = default;

        // IVulkanBackendExtension
        void configure(const RenderBackendConfig& config) override;
        [[nodiscard]] VulkanGraphHooks graphHooks() const noexcept override;
        void onBackendInitialized(const VulkanBackendServices& services) override;
        [[nodiscard]] bool wantsHook(
            const VulkanHookContext& context) const override;
        void onHook(const VulkanHookContext& context) override;
        void onFrameSlotRetired(uint32_t slot) override;
        [[nodiscard]] IVulkanIndirectOracle* indirectOracle() noexcept override {
            return &oracle_;
        }
        [[nodiscard]] IVulkanLegacyQualificationRequests*
            legacyQualificationRequests() noexcept override { return this; }
        void onBeforeDeviceDestroy() override;

        // Deferred capture (the R2.9 harness path): consumed at the next
        // SceneColorComplete or FinalCaptureHook matching the point.
        void armFrameCapture(uint64_t captureId, FrameCapturePoint point);

        // IVulkanLegacyQualificationRequests (removed with the IRenderBackend
        // methods in R2.9).
        void captureCurrentFrame(uint64_t captureId,
            const VulkanFrameRecording& frame,
            const VulkanCaptureHookPayload& source) override;
        [[nodiscard]] std::vector<FrameCapture> collectFrameCaptures(
            bool frameOpen, bool waitForPending) override;
        void requestOrdinary2CaptureValidation(uint64_t validationId,
            const VulkanFrameRecording& frame) override;
        [[nodiscard]] std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool frameOpen,
                bool waitForPending) override;
        void requestDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality,
            const VulkanFrameRecording& frame) override;
        [[nodiscard]] std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool frameOpen,
                bool waitForPending) override;
        void requestDepthPyramidCaptureValidation(uint64_t validationId,
            const VulkanFrameRecording& frame) override;
        [[nodiscard]] std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool frameOpen,
                bool waitForPending) override;

    private:
        struct PendingFrameCapture {
            uint64_t captureId = 0;
            uint32_t frameIndex = 0;
            VkExtent2D extent{};
            VkFormat format = VK_FORMAT_UNDEFINED;
            FrameCapturePoint point = FrameCapturePoint::SceneLinear;
            VulkanBufferResource readback;
        };
        struct PendingOrdinary2CaptureValidation {
            uint64_t validationId = 0;
            uint32_t frameIndex = 0;
            VkExtent2D extent{};
            uint32_t expectedDrawCount = 0;
            uint32_t workItemCount = 0;
            VulkanBufferResource readback;
        };
        struct PendingDeepLayeredCaptureValidation {
            uint64_t validationId = 0;
            uint32_t frameIndex = 0;
            VkExtent2D extent{};
            TransparencyQuality quality = TransparencyQuality::Hero4;
            uint32_t interfaceCount = 0;
            uint32_t expectedDrawCount = 0;
            uint32_t sceneResolveDrawCount = 0;
            uint32_t compatibilityForwardDrawCount = 0;
            uint32_t workItemCount = 0;
            VulkanBufferResource readback;
        };
        struct DeepLayeredCaptureValidationRequest {
            uint64_t validationId = 0;
            TransparencyQuality quality = TransparencyQuality::Hero4;
        };
        struct PendingDepthPyramidCaptureValidation {
            uint64_t validationId = 0;
            uint32_t frameIndex = 0;
            VkExtent2D extent{};
            uint32_t mipCount = 0;
            VulkanBufferResource readback;
        };
        struct ArmedFrameCapture {
            uint64_t captureId = 0;
            FrameCapturePoint point = FrameCapturePoint::SceneLinear;
        };

        void runProbeCaptureTargetValidation();
        void recordFrameCapture(uint64_t captureId, VkCommandBuffer cmd,
            uint32_t slot, const VulkanCaptureHookPayload& source);
        void recordOrdinary2Readback(const VulkanHookContext& context,
            const VulkanOrdinary2HookPayload& payload);
        void recordDeepLayeredReadback(const VulkanHookContext& context,
            const VulkanDeepLayeredHookPayload& payload);
        void recordDepthPyramidReadback(const VulkanHookContext& context,
            const VulkanDepthPyramidHookPayload& payload);
        void recordDeepResolveCounts(const VulkanHookContext& context,
            const VulkanDeepResolveCountsPayload& payload);
        void collectFrameCapturesForSlot(uint32_t frameIndex);
        void collectOrdinary2ValidationsForSlot(uint32_t frameIndex);
        void collectDeepLayeredValidationsForSlot(uint32_t frameIndex);
        void collectDepthPyramidValidationsForSlot(uint32_t frameIndex);
        [[nodiscard]] VulkanBufferResource createReadback(VkDeviceSize bytes);
        [[nodiscard]] bool attached() const noexcept {
            return services_.allocator != nullptr;
        }

        VulkanBackendServices services_{};
        VulkanIndirectOracle oracle_;
        bool validateProbeCaptureTargets_ = false;

        std::optional<ArmedFrameCapture> armedFrameCapture_;
        std::vector<PendingFrameCapture> pendingFrameCaptures_;
        std::vector<FrameCapture> completedFrameCaptures_;
        std::optional<uint64_t> ordinary2Request_;
        std::vector<PendingOrdinary2CaptureValidation> pendingOrdinary2_;
        std::vector<Ordinary2CaptureValidationResult> completedOrdinary2_;
        std::optional<DeepLayeredCaptureValidationRequest> deepLayeredRequest_;
        std::vector<PendingDeepLayeredCaptureValidation> pendingDeepLayered_;
        std::vector<DeepLayeredCaptureValidationResult> completedDeepLayered_;
        std::optional<uint64_t> depthPyramidRequest_;
        std::vector<PendingDepthPyramidCaptureValidation> pendingDepthPyramid_;
        std::vector<DepthPyramidCaptureValidationResult> completedDepthPyramid_;
    };

} // namespace Iridium
