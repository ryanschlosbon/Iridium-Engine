#pragma once

// M7.10.D GPU device-loss diagnostics.
//
// When a Vulkan call in the frame path returns VK_ERROR_DEVICE_LOST, the
// error helper calls noteVulkanDeviceLoss before it throws. The active
// VulkanDeviceLossDiagnostics then reports, once per process:
//   - VK_EXT_device_fault (enabled whenever the device supports it): the
//     driver's fault description, faulting addresses (read / write / execute
//     / instruction pointer, with precision) and vendor records; the vendor
//     binary, when supported, is written beside the report;
//   - VK_NV_device_diagnostic_checkpoints (--gpu-crash-checkpoints, on by
//     default): the last breadcrumb each pipeline stage reached on every
//     queue. Breadcrumbs mark the start and end of every executed render-graph
//     pass, the frame's start and its submission, and every upload-lane batch;
//     each marker is an integer carried in the pointer value (kind, frame
//     serial or timeline value, pass index), decoded to the pass name;
//   - the CPU trail of the last frames: serial, slot and the last passes
//     recorded, with whether the graphics timeline reached them.
// The report is one line `IRIDIUM_DEVICE_LOST {json}` on stderr and the same
// JSON in <reportDirectory>/<UTC time>-<pid>.json (write failures ignored).
// Steady frames never allocate; only the report path does.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>

class VkContext;

namespace Iridium {

    class VulkanFrameScheduler;
    class VulkanRenderGraphExecutor;
    struct VulkanPassCheckpointSink;

    enum class VulkanCheckpointKind : uint8_t {
        Pass = 0,         // index: compiled pass order; serial: frame serial
        FrameBegin = 1,   // serial: frame serial
        FrameSubmit = 2,  // serial: frame serial (last command before submit)
        Upload = 3,       // index: upload lane; serial: lane timeline value
    };

    struct VulkanCheckpointMarker {
        VulkanCheckpointKind kind = VulkanCheckpointKind::Pass;
        bool end = false;
        uint32_t index = 0;
        uint64_t serial = 0;
    };

    // 64-bit layout: [63:60] tag 0xA, [59:58] kind, [57] end, [56:40] index
    // (17 bits), [39:0] serial (40 bits). Out-of-range fields are truncated.
    [[nodiscard]] uint64_t encodeVulkanCheckpointMarker(
        const VulkanCheckpointMarker& marker) noexcept;
    // False when `value` does not carry the tag (not an Iridium marker).
    [[nodiscard]] bool decodeVulkanCheckpointMarker(uint64_t value,
        VulkanCheckpointMarker& marker) noexcept;

    class VulkanDeviceLossDiagnostics final {
    public:
        struct Sources {
            const ::VkContext* context = nullptr;
            const VulkanFrameScheduler* scheduler = nullptr;
            const VulkanRenderGraphExecutor* graph = nullptr;
            // Empty: the report goes to stderr only.
            std::filesystem::path reportDirectory;
        };

        VulkanDeviceLossDiagnostics() = default;
        ~VulkanDeviceLossDiagnostics() { cleanup(); }
        VulkanDeviceLossDiagnostics(const VulkanDeviceLossDiagnostics&) = delete;
        VulkanDeviceLossDiagnostics& operator=(const VulkanDeviceLossDiagnostics&) = delete;

        // After the device exists; becomes the process's active reporter.
        void init(const Sources& sources);
        // Before the device is destroyed.
        void cleanup() noexcept;

        [[nodiscard]] bool checkpointsEnabled() const noexcept {
            return setCheckpoint_ != nullptr;
        }
        [[nodiscard]] bool deviceFaultEnabled() const noexcept {
            return getFaultInfo_ != nullptr;
        }

        // ---- steady-frame recording (no allocation) ----------------------
        // A Ready frame's command buffer is open and nothing is recorded yet.
        void beginFrame(VkCommandBuffer commandBuffer, uint64_t serial,
            uint32_t slot) noexcept;
        // Last command of the frame before vkEndCommandBuffer and submission.
        void markFrameSubmit(VkCommandBuffer commandBuffer) noexcept;
        void markUpload(VkCommandBuffer commandBuffer, uint32_t lane,
            uint64_t timelineValue, bool end) noexcept;
        // The executor's per-pass hook (VulkanRenderGraphExecutor::
        // setPassCheckpointSink). The CPU trail is kept with or without
        // checkpoints.
        [[nodiscard]] VulkanPassCheckpointSink passCheckpointSink() noexcept;

        // ---- failure path --------------------------------------------------
        // Builds, prints and writes the report. Only the first call in the
        // process reports; returns whether this one did. `simulated` marks a
        // qualification report on a healthy device.
        bool report(VkResult result, const char* operation,
            bool simulated = false) noexcept;

    private:
        static constexpr uint32_t TrailFrames = 4;
        static constexpr uint32_t TrailPasses = 16;
        struct FrameTrail {
            uint64_t serial = 0;
            uint64_t graphRebuild = 0;
            uint32_t slot = 0;
            uint32_t passesBegun = 0;
            uint32_t lastEndedPass = UINT32_MAX;
            uint64_t lastMarker = 0;
            // Ring of the last TrailPasses begun passes (index passesBegun %).
            std::array<uint32_t, TrailPasses> recentPasses{};
        };

        static void markPass(void* owner, VkCommandBuffer commandBuffer,
            uint32_t passOrder, bool end) noexcept;
        void setCheckpoint(VkCommandBuffer commandBuffer,
            const VulkanCheckpointMarker& marker) noexcept;
        void writeReport(VkResult result, const char* operation, bool simulated);

        Sources sources_{};
        VkDevice device_ = VK_NULL_HANDLE;
        struct QueueInfo {
            VkQueue queue = VK_NULL_HANDLE;
            uint32_t family = 0;
            const char* role = "";
        };
        std::array<QueueInfo, 3> queues_{};
        uint32_t queueCount_ = 0;
        bool vendorBinary_ = false;
        PFN_vkCmdSetCheckpointNV setCheckpoint_ = nullptr;
        PFN_vkGetQueueCheckpointDataNV getCheckpoints_ = nullptr;
        PFN_vkGetDeviceFaultInfoEXT getFaultInfo_ = nullptr;
        std::array<FrameTrail, TrailFrames> trails_{};
        uint32_t currentTrail_ = 0;
        uint64_t recordingSerial_ = 0;
        uint64_t framesRecorded_ = 0;
    };

    // Every Vulkan error path of the frame loop calls this before it throws:
    // with VK_ERROR_DEVICE_LOST and an active reporter, it reports (once per
    // process). Other results are ignored.
    void noteVulkanDeviceLoss(VkResult result, const char* operation) noexcept;

} // namespace Iridium
