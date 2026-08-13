#include "CpuProfileExport.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace Iridium {

    namespace {

        const char* counterStatusName(ProfileCounterStatus status) noexcept {
            switch (status) {
            case ProfileCounterStatus::Exact: return "exact";
            case ProfileCounterStatus::Estimated: return "estimated";
            case ProfileCounterStatus::NotApplicable: return "not_applicable";
            case ProfileCounterStatus::Unavailable: return "unavailable";
            }
            return "unavailable";
        }

        const char* counterUnitName(ProfileCounterUnit unit) noexcept {
            switch (unit) {
            case ProfileCounterUnit::Count: return "count";
            case ProfileCounterUnit::Bytes: return "bytes";
            case ProfileCounterUnit::Millionths: return "millionths";
            }
            return "count";
        }

        nlohmann::ordered_json statisticsJson(
            const ProfileRangeRunStatistics& range) {
            const ProfileStatistics& statistics = range.statistics;
            return {
                { "unit", "nanoseconds" },
                { "current", statistics.current },
                { "median", statistics.median },
                { "p95", statistics.p95 },
                { "p99", statistics.p99 },
                { "minimum", statistics.minimum },
                { "maximum", statistics.maximum },
                { "sample_count", statistics.sampleCount },
                { "missing_frame_count", range.missingFrameCount },
                { "explicitly_unavailable_frame_count",
                    range.explicitlyUnavailableFrameCount },
                { "sample_capacity_overflow_count",
                    range.sampleCapacityOverflowCount },
                { "duration_overflow_frame_count",
                    range.durationOverflowFrameCount },
            };
        }

        int64_t signedDelta(uint64_t current, uint64_t previous) noexcept {
            constexpr uint64_t SignedMaximum = static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max());
            if (current >= previous) {
                return static_cast<int64_t>(std::min(current - previous, SignedMaximum));
            }
            return -static_cast<int64_t>(std::min(previous - current, SignedMaximum));
        }

    } // namespace

    void writeCpuProfileJsonLines(std::ostream& output,
        const CpuProfiler& profiler, const CpuProfileRunMetadata& metadata) {
        const std::vector<CpuFrameProfile> frames = profiler.snapshotCompletedFrames();
        const ProfileRunStatistics runStatistics = profiler.snapshotRunStatistics();

        nlohmann::ordered_json benchmark = {
            { "fixture_id", metadata.benchmarkFixtureId.empty()
                ? nlohmann::ordered_json(nullptr)
                : nlohmann::ordered_json(metadata.benchmarkFixtureId) },
            { "fixture_revision", metadata.benchmarkFixtureId.empty()
                ? nlohmann::ordered_json(nullptr)
                : nlohmann::ordered_json(metadata.benchmarkFixtureRevision) },
            { "camera_id", metadata.benchmarkCameraId.empty()
                ? nlohmann::ordered_json(nullptr)
                : nlohmann::ordered_json(metadata.benchmarkCameraId) },
            { "manifest_path", metadata.benchmarkManifestPath.empty()
                ? nlohmann::ordered_json(nullptr)
                : nlohmann::ordered_json(metadata.benchmarkManifestPath) },
            { "manifest_sha256", metadata.benchmarkManifestSha256.empty()
                ? nlohmann::ordered_json(nullptr)
                : nlohmann::ordered_json(metadata.benchmarkManifestSha256) },
            { "content_hashes", nlohmann::ordered_json::array() },
        };
        for (const auto& [path, hash] : metadata.benchmarkContentHashes) {
            benchmark["content_hashes"].push_back({
                { "path", path }, { "sha256", hash }
            });
        }

        nlohmann::ordered_json captureOutputs = nlohmann::ordered_json::array();
        for (const auto& [path, hash] : metadata.captureOutputs) {
            captureOutputs.push_back({
                { "path", path }, { "sha256", hash }
            });
        }

        nlohmann::ordered_json header = {
            { "type", "run_header" },
            { "schema_version", 1 },
            { "run_id", metadata.runId },
            { "source", {
                { "commit", metadata.sourceCommit },
                { "branch", metadata.sourceBranch },
                { "dirty_at_configure", metadata.sourceDirtyAtConfigure },
            } },
            { "build_configuration", metadata.buildConfiguration },
            { "toolchain", {
                { "compiler", metadata.compiler },
                { "shader_compiler", metadata.shaderCompiler },
                { "vulkan_sdk", metadata.vulkanSdkVersion },
            } },
            { "system", {
                { "operating_system", metadata.operatingSystem },
                { "cpu", metadata.cpuName },
                { "physical_memory_bytes", metadata.systemMemoryBytes },
            } },
            { "gpu", {
                { "name", metadata.gpuName },
                { "uuid", metadata.gpuUuid },
                { "vendor_id", metadata.gpuVendorId },
                { "device_id", metadata.gpuDeviceId },
                { "driver_name", metadata.gpuDriverName },
                { "driver_version", metadata.gpuDriverVersion },
                { "driver_info", metadata.gpuDriverInfo },
            } },
            { "vulkan", {
                { "device_api", metadata.vulkanDeviceApiVersion },
                { "loader_api", metadata.vulkanLoaderApiVersion },
                { "application_enabled_layers", metadata.applicationEnabledLayers },
                { "active_tools", metadata.activeVulkanTools },
            } },
            { "validation_enabled", metadata.validationEnabled },
            { "window_visible", metadata.windowVisible },
            { "window_decorated", metadata.windowDecorated },
            { "requested_window_size", {
                { "width", metadata.requestedWindowWidth },
                { "height", metadata.requestedWindowHeight },
            } },
            { "render_extent", {
                { "width", metadata.renderWidth },
                { "height", metadata.renderHeight },
            } },
            { "frame_limit", metadata.frameLimit },
            { "warmup_frames", metadata.warmupFrameCount },
            { "measured_frames", metadata.measuredFrameCount },
            { "measurement_wall_ns", metadata.measurementWallNanoseconds },
            { "profiling", {
                { "cpu_enabled", metadata.cpuProfilingEnabled },
                { "gpu_requested", metadata.gpuProfilingRequested },
                { "gpu_available", metadata.gpuProfilingAvailable },
                { "gpu_timestamp_period_ns", metadata.gpuTimestampPeriodNanoseconds },
                { "gpu_timestamp_valid_bits", metadata.gpuTimestampValidBits },
                { "engine_allocation_tracking_available",
                    metadata.engineAllocationTrackingAvailable },
                { "driver_memory_budget_available",
                    metadata.driverMemoryBudgetAvailable },
                { "cpp_allocation_tracking_available",
                    metadata.cppAllocationTrackingAvailable },
                { "transparent_pipeline_statistics_requested",
                    metadata.transparentPipelineStatisticsRequested },
                { "transparent_pipeline_statistics_available",
                    metadata.transparentPipelineStatisticsAvailable },
                { "gpu_light_records_available",
                    metadata.gpuLightRecordsAvailable },
                { "max_gpu_light_records", metadata.maxGpuLightRecords },
            } },
            { "display", {
                { "swapchain_format", metadata.swapchainFormat },
                { "swapchain_color_space", metadata.swapchainColorSpace },
                { "present_mode", metadata.presentMode },
                { "swapchain_image_count", metadata.swapchainImageCount },
                { "output_mode", metadata.outputMode },
				{ "supported_output_transports", metadata.supportedOutputTransports },
				{ "requested_output_transport", metadata.requestedOutputTransport },
				{ "effective_output_transport", metadata.effectiveOutputTransport },
				{ "output_transport_diagnostic", metadata.outputTransportDiagnostic },
				{ "swapchain_colorspace_extension_enabled",
					metadata.swapchainColorspaceExtensionEnabled },
				{ "hdr_metadata_extension_enabled",
					metadata.hdrMetadataExtensionEnabled },
                { "hdr_metadata_applied", metadata.hdrMetadataApplied },
                { "display_profile", metadata.displayProfile },
                { "output_transfer", metadata.outputTransfer },
                { "paper_white_nits", metadata.paperWhiteNits },
                { "peak_nits", metadata.peakNits },
                { "scrgb_nits_per_unit", metadata.scRgbNitsPerUnit },
            } },
            { "render_configuration", {
                { "base_resolution", {
                    { "width", metadata.baseWidth },
                    { "height", metadata.baseHeight },
                } },
                { "reconstruction_mode", metadata.reconstructionMode },
                { "quality_settings", metadata.qualitySettings },
                { "render_mode", metadata.renderMode },
                { "cache_state", metadata.cacheState },
                { "output_operator", metadata.outputOperator },
                { "exposure_state", metadata.exposureState },
                { "render_graph", {
                    { "enabled", metadata.renderGraphEnabled },
                    { "topology_hash", metadata.renderGraphTopologyHash },
                    { "pass_count", metadata.renderGraphPassCount },
                    { "logical_resource_count",
                        metadata.renderGraphLogicalResourceCount },
                    { "physical_slot_count", metadata.renderGraphPhysicalSlotCount },
                    { "barrier_count", metadata.renderGraphBarrierCount },
                    { "frame_count", metadata.renderGraphFrameCount },
                    { "requested_bytes", metadata.renderGraphRequestedBytes },
                    { "committed_bytes", metadata.renderGraphCommittedBytes },
                    { "rebuild_count", metadata.renderGraphRebuildCount },
                    { "cache_miss_count", metadata.renderGraphCacheMissCount },
                } },
                { "gpu_lights", {
                    { "capacity", metadata.gpuLightCapacity },
                    { "active_count", metadata.gpuLightActiveCount },
                    { "last_upload_bytes", metadata.gpuLightUploadBytes },
                    { "last_upload_ranges", metadata.gpuLightUploadRanges },
                } },
            } },
            { "startup", {
                { "total_ns", metadata.startupTotalNanoseconds },
                { "window_init_ns", metadata.windowInitNanoseconds },
                { "backend_init_ns", metadata.backendInitNanoseconds },
                { "editor_init_ns", metadata.editorInitNanoseconds },
                { "manifest_verification_ns", metadata.manifestVerificationNanoseconds },
                { "source_import_ns", metadata.sourceImportNanoseconds },
                { "model_load_ns", metadata.modelLoadNanoseconds },
                { "environment_creation_ns", metadata.environmentCreationNanoseconds },
                { "scene_construction_ns", metadata.sceneConstructionNanoseconds },
                { "model_load_mode", metadata.modelLoadMode },
                { "model_location", metadata.modelLocation },
                { "model_asset_guid", metadata.modelAssetGuid },
                { "model_artifact_cook_key",
                    metadata.modelArtifactCookKey },
                { "environment_load_mode", metadata.environmentLoadMode },
                { "environment_location", metadata.environmentLocation },
                { "environment_asset_guid", metadata.environmentAssetGuid },
                { "environment_artifact_cook_key",
                    metadata.environmentArtifactCookKey },
                { "environment_source_texture_guid",
                    metadata.environmentSourceTextureGuid },
                { "environment_source_primaries",
                    metadata.environmentSourcePrimaries },
                { "environment_radiance_scale",
                    metadata.environmentRadianceScale },
                { "upload_submitted_bytes", metadata.uploadSubmittedBytes },
                { "upload_submitted_batches", metadata.uploadSubmittedBatches },
                { "upload_submit_and_wait_ns", metadata.uploadSubmitAndWaitNanoseconds },
            } },
            { "color_domain", metadata.colorDomain },
            { "render_debug_view", metadata.renderDebugView },
            { "render_debug_view_semantics", metadata.renderDebugViewSemantics },
            { "benchmark", std::move(benchmark) },
            { "capture_outputs", std::move(captureOutputs) },
            { "cpu_duration_unit", "nanoseconds" },
            { "gpu_duration_unit", "nanoseconds" },
            { "completed_frame_capacity", CpuProfiler::CompletedFrameCapacity },
            { "run_statistic_sample_capacity",
                CpuProfiler::RunStatisticSampleCapacity },
            { "cpu_run_statistic_range_capacity",
                CpuProfiler::MaxCpuRunStatisticRanges },
            { "gpu_run_statistic_range_capacity",
                CpuProfiler::MaxGpuRunStatisticRanges },
            { "frames_retained", frames.size() },
            { "frames_completed", profiler.totalCompletedFrameCount() },
            { "dropped_frames", profiler.droppedFrameCount() },
            { "unavailable_fields", metadata.unavailableFields },
        };
        output << header.dump() << '\n';

        FrameMemoryProfile previousMemory{};
        bool previousMemoryAvailable = false;

        for (const CpuFrameProfile& frame : frames) {
            nlohmann::ordered_json eventJson = nlohmann::ordered_json::array();
            for (const CpuProfileEvent& event : frame.events) {
                const std::string name = event.name != nullptr ? event.name : "unavailable";
                eventJson.push_back({
                    { "name", name },
                    { "event_id", event.eventId },
                    { "parent_event_id", event.parentEventId },
                    { "thread_id", event.threadId },
                    { "start_ns", event.startNanoseconds },
                    { "duration_ns", event.durationNanoseconds },
                });
            }

            nlohmann::ordered_json counterJson = nlohmann::ordered_json::array();
            for (const FrameProfileCounter& counter : frame.counters) {
                counterJson.push_back({
                    { "name", counter.name != nullptr ? counter.name : "unavailable" },
                    { "value", counter.value },
                    { "status", counterStatusName(counter.status) },
                    { "unit", counterUnitName(counter.unit) },
                });
            }

            nlohmann::ordered_json gpuRangeJson = nlohmann::ordered_json::array();
            for (const GpuProfileRange& range : frame.gpuRanges) {
                const std::string name = range.name != nullptr
                    ? range.name
                    : "unavailable";
                gpuRangeJson.push_back({
                    { "name", name },
                    { "queue", "graphics" },
                    { "start_ns", range.startNanoseconds },
                    { "duration_ns", range.durationNanoseconds },
                    { "available", range.available },
                });
            }

            nlohmann::ordered_json memoryCategoryJson =
                nlohmann::ordered_json::array();
            const uint32_t categoryCount = std::min<uint32_t>(
                frame.memory.categoryCount,
                static_cast<uint32_t>(frame.memory.categories.size()));
            for (uint32_t index = 0; index < categoryCount; ++index) {
                const ProfileMemoryCategorySnapshot& category =
                    frame.memory.categories[index];
                nlohmann::ordered_json categoryJson = {
                    { "name", category.name != nullptr ? category.name : "unavailable" },
                    { "lifetime_class", category.lifetimeClass != nullptr
                        ? category.lifetimeClass
                        : "unavailable" },
                    { "requested_live_bytes", category.requestedLiveBytes },
                    { "requested_peak_bytes", category.requestedPeakBytes },
                    { "committed_live_bytes", category.committedLiveBytes },
                    { "committed_peak_bytes", category.committedPeakBytes },
                    { "live_allocation_count", category.liveAllocationCount },
                    { "peak_allocation_count", category.peakAllocationCount },
                    { "observed_memory_type_mask", category.observedMemoryTypeMask },
                    { "observed_memory_heap_mask", category.observedMemoryHeapMask },
                    { "requested_bytes_available", category.requestedBytesAvailable },
                    { "committed_bytes_available", category.committedBytesAvailable },
                    { "engine_owned", category.engineOwned },
                    { "delta_available", previousMemoryAvailable },
                };
                if (previousMemoryAvailable && index < previousMemory.categoryCount) {
                    const ProfileMemoryCategorySnapshot& previous =
                        previousMemory.categories[index];
                    categoryJson["requested_live_delta_bytes"] = signedDelta(
                        category.requestedLiveBytes, previous.requestedLiveBytes);
                    categoryJson["committed_live_delta_bytes"] = signedDelta(
                        category.committedLiveBytes, previous.committedLiveBytes);
                    categoryJson["live_allocation_delta"] = signedDelta(
                        category.liveAllocationCount, previous.liveAllocationCount);
                }
                else {
                    categoryJson["requested_live_delta_bytes"] = nullptr;
                    categoryJson["committed_live_delta_bytes"] = nullptr;
                    categoryJson["live_allocation_delta"] = nullptr;
                }
                memoryCategoryJson.push_back(std::move(categoryJson));
            }

            nlohmann::ordered_json heapJson = nlohmann::ordered_json::array();
            const uint32_t heapCount = std::min<uint32_t>(frame.memory.heapCount,
                static_cast<uint32_t>(frame.memory.heaps.size()));
            for (uint32_t index = 0; index < heapCount; ++index) {
                const ProfileMemoryHeapSnapshot& heap = frame.memory.heaps[index];
                heapJson.push_back({
                    { "heap_index", index },
                    { "heap_size_bytes", heap.heapSizeBytes },
                    { "flags", heap.flags },
                    { "engine_committed_live_bytes", heap.engineCommittedLiveBytes },
                    { "engine_committed_peak_bytes", heap.engineCommittedPeakBytes },
                    { "driver_budget_available", heap.driverBudgetAvailable },
                    { "driver_budget_bytes", heap.driverBudgetBytes },
                    { "driver_usage_bytes", heap.driverUsageBytes },
                });
            }

            nlohmann::ordered_json memoryJson = {
                { "engine_allocation_totals_available",
                    frame.memory.engineAllocationTotalsAvailable },
                { "driver_heap_budget_available",
                    frame.memory.driverHeapBudgetAvailable },
                { "engine_totals", {
                    { "requested_live_bytes", frame.memory.engineRequestedLiveBytes },
                    { "requested_peak_bytes", frame.memory.engineRequestedPeakBytes },
                    { "committed_live_bytes", frame.memory.engineCommittedLiveBytes },
                    { "committed_peak_bytes", frame.memory.engineCommittedPeakBytes },
                    { "live_allocation_count", frame.memory.engineLiveAllocationCount },
                    { "peak_allocation_count", frame.memory.enginePeakAllocationCount },
                } },
                { "categories", std::move(memoryCategoryJson) },
                { "driver_heaps", std::move(heapJson) },
            };

            nlohmann::ordered_json frameJson = {
                { "type", "frame" },
                { "schema_version", 1 },
                { "frame_id", frame.frameId },
                { "cpu_events", std::move(eventJson) },
                { "gpu_ranges", std::move(gpuRangeJson) },
                { "counters", std::move(counterJson) },
                { "memory", std::move(memoryJson) },
                { "overflow", {
                    { "dropped_events", frame.droppedEvents },
                    { "dropped_gpu_ranges", frame.droppedGpuRanges },
                    { "dropped_counters", frame.droppedCounters },
                    { "nesting_errors", frame.nestingErrors },
                } },
                { "gpu_ranges_available", !frame.gpuRanges.empty() },
            };
            output << frameJson.dump() << '\n';
            if (frame.memory.engineAllocationTotalsAvailable) {
                previousMemory = frame.memory;
                previousMemoryAvailable = true;
            }
        }

        nlohmann::ordered_json rangeSummary = nlohmann::ordered_json::object();
        for (const ProfileRangeRunStatistics& range : runStatistics.cpuRanges) {
            rangeSummary[range.name != nullptr ? range.name : "unavailable"] =
                statisticsJson(range);
        }

        nlohmann::ordered_json gpuRangeSummary = nlohmann::ordered_json::object();
        for (const ProfileRangeRunStatistics& range : runStatistics.gpuRanges) {
            gpuRangeSummary[range.name != nullptr ? range.name : "unavailable"] =
                statisticsJson(range);
        }

        nlohmann::ordered_json summary = {
            { "type", "run_summary" },
            { "schema_version", 1 },
            { "frames_retained", frames.size() },
            { "frames_completed", profiler.totalCompletedFrameCount() },
            { "dropped_frames", profiler.droppedFrameCount() },
            { "aggregate_scope", "all_completed_frames" },
            { "aggregate_storage", {
                { "sample_capacity_per_range",
                    CpuProfiler::RunStatisticSampleCapacity },
                { "cpu_range_capacity",
                    CpuProfiler::MaxCpuRunStatisticRanges },
                { "gpu_range_capacity",
                    CpuProfiler::MaxGpuRunStatisticRanges },
                { "cpu_detail_overflow_frame_count",
                    runStatistics.cpuDetailOverflowFrameCount },
                { "gpu_detail_overflow_frame_count",
                    runStatistics.gpuDetailOverflowFrameCount },
                { "unaggregated_cpu_range_value_count",
                    runStatistics.unaggregatedCpuRangeValueCount },
                { "unaggregated_gpu_range_value_count",
                    runStatistics.unaggregatedGpuRangeValueCount },
            } },
            { "cpu_ranges", std::move(rangeSummary) },
            { "gpu_ranges", std::move(gpuRangeSummary) },
        };
        if (!frames.empty()) {
            const FrameMemoryProfile& memory = frames.back().memory;
            summary["memory_latest"] = {
                { "engine_allocation_totals_available",
                    memory.engineAllocationTotalsAvailable },
                { "driver_heap_budget_available",
                    memory.driverHeapBudgetAvailable },
                { "requested_live_bytes", memory.engineRequestedLiveBytes },
                { "requested_peak_bytes", memory.engineRequestedPeakBytes },
                { "committed_live_bytes", memory.engineCommittedLiveBytes },
                { "committed_peak_bytes", memory.engineCommittedPeakBytes },
                { "live_allocation_count", memory.engineLiveAllocationCount },
                { "peak_allocation_count", memory.enginePeakAllocationCount },
            };
        }
        output << summary.dump() << '\n';

        if (!output.good()) {
            throw std::runtime_error("Failed while writing CPU profile JSON Lines output");
        }
    }

    void writeCpuProfileJsonLines(const std::filesystem::path& outputPath,
        const CpuProfiler& profiler, const CpuProfileRunMetadata& metadata) {
        if (outputPath.empty()) {
            throw std::invalid_argument("CPU profile output path must not be empty");
        }
        if (outputPath.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(outputPath.parent_path(), error);
            if (error) {
                throw std::runtime_error("Failed to create CPU profile output directory");
            }
        }

        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("Failed to open CPU profile output file");
        }
        writeCpuProfileJsonLines(output, profiler, metadata);
    }

} // namespace Iridium
