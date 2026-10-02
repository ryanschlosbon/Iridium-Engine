#pragma once

// Qualification oracles for the Vulkan backend's GPU-driven work (M7R R2.8):
// shadow/probe indirect command regions (visibility + LOD), main-view GPU LOD,
// depth-occlusion CPU-projected queries, and the VSM full-depth request oracle.
// The backend emits expectations at its recording sites and calls the verify
// functions when a frame slot retires; this class owns the expected state and
// every comparison.

#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"

#include <array>
#include <optional>
#include <vector>

namespace Iridium {

    struct VulkanIndirectOracleConfig {
        bool shadowIndirect = false;
        bool gpuLod = false;
        bool probeLod = false;
        bool depthOcclusion = false;
        bool virtualShadowDepth = false;

        [[nodiscard]] static VulkanIndirectOracleConfig fromBackendConfig(
            const RenderBackendConfig& config) noexcept {
            return { config.enableShadowIndirectQualificationOracle,
                config.enableGpuLodQualificationOracle,
                config.enableProbeLodQualificationOracle,
                config.enableDepthOcclusionQualificationOracle,
                config.virtualShadowDepthQualificationOracle };
        }
    };

    class VulkanIndirectOracle final : public IVulkanIndirectOracle {
    public:
        static constexpr uint32_t FramesInFlight =
            VulkanFrameScheduler::FramesInFlight;

        void configure(const VulkanIndirectOracleConfig& config) noexcept {
            config_ = config;
        }
        [[nodiscard]] const VulkanIndirectOracleConfig& config() const noexcept {
            return config_;
        }
        // Device services for the VSM depth snapshots (allocation, profiler).
        void attach(VulkanResourceAllocator* allocator,
            CpuProfiler* profiler) noexcept {
            allocator_ = allocator;
            profiler_ = profiler;
        }
        void destroyDeviceResources() noexcept;

        [[nodiscard]] bool enabled(
            VulkanIndirectOracleView view) const noexcept override;

        void beginShadowWork(VulkanIndirectOracleView view, uint32_t slot,
            size_t countRegionCount) override;
        void expectShadowCommand(VulkanIndirectOracleView view, uint32_t slot,
            size_t countIndex,
            const GpuSceneIndexedIndirectCommand& command) override;
        [[nodiscard]] VulkanIndirectOracleResult verifyShadowWork(
            VulkanIndirectOracleView view, uint32_t slot,
            const VulkanIndirectReadback& readback) override;

        void beginOpaqueWork(uint32_t slot, uint32_t primitiveCount,
            uint32_t candidateCount, bool lod, bool occlusion) override;
        void expectOpaqueVisibleCandidate(uint32_t slot,
            uint32_t candidateIndex) override;
        void expectOpaqueOcclusionQuery(uint32_t slot,
            uint32_t candidateIndex) override;
        void expectOpaqueProjectionRejected(uint32_t slot) override;
        void expectOpaqueLod(uint32_t slot, uint32_t primitiveIndex,
            const VulkanOpaqueLodExpectation& expectation) override;
        void verifyOpaqueLodCommands(uint32_t slot, const uint32_t* counts,
            std::span<const uint32_t> binCapacities,
            const GpuSceneIndexedIndirectCommand* commands) override;
        [[nodiscard]] VulkanOcclusionQueryVerdict verifyOcclusionQueries(
            uint32_t slot, const DepthPyramidDeviceResult* results) override;
        [[nodiscard]] bool opaqueCandidateCpuVisible(uint32_t slot,
            uint32_t candidateIndex) const noexcept override;
        [[nodiscard]] bool unsafeGpuSceneOcclusion(uint32_t slot,
            uint32_t candidateIndex) const noexcept override;
        [[nodiscard]] bool rejectOpaqueLodPrimitive(uint32_t slot,
            uint32_t primitiveIndex) override;
        [[nodiscard]] VulkanOpaqueLodVerdict finishOpaqueLod(
            uint32_t slot) override;

        // VirtualShadowDepthSnapshot hook body: copies the scene depth the VSM
        // marking pass consumed into the slot's host-visible snapshot.
        void recordVirtualShadowDepthSnapshot(VkCommandBuffer cmd, uint32_t slot,
            const VulkanVirtualShadowDepthPayload& payload);
        [[nodiscard]] bool beginVirtualShadowVerify(
            const VulkanVirtualShadowOracleInput& input) override;
        void verifyVirtualShadowRequest(uint32_t requestIndex,
            const VirtualShadowPageRequest& request) override;
        [[nodiscard]] uint64_t comparedVirtualShadowRequests()
            const noexcept override;

    private:
        struct ShadowSlot {
            bool begun = false;
            std::vector<uint32_t> expectedCounts;
            std::vector<std::vector<GpuSceneIndexedIndirectCommand>>
                expectedCommands;
        };
        struct OpaqueSlot {
            bool lod = false;
            bool occlusion = false;
            // GPU LOD
            std::vector<GpuSceneIndexedIndirectCommand> expectedCommandsByPrimitive;
            std::vector<uint8_t> seenPrimitives;
            std::vector<GpuSceneIndexedIndirectCommand> commandReadback;
            uint64_t baseTriangles = 0;
            uint64_t oracleTriangles = 0;
            uint64_t oracleReducedCommands = 0;
            uint64_t historyValid = 0;
            uint64_t historyReset = 0;
            uint64_t historyChanged = 0;
            uint64_t deviceTriangles = 0;
            uint64_t mismatchedCommands = 0;
            // Depth occlusion
            uint32_t candidateCount = 0;
            std::vector<uint32_t> projectedCandidateIndices;
            uint32_t projectionRejected = 0;
            std::vector<uint8_t> cpuVisibleCandidates;
            std::vector<uint8_t> cpuProjectedCandidates;
            std::vector<uint8_t> cpuOccludedCandidates;
        };
        struct VirtualShadowSlot {
            VulkanBufferResource depthReadback{};
            VkExtent2D extent{};
            glm::mat4 inverseViewProjection{ 1.0f };
        };

        [[nodiscard]] ShadowSlot& shadowSlot(VulkanIndirectOracleView view,
            uint32_t slot);

        VulkanIndirectOracleConfig config_{};
        VulkanResourceAllocator* allocator_ = nullptr;
        CpuProfiler* profiler_ = nullptr;
        std::array<std::array<ShadowSlot, FramesInFlight>,
            kVulkanShadowOracleViewCount> shadow_{};
        std::array<OpaqueSlot, FramesInFlight> opaque_{};
        std::array<VirtualShadowSlot, FramesInFlight> virtualShadow_{};
        std::optional<DirectionalVirtualShadowMarkPlan> virtualShadowPlan_;
    };

} // namespace Iridium
