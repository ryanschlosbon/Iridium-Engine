#include "VulkanDeviceLossDiagnostics.h"

#include "VkContext.h"
#include "VulkanFrameScheduler.h"
#include "VulkanRenderGraphExecutor.h"
#include "core/BuildInfo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Iridium {

    namespace {

        static_assert(sizeof(void*) == sizeof(uint64_t),
            "Checkpoint markers carry 64-bit integers in the pointer value");

        constexpr uint64_t MarkerTag = 0xAull;
        constexpr uint64_t IndexMask = (1ull << 17) - 1ull;
        constexpr uint64_t SerialMask = (1ull << 40) - 1ull;

        using Json = nlohmann::ordered_json;

        std::atomic<VulkanDeviceLossDiagnostics*> activeReporter{ nullptr };
        std::atomic<bool> reported{ false };

        std::string hex(uint64_t value) {
            char text[19]{};
            std::snprintf(text, sizeof(text), "0x%016llx",
                static_cast<unsigned long long>(value));
            return text;
        }

        std::string hex32(uint32_t value) {
            char text[11]{};
            std::snprintf(text, sizeof(text), "0x%08x", value);
            return text;
        }

        const char* resultName(VkResult result) noexcept {
            switch (result) {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_INCOMPLETE: return "VK_INCOMPLETE";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            default: return "other";
            }
        }

        Json resultJson(VkResult result) {
            return { { "code", static_cast<int>(result) }, { "name", resultName(result) } };
        }

        std::string fixedString(const char* text, size_t capacity) {
            return std::string(text, strnlen(text, capacity));
        }

        const char* checkpointKindName(VulkanCheckpointKind kind) noexcept {
            switch (kind) {
            case VulkanCheckpointKind::Pass: return "pass";
            case VulkanCheckpointKind::FrameBegin: return "frame-begin";
            case VulkanCheckpointKind::FrameSubmit: return "frame-submit";
            case VulkanCheckpointKind::Upload: return "upload";
            }
            return "unknown";
        }

        const char* faultAddressTypeName(VkDeviceFaultAddressTypeEXT type) noexcept {
            switch (type) {
            case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
                return "instruction-pointer-unknown";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
                return "instruction-pointer-invalid";
            case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
                return "instruction-pointer-fault";
            default: return "unknown";
            }
        }

        std::string pipelineStageName(VkPipelineStageFlagBits stage) {
            switch (stage) {
            case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT: return "TOP_OF_PIPE";
            case VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT: return "DRAW_INDIRECT";
            case VK_PIPELINE_STAGE_VERTEX_INPUT_BIT: return "VERTEX_INPUT";
            case VK_PIPELINE_STAGE_VERTEX_SHADER_BIT: return "VERTEX_SHADER";
            case VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT:
                return "TESSELLATION_CONTROL_SHADER";
            case VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT:
                return "TESSELLATION_EVALUATION_SHADER";
            case VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT: return "GEOMETRY_SHADER";
            case VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT: return "FRAGMENT_SHADER";
            case VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT: return "EARLY_FRAGMENT_TESTS";
            case VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT: return "LATE_FRAGMENT_TESTS";
            case VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:
                return "COLOR_ATTACHMENT_OUTPUT";
            case VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT: return "COMPUTE_SHADER";
            case VK_PIPELINE_STAGE_TRANSFER_BIT: return "TRANSFER";
            case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: return "BOTTOM_OF_PIPE";
            case VK_PIPELINE_STAGE_HOST_BIT: return "HOST";
            case VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT: return "ALL_GRAPHICS";
            case VK_PIPELINE_STAGE_ALL_COMMANDS_BIT: return "ALL_COMMANDS";
            default: return hex(static_cast<uint64_t>(stage));
            }
        }

        struct UtcStamp {
            std::string iso;    // 2026-10-13T12:34:56.789Z
            std::string file;   // 20261013-123456-789
        };

        UtcStamp utcNow() {
            const auto now = std::chrono::system_clock::now();
            const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
            const auto milliseconds = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count() % 1000);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &seconds);
#else
            gmtime_r(&seconds, &utc);
#endif
            char iso[32]{};
            std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec, milliseconds);
            char file[32]{};
            std::snprintf(file, sizeof(file), "%04d%02d%02d-%02d%02d%02d-%03d",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec, milliseconds);
            return { iso, file };
        }

        int processId() noexcept {
#if defined(_WIN32)
            return _getpid();
#else
            return static_cast<int>(getpid());
#endif
        }

        std::string apiVersionText(uint32_t version) {
            return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                std::to_string(VK_API_VERSION_MINOR(version)) + "." +
                std::to_string(VK_API_VERSION_PATCH(version));
        }

    } // namespace

    uint64_t encodeVulkanCheckpointMarker(const VulkanCheckpointMarker& marker) noexcept {
        return (MarkerTag << 60) |
            (static_cast<uint64_t>(marker.kind) & 0x3ull) << 58 |
            (marker.end ? 1ull : 0ull) << 57 |
            (static_cast<uint64_t>(marker.index) & IndexMask) << 40 |
            (marker.serial & SerialMask);
    }

    bool decodeVulkanCheckpointMarker(uint64_t value,
        VulkanCheckpointMarker& marker) noexcept {
        if ((value >> 60) != MarkerTag) return false;
        marker.kind = static_cast<VulkanCheckpointKind>((value >> 58) & 0x3ull);
        marker.end = ((value >> 57) & 1ull) != 0;
        marker.index = static_cast<uint32_t>((value >> 40) & IndexMask);
        marker.serial = value & SerialMask;
        return true;
    }

    void VulkanDeviceLossDiagnostics::init(const Sources& sources) {
        cleanup();
        if (sources.context == nullptr)
            throw std::invalid_argument("Device-loss diagnostics need a Vulkan context");
        sources_ = sources;
        const ::VkContext& context = *sources.context;
        device_ = context.getDevice();
        queueCount_ = 0;
        const auto addQueue = [&](VkQueue queue, uint32_t family, const char* role) {
            if (queue == VK_NULL_HANDLE) return;
            for (uint32_t index = 0; index < queueCount_; ++index)
                if (queues_[index].queue == queue) return;
            queues_[queueCount_++] = { queue, family, role };
        };
        addQueue(context.getGraphicsQueue(), context.getGraphicsQueueFamily(), "graphics");
        addQueue(context.getPresentQueue(), context.getGraphicsQueueFamily(), "present");
        addQueue(context.getTransferQueue(), context.getTransferQueueFamily(), "transfer");
        if (context.hasDiagnosticCheckpoints()) {
            setCheckpoint_ = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(
                vkGetDeviceProcAddr(device_, "vkCmdSetCheckpointNV"));
            getCheckpoints_ = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(
                vkGetDeviceProcAddr(device_, "vkGetQueueCheckpointDataNV"));
            if (setCheckpoint_ == nullptr || getCheckpoints_ == nullptr) {
                setCheckpoint_ = nullptr;
                getCheckpoints_ = nullptr;
            }
        }
        if (context.hasDeviceFault()) {
            getFaultInfo_ = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
                vkGetDeviceProcAddr(device_, "vkGetDeviceFaultInfoEXT"));
            vendorBinary_ = getFaultInfo_ != nullptr && context.hasDeviceFaultVendorBinary();
        }
        trails_ = {};
        currentTrail_ = 0;
        recordingSerial_ = 0;
        framesRecorded_ = 0;
        activeReporter.store(this);
        std::cout << "GPU crash diagnostics: device fault "
            << (deviceFaultEnabled() ? (vendorBinary_ ? "on (vendor binary)" : "on")
                : "unavailable")
            << ", checkpoints "
            << (checkpointsEnabled() ? "on"
                : (context.hasDiagnosticCheckpoints() ? "unavailable" : "off"))
            << '\n';
    }

    void VulkanDeviceLossDiagnostics::cleanup() noexcept {
        VulkanDeviceLossDiagnostics* expected = this;
        (void)activeReporter.compare_exchange_strong(expected, nullptr);
        setCheckpoint_ = nullptr;
        getCheckpoints_ = nullptr;
        getFaultInfo_ = nullptr;
        vendorBinary_ = false;
        device_ = VK_NULL_HANDLE;
        queueCount_ = 0;
        sources_ = {};
    }

    void VulkanDeviceLossDiagnostics::setCheckpoint(VkCommandBuffer commandBuffer,
        const VulkanCheckpointMarker& marker) noexcept {
        const uint64_t value = encodeVulkanCheckpointMarker(marker);
        trails_[currentTrail_].lastMarker = value;
        setCheckpoint_(commandBuffer,
            reinterpret_cast<const void*>(static_cast<uintptr_t>(value)));
    }

    void VulkanDeviceLossDiagnostics::beginFrame(VkCommandBuffer commandBuffer,
        uint64_t serial, uint32_t slot) noexcept {
        recordingSerial_ = serial;
        ++framesRecorded_;
        currentTrail_ = static_cast<uint32_t>(serial % TrailFrames);
        FrameTrail& trail = trails_[currentTrail_];
        trail.serial = serial;
        trail.slot = slot;
        trail.graphRebuild = sources_.graph != nullptr ? sources_.graph->rebuildCount() : 0;
        trail.passesBegun = 0;
        trail.lastEndedPass = UINT32_MAX;
        trail.lastMarker = 0;
        if (setCheckpoint_ != nullptr)
            setCheckpoint(commandBuffer, { VulkanCheckpointKind::FrameBegin, false, 0, serial });
    }

    void VulkanDeviceLossDiagnostics::markFrameSubmit(VkCommandBuffer commandBuffer) noexcept {
        if (setCheckpoint_ != nullptr)
            setCheckpoint(commandBuffer,
                { VulkanCheckpointKind::FrameSubmit, true, 0, recordingSerial_ });
    }

    void VulkanDeviceLossDiagnostics::markUpload(VkCommandBuffer commandBuffer,
        uint32_t lane, uint64_t timelineValue, bool end) noexcept {
        if (setCheckpoint_ == nullptr) return;
        // Upload batches are not part of the frame trail.
        setCheckpoint_(commandBuffer, reinterpret_cast<const void*>(static_cast<uintptr_t>(
            encodeVulkanCheckpointMarker(
                { VulkanCheckpointKind::Upload, end, lane, timelineValue }))));
    }

    void VulkanDeviceLossDiagnostics::markPass(void* owner, VkCommandBuffer commandBuffer,
        uint32_t passOrder, bool end) noexcept {
        auto& self = *static_cast<VulkanDeviceLossDiagnostics*>(owner);
        FrameTrail& trail = self.trails_[self.currentTrail_];
        if (end) {
            trail.lastEndedPass = passOrder;
        }
        else {
            trail.recentPasses[trail.passesBegun % TrailPasses] = passOrder;
            ++trail.passesBegun;
        }
        if (self.setCheckpoint_ != nullptr)
            self.setCheckpoint(commandBuffer,
                { VulkanCheckpointKind::Pass, end, passOrder, self.recordingSerial_ });
    }

    VulkanPassCheckpointSink VulkanDeviceLossDiagnostics::passCheckpointSink() noexcept {
        return { this, &VulkanDeviceLossDiagnostics::markPass };
    }

    bool VulkanDeviceLossDiagnostics::report(VkResult result, const char* operation,
        bool simulated) noexcept {
        if (reported.exchange(true)) return false;
        try {
            writeReport(result, operation, simulated);
        }
        catch (const std::exception& error) {
            try {
                std::cerr << "IRIDIUM_DEVICE_LOST {\"reportError\":\"" << error.what()
                    << "\"}" << std::endl;
            }
            catch (...) {}
        }
        catch (...) {}
        return true;
    }

    void VulkanDeviceLossDiagnostics::writeReport(VkResult result, const char* operation,
        bool simulated) {
        const UtcStamp stamp = utcNow();
        std::filesystem::path reportFile;
        if (!sources_.reportDirectory.empty())
            reportFile = sources_.reportDirectory /
                (stamp.file + "-" + std::to_string(processId()) + ".json");

        const RenderGraph::CompiledGraph* plan =
            sources_.graph != nullptr ? sources_.graph->compiledGraph() : nullptr;
        const uint64_t planRebuild =
            sources_.graph != nullptr ? sources_.graph->rebuildCount() : 0;
        const auto passName = [&](uint32_t passOrder) -> std::string {
            if (plan == nullptr || passOrder >= plan->passes().size()) return "?";
            return plan->passes()[passOrder].name;
        };
        const auto trailFor = [&](uint64_t serial) -> const FrameTrail* {
            for (const FrameTrail& trail : trails_)
                if (trail.serial == serial && serial != 0) return &trail;
            return nullptr;
        };
        const auto describeMarker = [&](uint64_t value) {
            Json entry;
            entry["marker"] = hex(value);
            VulkanCheckpointMarker marker{};
            if (!decodeVulkanCheckpointMarker(value, marker)) {
                entry["kind"] = "foreign";
                return entry;
            }
            entry["kind"] = checkpointKindName(marker.kind);
            entry["end"] = marker.end;
            if (marker.kind == VulkanCheckpointKind::Upload) {
                entry["lane"] = marker.index == 0 ? "transfer" : "graphics";
                entry["timelineValue"] = marker.serial;
                return entry;
            }
            entry["frameSerial"] = marker.serial;
            if (marker.kind == VulkanCheckpointKind::Pass) {
                entry["passIndex"] = marker.index;
                entry["pass"] = passName(marker.index);
                // Pass indices are only meaningful within the plan that
                // recorded them.
                const FrameTrail* trail = trailFor(marker.serial);
                entry["planCurrent"] = trail != nullptr && trail->graphRebuild == planRebuild;
            }
            return entry;
        };

        Json report;
        report["schema"] = "iridium.device-lost.v1";
        report["simulated"] = simulated;
        report["time"] = stamp.iso;
        report["operation"] = operation != nullptr ? operation : "";
        report["vkResult"] = resultJson(result);
        report["engine"] = {
            { "commit", BuildInfo::sourceCommit() },
            { "branch", BuildInfo::sourceBranch() },
            { "dirtyAtConfigure", BuildInfo::sourceDirtyAtConfigure() },
            { "configuration", BuildInfo::configuration() },
            { "vulkanSdk", BuildInfo::vulkanSdk() },
            { "pid", processId() },
        };
        if (sources_.context != nullptr) {
            const ::VkContext& context = *sources_.context;
            const VkPhysicalDeviceProperties& properties =
                context.getPhysicalDeviceProperties();
            const VkPhysicalDeviceDriverProperties& driver =
                context.getPhysicalDeviceDriverProperties();
            report["device"] = {
                { "name", fixedString(properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE) },
                { "vendorId", hex32(properties.vendorID) },
                { "deviceId", hex32(properties.deviceID) },
                { "apiVersion", apiVersionText(properties.apiVersion) },
                { "driverVersion", hex32(properties.driverVersion) },
                { "driverName", fixedString(driver.driverName, VK_MAX_DRIVER_NAME_SIZE) },
                { "driverInfo", fixedString(driver.driverInfo, VK_MAX_DRIVER_INFO_SIZE) },
            };
        }
        report["diagnostics"] = {
            { "deviceFault", deviceFaultEnabled() },
            { "deviceFaultVendorBinary", vendorBinary_ },
            { "checkpoints", checkpointsEnabled() },
        };

        // Frame state. The graphics timeline value is the last frame serial
        // the GPU finished (fence path: completedSerial only).
        uint64_t completedSerial = 0;
        {
            Json frame;
            frame["framesRecorded"] = framesRecorded_;
            frame["recordingSerial"] = recordingSerial_;
            if (sources_.scheduler != nullptr) {
                const VulkanFrameScheduler& scheduler = *sources_.scheduler;
                completedSerial = scheduler.completedSerial();
                frame["slot"] = scheduler.currentFrameIndex();
                frame["lastSubmittedSerial"] = scheduler.lastSubmittedSerial();
                frame["completedSerial"] = scheduler.completedSerial();
                if (scheduler.graphicsTimeline() != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    uint64_t value = 0;
                    const VkResult timelineResult = vkGetSemaphoreCounterValue(device_,
                        scheduler.graphicsTimeline(), &value);
                    frame["graphicsTimelineQuery"] = resultJson(timelineResult);
                    if (timelineResult == VK_SUCCESS) {
                        frame["graphicsTimelineValue"] = value;
                        completedSerial = (std::max)(completedSerial, value);
                    }
                }
            }
            report["frame"] = std::move(frame);
        }

        // VK_EXT_device_fault.
        std::vector<uint8_t> vendorBinary;
        if (getFaultInfo_ != nullptr && device_ != VK_NULL_HANDLE) {
            Json fault;
            VkDeviceFaultCountsEXT counts{ VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
            const VkResult countResult = getFaultInfo_(device_, &counts, nullptr);
            fault["countQuery"] = resultJson(countResult);
            fault["addressInfoCount"] = counts.addressInfoCount;
            fault["vendorInfoCount"] = counts.vendorInfoCount;
            fault["vendorBinarySize"] = counts.vendorBinarySize;
            if (countResult == VK_SUCCESS || countResult == VK_INCOMPLETE) {
                std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
                std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
                if (!vendorBinary_) counts.vendorBinarySize = 0;
                vendorBinary.resize(static_cast<size_t>(counts.vendorBinarySize));
                VkDeviceFaultInfoEXT info{ VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
                info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
                info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
                info.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();
                const VkResult infoResult = getFaultInfo_(device_, &counts, &info);
                fault["infoQuery"] = resultJson(infoResult);
                if (infoResult == VK_SUCCESS || infoResult == VK_INCOMPLETE) {
                    fault["description"] = fixedString(info.description,
                        VK_MAX_DESCRIPTION_SIZE);
                    Json addressInfos = Json::array();
                    for (uint32_t index = 0;
                         index < counts.addressInfoCount && index < addresses.size(); ++index) {
                        const VkDeviceFaultAddressInfoEXT& address = addresses[index];
                        // The fault lies in [address & ~(precision-1),
                        // address | (precision-1)] (precision: a power of two).
                        const uint64_t precision = address.addressPrecision;
                        const uint64_t lowMask = precision > 1 ? precision - 1 : 0;
                        addressInfos.push_back({
                            { "type", faultAddressTypeName(address.addressType) },
                            { "address", hex(address.reportedAddress) },
                            { "precision", precision },
                            { "rangeLow", hex(address.reportedAddress & ~lowMask) },
                            { "rangeHigh", hex(address.reportedAddress | lowMask) },
                        });
                    }
                    fault["addressInfos"] = std::move(addressInfos);
                    Json vendorInfos = Json::array();
                    for (uint32_t index = 0;
                         index < counts.vendorInfoCount && index < vendors.size(); ++index) {
                        const VkDeviceFaultVendorInfoEXT& vendor = vendors[index];
                        vendorInfos.push_back({
                            { "description", fixedString(vendor.description,
                                VK_MAX_DESCRIPTION_SIZE) },
                            { "code", hex(vendor.vendorFaultCode) },
                            { "data", hex(vendor.vendorFaultData) },
                        });
                    }
                    fault["vendorInfos"] = std::move(vendorInfos);
                    vendorBinary.resize(static_cast<size_t>(counts.vendorBinarySize));
                }
                else {
                    vendorBinary.clear();
                }
            }
            if (!vendorBinary.empty() && !reportFile.empty()) {
                std::filesystem::path binaryFile = reportFile;
                binaryFile.replace_extension(".vendor.bin");
                fault["vendorBinaryFile"] = binaryFile.generic_string();
            }
            report["fault"] = std::move(fault);
        }

        // VK_NV_device_diagnostic_checkpoints: per queue, the last marker
        // each pipeline stage reached. TOP_OF_PIPE is the last work started,
        // BOTTOM_OF_PIPE the last work finished; a hang or fault lies
        // between them.
        if (getCheckpoints_ != nullptr) {
            Json queues = Json::array();
            for (uint32_t index = 0; index < queueCount_; ++index) {
                const QueueInfo& queue = queues_[index];
                uint32_t count = 0;
                getCheckpoints_(queue.queue, &count, nullptr);
                std::vector<VkCheckpointDataNV> data(count,
                    VkCheckpointDataNV{ VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV });
                if (count != 0) getCheckpoints_(queue.queue, &count, data.data());
                Json entries = Json::array();
                for (uint32_t entryIndex = 0; entryIndex < count; ++entryIndex) {
                    Json entry = describeMarker(static_cast<uint64_t>(
                        reinterpret_cast<uintptr_t>(data[entryIndex].pCheckpointMarker)));
                    entry["stage"] = pipelineStageName(data[entryIndex].stage);
                    entries.push_back(std::move(entry));
                }
                queues.push_back({
                    { "queue", queue.role },
                    { "family", queue.family },
                    { "entries", std::move(entries) },
                });
            }
            report["checkpoints"] = std::move(queues);
        }

        // The CPU trail: what each recent frame recorded, oldest first.
        {
            Json frames = Json::array();
            std::array<const FrameTrail*, TrailFrames> ordered{};
            uint32_t orderedCount = 0;
            for (const FrameTrail& trail : trails_)
                if (trail.serial != 0) ordered[orderedCount++] = &trail;
            std::sort(ordered.begin(), ordered.begin() + orderedCount,
                [](const FrameTrail* a, const FrameTrail* b) { return a->serial < b->serial; });
            for (uint32_t index = 0; index < orderedCount; ++index) {
                const FrameTrail& trail = *ordered[index];
                Json lastPasses = Json::array();
                const uint32_t shown = (std::min)(trail.passesBegun, TrailPasses);
                for (uint32_t pass = trail.passesBegun - shown; pass < trail.passesBegun; ++pass)
                    lastPasses.push_back(passName(trail.recentPasses[pass % TrailPasses]));
                Json frame;
                frame["serial"] = trail.serial;
                frame["slot"] = trail.slot;
                frame["gpuCompleted"] = trail.serial <= completedSerial;
                frame["passesBegun"] = trail.passesBegun;
                frame["planCurrent"] = trail.graphRebuild == planRebuild;
                frame["lastPasses"] = std::move(lastPasses);
                frame["lastEndedPass"] = trail.lastEndedPass == UINT32_MAX
                    ? std::string("none") : passName(trail.lastEndedPass);
                if (trail.lastMarker != 0)
                    frame["lastRecordedMarker"] = describeMarker(trail.lastMarker);
                frames.push_back(std::move(frame));
            }
            report["recentFrames"] = std::move(frames);
        }
        report["reportFile"] = reportFile.empty() ? std::string() : reportFile.generic_string();

        const std::string text = report.dump(-1, ' ', false,
            Json::error_handler_t::replace);
        bool written = false;
        if (!reportFile.empty()) {
            std::error_code error;
            std::filesystem::create_directories(sources_.reportDirectory, error);
            std::ofstream file(reportFile, std::ios::binary | std::ios::trunc);
            if (file) {
                file << text << '\n';
                written = static_cast<bool>(file);
            }
            if (!vendorBinary.empty()) {
                std::filesystem::path binaryFile = reportFile;
                binaryFile.replace_extension(".vendor.bin");
                std::ofstream binary(binaryFile, std::ios::binary | std::ios::trunc);
                if (binary)
                    binary.write(reinterpret_cast<const char*>(vendorBinary.data()),
                        static_cast<std::streamsize>(vendorBinary.size()));
            }
        }
        std::cerr << "IRIDIUM_DEVICE_LOST " << text << std::endl;
        if (!reportFile.empty() && !written)
            std::cerr << "Device-loss report could not be written to "
                << reportFile.generic_string() << std::endl;
    }

    void noteVulkanDeviceLoss(VkResult result, const char* operation) noexcept {
        if (result != VK_ERROR_DEVICE_LOST) return;
        VulkanDeviceLossDiagnostics* reporter = activeReporter.load();
        if (reporter != nullptr) (void)reporter->report(result, operation);
    }

} // namespace Iridium
