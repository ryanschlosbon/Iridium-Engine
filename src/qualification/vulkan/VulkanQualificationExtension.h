#pragma once

// The qualification side of the Vulkan backend (M7R R2.7-R2.9): frame
// captures, Ordinary2 / deep-layered / depth-pyramid capture validation
// readbacks, the reflection-probe capture-target startup validator, and the
// indirect/VSM oracle (VulkanIndirectOracle). The qualification harness owns
// it, attaches it through RenderBackendCreateInfo and drives it through
// IQualificationBackend; the backend reaches it only through
// IVulkanBackendExtension.

#include "qualification/QualificationBackend.h"
#include "qualification/vulkan/VulkanIndirectOracle.h"
#include "qualification/vulkan/VulkanIndirectStreamDigest.h"
#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <optional>
#include <vector>

namespace Iridium {

    class VulkanQualificationExtension final : public IVulkanBackendExtension,
        public IQualificationBackend {
    public:
        VulkanQualificationExtension() = default;
        VulkanQualificationExtension(const VulkanQualificationExtension&) = delete;
        VulkanQualificationExtension& operator=(
            const VulkanQualificationExtension&) = delete;
        ~VulkanQualificationExtension() override = default;

        // IVulkanBackendExtension
        [[nodiscard]] VulkanGraphHooks graphHooks() const noexcept override;
        void onBackendInitialized(const VulkanBackendServices& services) override;
        [[nodiscard]] bool wantsHook(
            const VulkanHookContext& context) const override;
        void onHook(const VulkanHookContext& context) override;
        void onFrameSlotRetired(uint32_t slot) override;
        [[nodiscard]] IVulkanIndirectOracle* indirectOracle() noexcept override {
            return &oracle_;
        }
        [[nodiscard]] IVulkanIndirectStreamObserver*
            indirectStreamObserver() noexcept override {
            return indirectStreamDigestEnabled_ ? &indirectStreamDigest_ : nullptr;
        }
        [[nodiscard]] const VulkanIndirectStreamDigest&
            indirectStreamDigest() const noexcept {
            return indirectStreamDigest_;
        }
        void onBeforeDeviceDestroy() override;

        // IQualificationBackend
        [[nodiscard]] IRenderBackendExtension& backendExtension() noexcept override {
            return *this;
        }
        void configureQualification(
            const QualificationBackendConfig& config) override;
        // Consumed at the next SceneColorComplete (scene-linear) or
        // FinalCaptureHook (final output) hook.
        void armFrameCapture(uint64_t captureId,
            FrameCapturePoint point) override;
        // Consumed by the next Ordinary2 / matching deep-tier validation hook
        // with at least one prepared draw.
        void armOrdinary2CaptureValidation(uint64_t validationId) override;
        void armDeepLayeredCaptureValidation(uint64_t validationId,
            TransparencyQuality quality) override;
        // Requires an initialized backend with the depth pyramid enabled.
        void armDepthPyramidCaptureValidation(uint64_t validationId) override;
        [[nodiscard]] std::vector<FrameCapture> collectFrameCaptures(
            bool waitForPending) override;
        [[nodiscard]] std::vector<Ordinary2CaptureValidationResult>
            collectOrdinary2CaptureValidations(bool waitForPending) override;
        [[nodiscard]] std::vector<DeepLayeredCaptureValidationResult>
            collectDeepLayeredCaptureValidations(bool waitForPending) override;
        [[nodiscard]] std::vector<DepthPyramidCaptureValidationResult>
            collectDepthPyramidCaptureValidations(bool waitForPending) override;

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
        // M7R R4b.5 --qualification-alias-poison: at FrameGraphBegin, fills
        // the slot's alias heaps through buffers over each whole heap
        // (recreated when the graph is rebuilt), then orders the fill before
        // every later access.
        void recordAliasPoison(const VulkanHookContext& context);
        void destroyAliasPoisonBuffers() noexcept;
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
        VulkanIndirectStreamDigest indirectStreamDigest_;
        bool indirectStreamDigestEnabled_ = false;
        bool validateProbeCaptureTargets_ = false;
        bool aliasPoison_ = false;
        struct AliasPoisonSlot {
            uint64_t rebuildCount = 0;
            bool valid = false;
            std::vector<VulkanBufferResource> buffers;
        };
        std::vector<AliasPoisonSlot> aliasPoisonSlots_;

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
