#include "utils/Sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

    using Json = nlohmann::json;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
        return false; } } while (false)

    std::filesystem::path root() {
        return std::filesystem::path(PROJECT_ROOT_DIR);
    }

    Json readJson(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("could not read " + path.string());
        return Json::parse(input);
    }

    bool near(double actual, double expected, double absolute = 1.0e-9,
        double relative = 1.0e-9) {
        if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
        const double difference = std::abs(actual - expected);
        return difference <= absolute || difference <=
            relative * std::max(std::abs(expected), 1.0);
    }

    bool hasUuidV7Shape(std::string_view value) {
        return value.size() == 36 && value[8] == '-' && value[13] == '-' &&
            value[14] == '7' && value[18] == '-' && value[23] == '-';
    }

    bool fixtureMatrixIsCompleteAndDeterministic() {
        const Json matrix = readJson(root() / "assets" / "benchmarks" / "m6" /
            "fixture-matrix.v1.json");
        CHECK(matrix.at("schema_version") == 1);
        CHECK(matrix.at("milestone") == "M6.0");
        CHECK(matrix.at("color_domain") == "scene_linear_acescg_ap1");
        CHECK(matrix.at("world_units") == "metres");
        CHECK(matrix.at("production_resolution") == Json::array({ 3840, 2160 }));
        CHECK(matrix.at("identity_policy").at("seed").is_number_unsigned());
        CHECK(matrix.at("identity_policy").at("connected_component_order") ==
            "lowest_source_triangle_index");
        CHECK(matrix.at("quality_policy").at("fixture_light_shadow_quality") ==
            "Ultra");
        CHECK(matrix.at("quality_policy").at("pcss_blocker_samples") == 24);
        CHECK(matrix.at("quality_policy").at("pcss_filter_samples") == 48);
        CHECK(matrix.at("quality_policy").at("layer_interface_tiers") ==
            Json::array({ 2, 4, 8 }));

        std::set<std::string> cameras;
        for (const Json& camera : matrix.at("cameras")) {
            CHECK(cameras.insert(camera.at("id").get<std::string>()).second);
            CHECK(camera.at("near_metres").get<double>() > 0.0);
            CHECK(camera.at("far_metres").get<double>() >
                camera.at("near_metres").get<double>());
        }
        CHECK(cameras.size() == 4);

        const std::set<std::string> requiredCases{
            "alfa_windows_headlamps_v1",
            "transparency_material_sweeps_v1",
            "sorted_disconnected_and_ties_v1",
            "sorted_intersection_cycle_v1",
            "thin_and_closed_topology_v1",
            "nested_shells_v1",
            "hero_layer_overflow_v1",
            "weighted_oit_particles_v1",
            "refraction_edges_and_discontinuities_v1",
            "transparency_motion_and_frame_bounds_v1",
            "transparent_shadow_contract_v1",
            "transparency_output_lifecycle_v1",
        };
        std::set<std::string> actualCases;
        std::set<std::string> guids;
        std::set<std::string> coveredAxes;
        for (const Json& fixture : matrix.at("cases")) {
            CHECK(actualCases.insert(fixture.at("id").get<std::string>()).second);
            const std::string guid = fixture.at("fixture_guid");
            CHECK(hasUuidV7Shape(guid));
            CHECK(guids.insert(guid).second);
            CHECK(cameras.contains(fixture.at("camera").get<std::string>()));
            CHECK(!fixture.at("classes").empty());
            CHECK(!fixture.at("axes").empty());
            CHECK(!fixture.at("expected_behavior").empty());
            for (const Json& axis : fixture.at("axes")) {
                coveredAxes.insert(axis.get<std::string>());
            }
        }
        CHECK(actualCases == requiredCases);

        std::set<std::string> requiredAxes;
        for (const Json& axis : matrix.at("required_axes")) {
            CHECK(requiredAxes.insert(axis.get<std::string>()).second);
        }
        CHECK(coveredAxes == requiredAxes);

        const auto sorted = std::ranges::find_if(matrix.at("cases"),
            [](const Json& value) {
                return value.at("id") == "sorted_disconnected_and_ties_v1";
            });
        CHECK(sorted != matrix.at("cases").end());
        CHECK(sorted->at("recipe").at("input_permutations") == 64);
        for (const Json& work : sorted->at("recipe").at("stable_work")) {
            CHECK(hasUuidV7Shape(work.at("entity_uuid").get<std::string>()));
            CHECK(hasUuidV7Shape(work.at("primitive_guid").get<std::string>()));
            CHECK(hasUuidV7Shape(work.at("material_guid").get<std::string>()));
        }

        CHECK(matrix.at("reference_artifacts").at("transport_math") ==
            "reference-transport.v1.json");
        return true;
    }

    double exactDielectricFresnel(double incidentIor, double transmittedIor,
        double incidentAngleDegrees) {
        const double incidentAngle = incidentAngleDegrees *
            std::numbers::pi / 180.0;
        const double sinTransmitted = incidentIor / transmittedIor *
            std::sin(incidentAngle);
        if (sinTransmitted >= 1.0) return 1.0;
        const double cosIncident = std::cos(incidentAngle);
        const double cosTransmitted = std::sqrt(
            std::max(0.0, 1.0 - sinTransmitted * sinTransmitted));
        const double rs = (incidentIor * cosIncident -
            transmittedIor * cosTransmitted) /
            (incidentIor * cosIncident + transmittedIor * cosTransmitted);
        const double rp = (incidentIor * cosTransmitted -
            transmittedIor * cosIncident) /
            (incidentIor * cosTransmitted + transmittedIor * cosIncident);
        return 0.5 * (rs * rs + rp * rp);
    }

    bool transportReferenceIsAnalyticAndFinite() {
        const Json reference = readJson(root() / "assets" / "benchmarks" / "m6" /
            "reference-transport.v1.json");
        CHECK(reference.at("schema_version") == 1);
        CHECK(reference.at("units").at("distance") == "metres");
        for (const Json& fixture : reference.at("fresnel_dielectric")) {
            const double n1 = fixture.at("incident_ior");
            const double n2 = fixture.at("transmitted_ior");
            const double angle = fixture.at("incident_angle_degrees");
            const double expected = fixture.at("expected_reflectance");
            CHECK(near(exactDielectricFresnel(n1, n2, angle), expected));
            const double critical = n1 > n2
                ? std::asin(n2 / n1) * 180.0 / std::numbers::pi
                : 90.0;
            const bool tir = angle > critical;
            CHECK(tir == fixture.at("total_internal_reflection").get<bool>());
            if (fixture.contains("critical_angle_degrees")) {
                CHECK(near(critical,
                    fixture.at("critical_angle_degrees").get<double>()));
            }
            if (fixture.contains("expected_transmitted_angle_degrees")) {
                const double transmitted = std::asin(n1 / n2 *
                    std::sin(angle * std::numbers::pi / 180.0)) *
                    180.0 / std::numbers::pi;
                CHECK(near(transmitted, fixture.at(
                    "expected_transmitted_angle_degrees").get<double>()));
            }
        }

        for (const Json& fixture : reference.at("beer_lambert")) {
            const double path = fixture.at("path_length_metres");
            const Json& color = fixture.at("attenuation_color");
            const Json& expected = fixture.at("expected_transmittance");
            const bool infinite = fixture.at("attenuation_distance_metres")
                .is_string();
            const double distance = infinite ? 1.0 :
                fixture.at("attenuation_distance_metres").get<double>();
            for (size_t channel = 0; channel < 3; ++channel) {
                const double actual = infinite ? 1.0 :
                    std::pow(color.at(channel).get<double>(), path / distance);
                CHECK(near(actual, expected.at(channel).get<double>()));
            }
        }

        const Json& thin = reference.at("thin_glass").at(0);
        CHECK(thin.at("sheet_thickness_metres") == 0.0);
        CHECK(thin.at("expected_absorption_path_metres") == 0.0);
        CHECK(thin.at("expected_lateral_screen_displacement") == 0.0);
        CHECK(thin.at("fresnel_and_transmission_remain_active") == true);
        return true;
    }

    bool runContractIsExplicitAndLocalInputsAreVerifiedWhenPresent() {
        const Json manifest = readJson(root() / "assets" / "benchmarks" / "m6" /
            "run-manifest.v1.json");
        CHECK(manifest.at("schema_version") == 1);
        CHECK(manifest.at("milestone") == "M6.5");
        CHECK(manifest.at("resolution") == Json::array({ 3840, 2160 }));
        CHECK(manifest.at("warmup_frames") == 500);
        CHECK(manifest.at("measured_frames") == 10000);
        CHECK(manifest.at("independent_processes") == 5);
        CHECK(manifest.at("statistics") ==
            Json::array({ "median", "p95", "p99" }));
        const Json& quality = manifest.at("quality_contract");
        CHECK(quality.at("fixture_light_shadow_quality") == "Ultra");
        CHECK(quality.at("pcss_blocker_samples") == 24);
        CHECK(quality.at("pcss_filter_samples") == 48);
        CHECK(quality.at("local_light_emission_axis") == "+Z");
        CHECK(quality.at("compatibility_path") ==
            "ClassifiedSortedSurfaceWithLegacyFallback");

        for (const Json& input : manifest.at("optional_local_inputs")) {
            const auto path = root() / input.at("path").get<std::string>();
            if (!std::filesystem::is_regular_file(path)) continue;
            CHECK(Iridium::sha256File(path) ==
                input.at("sha256").get<std::string>());
        }

        std::set<std::string> runs;
        for (const Json& run : manifest.at("required_runs")) {
            CHECK(runs.insert(run.at("id").get<std::string>()).second);
            CHECK(!run.at("required_artifacts").empty());
        }
        CHECK(runs.size() == 12);
        CHECK(runs.contains("m6_5_debug_populated_resize_validation_v1"));
        CHECK(runs.contains("m6_5_release_4k_populated_five_process_v1"));
        CHECK(manifest.at("required_counters").size() >= 15);
        CHECK(manifest.at("required_gpu_ranges").size() >= 12);
        CHECK(manifest.at("current_source_baseline_before_fixture_correction")
            .at("status") == "measured_effective_high_not_acceptance_baseline");
        CHECK(manifest.at("corrected_ultra_baseline").at("status") ==
            "accepted_with_rejected_nonstationary_runs");
        CHECK(manifest.at("corrected_ultra_baseline").at("accepted_profiles").size() == 5);
        CHECK(manifest.at("corrected_ultra_baseline").at("rejected_profiles").size() == 2);
        const Json& sorted = manifest.at("m6_2_sorted_surface_qualification");
        CHECK(sorted.at("status") ==
            "accepted_with_rejected_nonstationary_runs");
        CHECK(sorted.at("accepted_profiles").size() == 5);
        CHECK(sorted.at("rejected_profiles").size() == 3);
        CHECK(sorted.at("validation_process_pairs_identical") == true);
        CHECK(sorted.at("validation_message_count") == 0);
        CHECK(sorted.at("results").at(
            "transparent_sorted_packets") == 55);
        CHECK(sorted.at("results").at(
            "transparent_compatibility_fallback_packets") == 6);
        CHECK(sorted.at("results").at(
            "render_graph_allocation_delta_bytes") == 0);
        CHECK(sorted.at("results").at(
            "cpp_allocation_calls_median") == 39);
        CHECK(sorted.at("results").at(
            "cpp_requested_bytes_median") == 5288);
        const Json& metric = manifest.at(
            "m6_3_metric_transport_qualification");
        CHECK(metric.at("status") ==
            "accepted_with_rejected_nonstationary_or_over_budget_runs");
        CHECK(metric.at("accepted_profiles").size() == 5);
        CHECK(metric.at("rejected_profiles").size() == 5);
        CHECK(metric.at("validation_final_sdr_processes_identical") == true);
        CHECK(metric.at("validation_message_count") == 0);
        CHECK(metric.at("visual_comparison").at(
            "candidate_scene_linear_finite") == true);
        CHECK(metric.at("visual_comparison").at(
            "candidate_hdr10_finite_nonnegative") == true);
        CHECK(metric.at("results").at(
            "transparent_thin_glass_packets") == 6);
        CHECK(metric.at("results").at(
            "transparent_zero_sheet_thickness_packets") == 6);
        CHECK(metric.at("results").at(
            "ordinary_transparency_summed_median_of_run_medians_ms") < 1.0);
        CHECK(metric.at("results").at(
            "render_graph_allocation_delta_bytes") == 0);
        CHECK(metric.at("results").at(
            "requested_live_delta_from_m6_2_bytes") == 384);
        CHECK(metric.at("results").at(
            "cpp_allocation_calls_median") == 39);
        const Json& pyramid = manifest.at("m6_4_pyramid_candidate");
        CHECK(pyramid.at("status") == "complete");
        CHECK(pyramid.at("validation_scene_linear_processes_identical") == true);
        CHECK(pyramid.at("validation_message_count") == 0);
        CHECK(pyramid.at("results").at(
            "pyramid_builds_per_active_frame") == 1);
        CHECK(pyramid.at("results").at(
            "pyramid_mip_dispatches_at_4k") == 12);
        CHECK(pyramid.at("results").at(
            "ordinary_transparency_summed_median_of_medians_ms") < 1.0);
        CHECK(pyramid.at("visual_comparison").at(
            "zero_to_metric_final_sdr_changed_pixel_fraction_at_code_threshold_1") >
            0.0);
        const Json& residency = pyramid.at(
            "conditional_residency_validation");
        CHECK(residency.at("inactive_topology").at(
            "pyramid_resident") == 0);
        CHECK(residency.at("inactive_topology").at(
            "pyramid_builds") == 0);
        CHECK(residency.at("active_topology").at(
            "pyramid_resident") == 1);
        CHECK(residency.at("active_topology").at(
            "pyramid_builds") == 1);
        CHECK(residency.at("suppressed_requested_bytes") > 0);
        CHECK(residency.at("suppressed_committed_bytes") > 0);
        CHECK(residency.at("validation_message_count") == 0);
        const Json& output = pyramid.at("output_transport_validation");
        CHECK(output.at("scrgb").at("finite") == true);
        CHECK(output.at("hdr10").at("finite") == true);
        CHECK(output.at("hdr10").at("nonnegative") == true);
        CHECK(output.at("hdr10").at("hdr_metadata_applied") == true);
        CHECK(output.at("pyramid_builds_per_frame") == 1);
        CHECK(output.at("pyramid_mip_dispatches") == 12);
        CHECK(output.at("validation_message_count") == 0);
        const Json& lifecycle = pyramid.at(
            "window_lifecycle_validation");
        CHECK(lifecycle.at("independent_processes") == 2);
        CHECK(lifecycle.at("measured_frames_per_process") == 10000);
        CHECK(lifecycle.at("completed_window_cycles").at(0) == 8);
        CHECK(lifecycle.at("confirmed_move_successes_in_timed_process") == 9);
        CHECK(lifecycle.at("process_exit_codes").at(0) == 0);
        CHECK(lifecycle.at("process_exit_codes").at(1) == 0);
        CHECK(lifecycle.at("validation_message_count") == 0);
        CHECK(lifecycle.at("topology_rebuild_failures") == 0);
        CHECK(lifecycle.at("dropped_frames") == 0);
        const Json& qualification = pyramid.at(
            "five_process_release_qualification");
        CHECK(qualification.at("independent_processes") == 5);
        CHECK(qualification.at("total_measured_frames") == 50000);
        CHECK(qualification.at(
            "ordinary_transparency_summed_median_of_medians_ms") <
            pyramid.at("results").at("ordinary_transparency_budget_ms"));
        CHECK(qualification.at("gpu_frame_median_of_medians_ms") < 10.0);
        CHECK(qualification.at("pyramid_builds_per_active_frame") == 1);
        CHECK(qualification.at("pyramid_mip_dispatches_per_active_frame") == 12);
        CHECK(qualification.at("topology_rebuild_failures") == 0);
        CHECK(qualification.at("dropped_frames") == 0);
        CHECK(qualification.at("dropped_counters") == 0);
        CHECK(qualification.at("dropped_gpu_ranges") == 0);
        CHECK(qualification.at("dropped_cpu_events") == 0);
        CHECK(qualification.at(
            "steady_state_memory_identical_across_processes") == true);
        CHECK(qualification.at(
            "steady_state_counters_identical_across_processes") == true);
        CHECK(pyramid.at("open_gates").empty());
        const Json& layered = manifest.at("m6_5_layered_candidate");
        CHECK(layered.at("status") == "complete");
        CHECK(layered.at("importer_version") == 6);
        CHECK(layered.at("topology_contract").at(
            "requires_two_uses_per_edge") == true);
        CHECK(layered.at("topology_contract").at(
            "requires_opposite_edge_orientation") == true);
        CHECK(layered.at("topology_contract").at(
            "invalid_result") == "thin_glass_with_diagnostic");
        CHECK(layered.at("ordinary2_pairing_contract").at(
            "stored_interface_capacity") == 2);
        CHECK(layered.at("ordinary2_pairing_contract").at(
            "requires_same_stable_work_identity") == true);
        const Json& layeredGraph = layered.at("ordinary2_graph_contract");
        CHECK(layeredGraph.at("runtime_enabled") == true);
        CHECK(layeredGraph.at("conditional_default_absent") == true);
        CHECK(layeredGraph.at("atlas_tile_alignment_pixels") == 16);
        CHECK(layeredGraph.at("maximum_scene_area_fraction") == 0.25);
        CHECK(layeredGraph.at("interface_count") == 2);
        CHECK(layeredGraph.at("depth_format") == "D32_SFLOAT");
        CHECK(layeredGraph.at("identity_format") == "R32_UINT");
        CHECK(layeredGraph.at("graph_resources").size() == 5);
        CHECK(layeredGraph.at("graph_passes").size() == 5);
        CHECK(layeredGraph.at(
            "local_composition_pass_execution_enabled") == true);
        CHECK(layeredGraph.at(
            "scene_composition_pass_execution_enabled") == true);
        CHECK(layeredGraph.at("identity_clear_value") == 0);
        CHECK(layeredGraph.at("identity_work_table_index_bits") == 31);
        CHECK(layeredGraph.at("identity_orientation_bit") == 31);
        const Json& layeredWork = layered.at(
            "ordinary2_work_table_contract");
        CHECK(layeredWork.at("maximum_unique_work_items") == 4096);
        CHECK(layeredWork.at("steady_dynamic_allocations") == 0);
        CHECK(layeredWork.at("stable_identity_bytes_per_item") == 64);
        CHECK(layeredWork.at("maximum_load_factor") == 0.5);
        CHECK(layeredWork.at("reset") ==
            "clear_previous_frame_touched_slots_only");
        CHECK(layeredWork.at("diagnostics").size() == 5);
        const Json& atlasPreparation = layered.at(
            "ordinary2_atlas_preparation_contract");
        CHECK(atlasPreparation.at("runtime_enabled") == true);
        CHECK(atlasPreparation.at("maximum_request_count") == 4096);
        CHECK(atlasPreparation.at("steady_dynamic_allocations") == 0);
        CHECK(atlasPreparation.at("allocation_probe_iterations") == 100);
        CHECK(atlasPreparation.at("allocation_probe_calls") == 0);
        CHECK(atlasPreparation.at("allocation_probe_bytes") == 0);
        CHECK(atlasPreparation.at("tile_alignment_pixels") == 16);
        CHECK(atlasPreparation.at("capacity_extent_3840x2160").at(0) ==
            3840);
        CHECK(atlasPreparation.at("capacity_extent_3840x2160").at(1) ==
            528);
        CHECK(atlasPreparation.at(
            "topology_extent_stable_across_content_changes") == true);
        CHECK(atlasPreparation.at("request_order_independent") == true);
        CHECK(atlasPreparation.at("fallback_results").size() == 4);
        CHECK(atlasPreparation.at("viewport_translation_range").at(0) ==
            -32768);
        CHECK(atlasPreparation.at("viewport_translation_range").at(1) ==
            32767);
        CHECK(atlasPreparation.at("push_constant_bytes") == 80);
        const Json& projectionRequests = layered.at(
            "ordinary2_projection_request_contract");
        CHECK(projectionRequests.at("runtime_enabled") == true);
        CHECK(projectionRequests.at("collection_mode") ==
            "ordinary2_topology_active_or_open_profiler_frames");
        CHECK(projectionRequests.at("gpu_work_enabled") == true);
        CHECK(projectionRequests.at(
            "graph_topology_activation_enabled") == true);
        CHECK(projectionRequests.at("integer_guard_band_pixels") == 1);
        CHECK(projectionRequests.at("maximum_retained_requests") == 4096);
        CHECK(projectionRequests.at("request_storage") ==
            "fixed_array_heap");
        CHECK(projectionRequests.at("steady_dynamic_allocations") == 0);
        CHECK(projectionRequests.at("allocation_probe_iterations") == 100);
        CHECK(projectionRequests.at("allocation_probe_calls") == 0);
        CHECK(projectionRequests.at("allocation_probe_bytes") == 0);
        CHECK(projectionRequests.at("profiler_counters").size() == 16);
        CHECK(projectionRequests.at("default_graph_memory_delta_bytes") == 0);
        CHECK(projectionRequests.at("default_gpu_work_delta") == 0);
        const Json& projectionProbe = projectionRequests.at(
            "debug_validation_probe");
        CHECK(projectionProbe.at("measured_frames") == 2);
        CHECK(projectionProbe.at("compatibility_packets_per_frame") == 61);
        CHECK(projectionProbe.at("layered_candidates_per_frame") == 0);
        CHECK(projectionProbe.at("ordinary2_counter_count") == 16);
        CHECK(projectionProbe.at("dropped_frames") == 0);
        CHECK(projectionProbe.at("dropped_counters") == 0);
        CHECK(projectionProbe.at("validation_message_count") == 0);
        const Json& observedTransition = projectionRequests.at(
            "observed_existing_topology_transition");
        CHECK(observedTransition.at("ordinary2_caused") == false);
        CHECK(observedTransition.at("total_ms") > 60.0);
        const Json& captureDraws = layered.at(
            "ordinary2_capture_draw_contract");
        CHECK(captureDraws.at("runtime_enabled") == true);
        CHECK(captureDraws.at("command_recorder_compiled") == true);
        CHECK(captureDraws.at(
            "command_recorder_invoked_by_live_forward_path") == true);
        CHECK(captureDraws.at("maximum_prepared_draws") == 4096);
        CHECK(captureDraws.at("steady_dynamic_allocations") == 0);
        CHECK(captureDraws.at("allocation_probe_iterations") == 100);
        CHECK(captureDraws.at("allocation_probe_calls") == 0);
        CHECK(captureDraws.at("allocation_probe_bytes") == 0);
        CHECK(captureDraws.at("entry_exit_draw_order_identical") == true);
        CHECK(captureDraws.at("capture_order").at(0) == "entry");
        CHECK(captureDraws.at("capture_order").at(1) == "exit");
        CHECK(captureDraws.at("descriptor_sets").size() == 4);
        CHECK(captureDraws.at("push_fields").size() == 5);
        CHECK(captureDraws.at("default_graph_memory_delta_bytes") == 0);
        CHECK(captureDraws.at("default_gpu_work_delta") == 0);
        CHECK(captureDraws.at(
            "visible_scene_color_uses_compatibility_path") == false);
        CHECK(captureDraws.at("composition_hook_recorded") == true);
        CHECK(captureDraws.at(
            "accepted_packet_compatibility_draw_suppression") == true);
        CHECK(captureDraws.at(
            "rejected_packet_compatibility_fallback_retained") == true);
        const Json& captureShader = layered.at(
            "ordinary2_capture_shader_contract");
        CHECK(captureShader.at("runtime_enabled") == true);
        CHECK(captureShader.at("compiled_and_spirv_validated") == true);
        CHECK(captureShader.at("output") ==
            "R32_UINT packed work-orientation identity");
        CHECK(captureShader.at("push_constant_bytes") == 80);
        CHECK(captureShader.at("push_constant_growth_bytes") == 0);
        CHECK(captureShader.at("base_color_texture_alpha_coverage") == true);
        CHECK(captureShader.at("alpha_mask_cutoff_evaluated") == true);
        CHECK(captureShader.at("opaque_depth_occlusion_rejected") == true);
        CHECK(captureShader.at(
            "mirrored_semantic_orientation_corrected") == true);
        CHECK(captureShader.at("exit_requires_same_work_entry") == true);
        CHECK(captureShader.at("exit_requires_strictly_deeper_depth") == true);
        CHECK(captureShader.at("cpu_acceptance_reference") == true);
        CHECK(captureShader.at(
            "atlas_to_scene_viewport_offset_signed_16_bit_xy") == true);
        CHECK(captureShader.at("vulkan_render_pass_created") == true);
        CHECK(captureShader.at("vulkan_attachment_order").size() == 2);
        CHECK(captureShader.at("frame_context_images_graph_owned") == true);
        CHECK(captureShader.at(
            "entry_and_exit_framebuffers_created_with_topology") == true);
        CHECK(captureShader.at(
            "capture_descriptor_sets_per_frame_context") == 2);
        CHECK(captureShader.at("capture_descriptor_bindings").size() == 3);
        CHECK(captureShader.at(
            "pipeline_precreated_during_renderer_initialization") == true);
        CHECK(captureShader.at(
            "pipeline_lazy_creation_during_frame") == false);
        CHECK(captureShader.at("driver_pipeline_creation_validated") == true);
        CHECK(captureShader.at("driver_validation_run").at(
            "validation_message_count") == 0);
        CHECK(captureShader.at("driver_validation_run").at(
            "exit_code") == 0);
        CHECK(captureShader.at("runtime_draw_execution_enabled") == true);
        const Json& framePacing = layered.at("frame_pacing_observation");
        CHECK(framePacing.at("status") ==
            "predictable_startup_transition_moved_out_of_frames_dynamic_transition_retained");
        CHECK(framePacing.at("existing_inactive_hysteresis_frames") == 120);
        CHECK(framePacing.at("residency_policy_changed") == false);
        CHECK(framePacing.at("profiler_ranges").size() == 4);
        CHECK(layered.at("cook_validation").at(
            "valid_closed_resolved_layered_glass") == 2);
        CHECK(layered.at("cook_validation").at(
            "invalid_open_resolved_thin_glass") == 2);
        CHECK(layered.at("alfa_import_validation").at(
            "diagnostic_count") == 0);
        CHECK(layered.at("alfa_import_validation").at(
            "artifact_decoded") == true);
        CHECK(layered.at("alfa_import_validation").at(
            "primitive_count") == 174);
        CHECK(layered.at("alfa_import_validation").at(
            "root_and_subasset_guids_preserved") == true);
        const Json& liveCapture = layered.at(
            "ordinary2_live_capture_validation");
        CHECK(liveCapture.at("atlas_resident") == true);
        CHECK(liveCapture.at("atlas_extent") == Json::array({ 1280, 176 }));
        CHECK(liveCapture.at("measured_frames") == 4);
        CHECK(liveCapture.at("per_frame_counters").at(
            "candidate_packets") == 1);
        CHECK(liveCapture.at("per_frame_counters").at(
            "atlas_accepted_packets") == 1);
        CHECK(liveCapture.at("per_frame_counters").at(
            "capture_prepared_draws") == 1);
        CHECK(liveCapture.at("per_frame_counters").at(
            "capture_entry_draws") == 1);
        CHECK(liveCapture.at("per_frame_counters").at(
            "capture_exit_draws") == 1);
        CHECK(liveCapture.at("per_frame_counters").at(
            "fallback_packets") == 0);
        CHECK(liveCapture.at("validation_message_count") == 0);
        CHECK(liveCapture.at("dropped_frames") == 0);
        CHECK(liveCapture.at("dropped_counters") == 0);
        CHECK(liveCapture.at("dropped_gpu_ranges") == 0);
        CHECK(liveCapture.at("dropped_cpu_events") == 0);
        CHECK(liveCapture.at("visible_scene_color_uses_compatibility_path") ==
            true);
        CHECK(liveCapture.at("composition_pass_execution_enabled") == false);
        const Json& releaseSample = liveCapture.at(
            "native_4k_release_sample");
        CHECK(releaseSample.at("measured_frames") == 1000);
        CHECK(releaseSample.at("atlas_extent") ==
            Json::array({ 3840, 528 }));
        CHECK(releaseSample.at(
            "candidate_projected_accepted_prepared_entry_exit_per_frame") ==
            1);
        CHECK(releaseSample.at(
            "fallback_or_atlas_rejection_packets_per_frame") == 0);
        CHECK(releaseSample.at("topology_rebuilds") == 0);
        CHECK(releaseSample.at("topology_rebuild_failures") == 0);
        CHECK(releaseSample.at("dropped_frames") == 0);
        CHECK(releaseSample.at("gpu_entry_capture_p95_us") < 20.0);
        CHECK(releaseSample.at("gpu_exit_capture_p95_us") < 20.0);
        const Json& pairingReadback = layered.at(
            "ordinary2_gpu_pairing_readback_validation");
        CHECK(pairingReadback.at("explicit_one_shot_only") == true);
        CHECK(pairingReadback.at("expected_draws") == 1);
        CHECK(pairingReadback.at("work_items") == 1);
        CHECK(pairingReadback.at("entry_pixels") > 0);
        CHECK(pairingReadback.at("exit_pixels") ==
            pairingReadback.at("paired_pixels"));
        CHECK(pairingReadback.at("invalid_work_index_pixels") == 0);
        CHECK(pairingReadback.at("invalid_orientation_pixels") == 0);
        CHECK(pairingReadback.at("unpaired_exit_pixels") == 0);
        CHECK(pairingReadback.at("work_mismatch_pixels") == 0);
        CHECK(pairingReadback.at("invalid_depth_pixels") == 0);
        CHECK(pairingReadback.at("non_increasing_depth_pixels") == 0);
        CHECK(pairingReadback.at("minimum_paired_depth_delta") > 0.0);
        CHECK(pairingReadback.at("readback_requested_bytes") == 3'604'480);
        CHECK(pairingReadback.at("validation_message_count") == 0);
        CHECK(pairingReadback.at("passed") == true);
        CHECK(pairingReadback.at("default_runtime_readback_peak_bytes") == 0);
        CHECK(pairingReadback.at("default_runtime_readback_range_count") == 0);
        CHECK(pairingReadback.at("default_runtime_gpu_work_delta") == 0);
        CHECK(pairingReadback.at(
            "default_runtime_live_memory_delta_bytes") == 0);
        const Json& compositionShader = layered.at(
            "ordinary2_measured_chord_material_shader_contract");
        CHECK(compositionShader.at("runtime_enabled") == true);
        CHECK(compositionShader.at("compiled_and_spirv_validated") == true);
        CHECK(compositionShader.at("material_model") ==
            "production complex-forward material body");
        CHECK(compositionShader.at("shared_features").size() == 6);
        CHECK(compositionShader.at("interface_validation").size() == 5);
        CHECK(compositionShader.at("push_constant_bytes") == 80);
        CHECK(compositionShader.at("push_constant_growth_bytes") == 0);
        CHECK(compositionShader.at("visible_scene_color_changed") == true);
        const Json& localComposition = layered.at(
            "ordinary2_local_composition_validation");
        CHECK(localComposition.at("runtime_enabled") == true);
        CHECK(localComposition.at("validation") == true);
        CHECK(localComposition.at("measured_frames") == 4);
        CHECK(localComposition.at("per_frame_local_composition_draws") == 1);
        CHECK(localComposition.at("gpu_local_composition_p95_us") < 30.0);
        CHECK(localComposition.at("graph_pass_count") == 27);
        CHECK(localComposition.at("graph_logical_resource_count") == 32);
        CHECK(localComposition.at("graph_physical_slot_count") == 25);
        CHECK(localComposition.at("graph_barrier_count") == 89);
        CHECK(localComposition.at(
            "requested_delta_from_capture_only_bytes") == 3'604'480);
        CHECK(localComposition.at(
            "expected_requested_local_atlas_bytes_for_two_frame_contexts") ==
            3'604'480);
        CHECK(localComposition.at("validation_message_count") == 0);
        CHECK(localComposition.at("dropped_frames") == 0);
        CHECK(localComposition.at("dropped_counters") == 0);
        CHECK(localComposition.at("dropped_gpu_ranges") == 0);
        CHECK(localComposition.at("nesting_errors") == 0);
        CHECK(localComposition.at(
            "visible_scene_color_uses_compatibility_path") == true);
        CHECK(localComposition.at(
            "scene_composition_pass_execution_enabled") == false);
        const Json& localRelease = localComposition.at(
            "native_4k_release_sample");
        CHECK(localRelease.at("measured_frames") == 1000);
        CHECK(localRelease.at("atlas_extent") ==
            Json::array({ 3840, 528 }));
        CHECK(localRelease.at("per_frame_local_composition_draws") == 1);
        CHECK(localRelease.at("gpu_local_composition_p95_us") < 100.0);
        CHECK(localRelease.at("gpu_frame_p95_ms") < 1.0);
        CHECK(localRelease.at(
            "requested_delta_from_capture_only_bytes") == 32'440'320);
        CHECK(localRelease.at(
            "expected_requested_local_atlas_bytes_for_two_frame_contexts") ==
            32'440'320);
        CHECK(localRelease.at(
            "topology_rebuilds_during_measured_frames") == 0);
        CHECK(localRelease.at("dropped_frames") == 0);
        CHECK(localRelease.at("dropped_counters") == 0);
        CHECK(localRelease.at("dropped_gpu_ranges") == 0);
        const Json& sceneResolve = layered.at(
            "ordinary2_scene_resolve_validation");
        CHECK(sceneResolve.at("runtime_enabled") == true);
        CHECK(sceneResolve.at(
            "accepted_packet_compatibility_draw_suppression") == true);
        CHECK(sceneResolve.at("steady_dynamic_allocations") == 0);
        const Json& resolveDebug = sceneResolve.at("debug_validation");
        CHECK(resolveDebug.at("validation") == true);
        CHECK(resolveDebug.at("measured_frames") == 4);
        CHECK(resolveDebug.at("paired_pixels") ==
            resolveDebug.at("local_color_pixels"));
        CHECK(resolveDebug.at("local_color_invalid_pixels") == 0);
        CHECK(resolveDebug.at("one_shot_readback_bytes") == 5'406'720);
        CHECK(resolveDebug.at("per_frame_scene_resolve_draws") == 1);
        CHECK(resolveDebug.at(
            "per_frame_compatibility_forward_draws") == 0);
        CHECK(resolveDebug.at(
            "per_frame_compatibility_background_packets") == 0);
        CHECK(resolveDebug.at(
            "per_frame_compatibility_foreground_packets") == 0);
        CHECK(resolveDebug.at("gpu_scene_resolve_p95_us") < 10.0);
        CHECK(resolveDebug.at("validation_message_count") == 0);
        CHECK(resolveDebug.at("dropped_frames") == 0);
        const Json& rejectedResolveRelease = sceneResolve.at(
            "rejected_native_4k_release_sample");
        CHECK(rejectedResolveRelease.at("measured_frames") == 1000);
        CHECK(rejectedResolveRelease.at(
            "gpu_utilization_observed_after_run_percent") >= 90);
        CHECK(rejectedResolveRelease.at(
            "per_frame_scene_resolve_draws") == 1);
        CHECK(rejectedResolveRelease.at(
            "per_frame_compatibility_forward_draws") == 0);
        const Json& runtimeTransform = layered.at(
            "ordinary2_runtime_transform_validation");
        CHECK(runtimeTransform.at("runtime_enabled") == true);
        CHECK(runtimeTransform.at("normal_instance_scale") ==
            Json::array({ 1.0, 1.0, 1.0 }));
        CHECK(runtimeTransform.at("mirrored_instance_scale") ==
            Json::array({ -1.0, 1.0, 1.0 }));
        CHECK(runtimeTransform.at("performance_qualification") == false);
        const Json& normalTransform = runtimeTransform.at("normal");
        const Json& mirroredTransform = runtimeTransform.at("mirrored");
        CHECK(normalTransform.at("entry_pixels") > 0);
        CHECK(normalTransform.at("entry_pixels") ==
            normalTransform.at("paired_pixels"));
        CHECK(mirroredTransform.at("entry_pixels") ==
            normalTransform.at("entry_pixels"));
        CHECK(mirroredTransform.at("paired_pixels") ==
            normalTransform.at("paired_pixels"));
        CHECK(normalTransform.at("invalid_orientation_pixels") == 0);
        CHECK(mirroredTransform.at("invalid_orientation_pixels") == 0);
        CHECK(normalTransform.at("non_increasing_depth_pixels") == 0);
        CHECK(mirroredTransform.at("non_increasing_depth_pixels") == 0);
        CHECK(normalTransform.at("local_color_invalid_pixels") == 0);
        CHECK(mirroredTransform.at("local_color_invalid_pixels") == 0);
        CHECK(normalTransform.at("passed") == true);
        CHECK(mirroredTransform.at("passed") == true);
        CHECK(runtimeTransform.at(
            "normal_and_mirrored_capture_sha_identical") == true);
        const Json& mirroredRelease = runtimeTransform.at(
            "release_mirrored_confirmation");
        CHECK(mirroredRelease.at("configuration") == "Release");
        CHECK(mirroredRelease.at("validation") == true);
        CHECK(mirroredRelease.at("paired_pixels") ==
            mirroredRelease.at("entry_pixels"));
        CHECK(mirroredRelease.at("invalid_orientation_pixels") == 0);
        CHECK(mirroredRelease.at("compatibility_forward_draws_per_frame") ==
            0);
        CHECK(mirroredRelease.at("passed") == true);
        CHECK(std::filesystem::exists(root() / runtimeTransform.at(
            "benchmark_manifest").get<std::string>()));
        const Json& invalidTopology = layered.at(
            "ordinary2_invalid_topology_runtime_validation");
        CHECK(invalidTopology.at("runtime_enabled") == true);
        CHECK(invalidTopology.at("cook_diagnostic_contract") ==
            "GLTF_TRANSPARENCY_LAYERED_TOPOLOGY_INVALID");
        CHECK(invalidTopology.at("validation_option") ==
            "--validate-ordinary2-fallback");
        const Json& fallbackContract = invalidTopology.at(
            "validation_contract");
        CHECK(fallbackContract.at("transparent_submeshes") == 1);
        CHECK(fallbackContract.at("requested_layered_candidates") == 1);
        CHECK(fallbackContract.at("fallback_thin_glass_submeshes") == 1);
        CHECK(fallbackContract.at("fallback_flagged_submeshes") == 1);
        CHECK(fallbackContract.at("topology_required_submeshes") == 1);
        CHECK(fallbackContract.at("layered_glass_submeshes") == 0);
        CHECK(fallbackContract.at("ordinary2_atlas_resident") == false);
        CHECK(fallbackContract.at("ordinary2_atlas_extent") ==
            Json::array({ 0, 0 }));
        CHECK(fallbackContract.at("refraction_pyramids_resident") == true);
        CHECK(fallbackContract.at("passed") == true);
        const Json& fallbackCounters = invalidTopology.at(
            "per_frame_counters");
        CHECK(fallbackCounters.at("thin_glass_packets") == 1);
        CHECK(fallbackCounters.at("layered_glass_packets") == 0);
        CHECK(fallbackCounters.at("compatibility_forward_draws") == 1);
        CHECK(fallbackCounters.at("ordinary2_candidates") == 0);
        CHECK(fallbackCounters.at(
            "ordinary2_atlas_accepted_packets") == 0);
        CHECK(fallbackCounters.at("ordinary2_capture_entry_draws") == 0);
        CHECK(fallbackCounters.at(
            "ordinary2_local_composition_draws") == 0);
        CHECK(fallbackCounters.at("ordinary2_scene_resolve_draws") == 0);
        CHECK(invalidTopology.at("debug").at("validation") == true);
        CHECK(invalidTopology.at("release").at("validation") == true);
        CHECK(invalidTopology.at("debug").at("capture_sha256") ==
            invalidTopology.at("release").at("capture_sha256"));
        CHECK(invalidTopology.at("performance_qualification") == false);
        CHECK(std::filesystem::exists(root() / invalidTopology.at(
            "fixture").get<std::string>()));
        CHECK(std::filesystem::exists(root() / invalidTopology.at(
            "metadata").get<std::string>()));
        const Json& ordinary2Lifecycle = layered.at(
            "ordinary2_populated_resize_lifecycle_validation");
        CHECK(ordinary2Lifecycle.at("runtime_enabled") == true);
        CHECK(ordinary2Lifecycle.at("instance_grid") ==
            Json::array({ 4, 2, 1 }));
        CHECK(ordinary2Lifecycle.at("instance_count") == 8);
        CHECK(ordinary2Lifecycle.at("validation_option") ==
            "--validate-ordinary2-resize");
        CHECK(ordinary2Lifecycle.at("resize_sequence") == Json::array({
            Json::array({ 960, 540 }),
            Json::array({ 1600, 900 }),
            Json::array({ 1280, 720 }) }));
        const Json& resizeContract = ordinary2Lifecycle.at("resize_contract");
        CHECK(resizeContract.at("requests") == 3);
        CHECK(resizeContract.at("successes") == 3);
        CHECK(resizeContract.at("failures") == 0);
        CHECK(resizeContract.at("render_graph_rebuild_delta") == 3);
        CHECK(resizeContract.at("ordinary2_atlas_resident") == true);
        CHECK(resizeContract.at("refraction_pyramids_resident") == true);
        CHECK(resizeContract.at("passed") == true);
        CHECK(ordinary2Lifecycle.at(
            "per_extent_steady_counters").size() == 3);
        for (const Json& extent :
            ordinary2Lifecycle.at("per_extent_steady_counters")) {
            CHECK(extent.at("ordinary2_candidates") == 8);
            CHECK(extent.at("ordinary2_accepted_packets") == 8);
            CHECK(extent.at("ordinary2_atlas_rejected_packets") == 0);
            CHECK(extent.at("entry_exit_local_resolve_draws_each") == 8);
            CHECK(extent.at("compatibility_background_packets") == 0);
            CHECK(extent.at("compatibility_foreground_packets") == 0);
        }
        const Json& lifecycleReadback = ordinary2Lifecycle.at(
            "post_restore_gpu_readback");
        CHECK(lifecycleReadback.at("expected_draws") == 8);
        CHECK(lifecycleReadback.at("entry_pixels") == 47070);
        CHECK(lifecycleReadback.at("paired_pixels") == 47070);
        CHECK(lifecycleReadback.at("invalid_work_index_pixels") == 0);
        CHECK(lifecycleReadback.at("invalid_orientation_pixels") == 0);
        CHECK(lifecycleReadback.at("non_increasing_depth_pixels") == 0);
        CHECK(lifecycleReadback.at("local_color_invalid_pixels") == 0);
        CHECK(lifecycleReadback.at("passed") == true);
        CHECK(ordinary2Lifecycle.at("debug").at("validation") == true);
        CHECK(ordinary2Lifecycle.at("release").at("validation") == true);
        CHECK(ordinary2Lifecycle.at("visual_comparison").at(
            "maximum_channel_delta_lsb") == 1);
        CHECK(ordinary2Lifecycle.at("performance_qualification") == false);
        CHECK(std::filesystem::exists(root() / ordinary2Lifecycle.at(
            "fixture").get<std::string>()));
        const Json& ordinary2Qualification = layered.at(
            "ordinary2_five_process_release_qualification");
        CHECK(ordinary2Qualification.at("build") == "x64-release");
        CHECK(ordinary2Qualification.at("extent") ==
            Json::array({ 3840, 2160 }));
        CHECK(ordinary2Qualification.at("independent_processes") == 5);
        CHECK(ordinary2Qualification.at("measured_frames_per_process") ==
            10000);
        CHECK(ordinary2Qualification.at("total_measured_frames") == 50000);
        CHECK(ordinary2Qualification.at("profiles").size() == 5);
        CHECK(ordinary2Qualification.at("profile_sha256").size() == 5);
        CHECK(ordinary2Qualification.at(
            "gpu_frame_median_of_medians_ms").get<double>() < 1.0);
        CHECK(ordinary2Qualification.at(
            "all_transparency_median_of_medians_ms").get<double>() < 0.9);
        CHECK(ordinary2Qualification.at(
            "ordinary2_candidates_per_frame") == 8);
        CHECK(ordinary2Qualification.at(
            "ordinary2_accepted_packets_per_frame") == 8);
        CHECK(ordinary2Qualification.at(
            "ordinary2_atlas_rejections_per_frame") == 0);
        CHECK(ordinary2Qualification.at("topology_rebuilds") == 0);
        CHECK(ordinary2Qualification.at("topology_rebuild_failures") == 0);
        CHECK(ordinary2Qualification.at("fallback_frames_after_warmup") == 0);
        CHECK(ordinary2Qualification.at("dropped_frames") == 0);
        CHECK(ordinary2Qualification.at(
            "steady_state_memory_identical_across_processes") == true);
        CHECK(ordinary2Qualification.at(
            "steady_state_counters_identical_across_processes") == true);
        CHECK(ordinary2Qualification.at("performance_qualification") == true);
        const Json& artistUi = layered.at(
            "artist_transparency_policy_ui_contract");
        CHECK(artistUi.at("stable_guid_targeted") == true);
        CHECK(artistUi.at("material_policy_inherited_by_primitives") == true);
        CHECK(artistUi.at("primitive_policy_overrides_material") == true);
        CHECK(artistUi.at("target_discovery").size() == 3);
        CHECK(artistUi.at("controls").size() == 7);
        CHECK(artistUi.at("layer_budgets").size() == 3);
        CHECK(artistUi.at("layer_budgets").at(0).at("value") ==
            "ordinary2");
        CHECK(artistUi.at("layer_budgets").at(0).at(
            "runtime_available") == true);
        CHECK(artistUi.at("layer_budgets").at(1).at("value") == "hero4");
        CHECK(artistUi.at("layer_budgets").at(1).at(
            "runtime_available") == true);
        CHECK(artistUi.at("layer_budgets").at(2).at("value") ==
            "cinematic8");
        CHECK(artistUi.at("layer_budgets").at(2).at(
            "runtime_available") == true);
        CHECK(artistUi.at("tooltip_topics").size() == 5);
        const Json fixtureMetadata = readJson(root() / "assets" /
            "benchmarks" / "m6" /
            "ordinary2_closed_tetrahedron.gltf.iridium.meta");
        CHECK(fixtureMetadata.at("assetGuid") ==
            liveCapture.at("asset_guid"));
        CHECK(fixtureMetadata.at("importer").at("version") == 6);
        CHECK(fixtureMetadata.at("settings").at("values").at(
            "transparency_execution_mode") == "classified");
        CHECK(std::filesystem::exists(root() / "assets" / "benchmarks" /
            "m6" / "ordinary2_closed_tetrahedron.gltf"));
        CHECK(layered.at("runtime_rendering_changed") == true);
        CHECK(layered.at("visible_scene_color_changed") == true);
        CHECK(layered.at("default_non_ordinary2_graph_memory_delta_bytes") ==
            0);
        CHECK(layered.at("open_gates").empty());
        const Json& layeredTiers = manifest.at(
            "m6_6_layered_tiers_candidate");
        CHECK(layeredTiers.at("status") ==
            "hero4_cinematic8_scene_resolve_and_residual_active");
        CHECK(layeredTiers.at("current_fixture_manifest_sha256") ==
            "273afc1f3a839bd69a636b6432c2b91fcd23444ea7183164f3362de43bc651d6");
        CHECK(layeredTiers.at("runtime_rendering_changed") == true);
        CHECK(layeredTiers.at("hero4_runtime_enabled") == true);
        CHECK(layeredTiers.at("cinematic8_runtime_enabled") == true);
        CHECK(layeredTiers.at("tier_contracts").size() == 3);
        CHECK(layeredTiers.at("tier_contracts").at(0).at(
            "maximum_interfaces") == 2);
        CHECK(layeredTiers.at("tier_contracts").at(1).at(
            "maximum_interfaces") == 4);
        CHECK(layeredTiers.at("tier_contracts").at(2).at(
            "maximum_interfaces") == 8);
        CHECK(layeredTiers.at("tier_contracts").at(0).at(
            "maximum_scene_area_fraction") == 0.25);
        CHECK(layeredTiers.at("tier_contracts").at(1).at(
            "maximum_scene_area_fraction") == 0.5);
        CHECK(layeredTiers.at("tier_contracts").at(2).at(
            "maximum_scene_area_fraction") == 1.0);
        const Json& reduction = layeredTiers.at(
            "interface_reduction_contract");
        CHECK(reduction.at("nested_closed_volumes") == true);
        CHECK(reduction.at("crossing_closed_volumes") == true);
        CHECK(reduction.at("silent_interface_loss") == false);
        CHECK(reduction.at("dynamic_allocation") == false);
        const Json& earlyTermination = layeredTiers.at(
            "early_termination_contract");
        CHECK(earlyTermination.at(
            "remaining_transmittance_threshold") == 0.0009765625);
        CHECK(earlyTermination.at("execution_active") == true);
        CHECK(earlyTermination.at("tile_extent_pixels") == 16);
        CHECK(earlyTermination.at("malformed_stack_terminates") == false);
        CHECK(earlyTermination.at("dynamic_allocation") == false);
        CHECK(layeredTiers.at("native_4k_area_caps_pixels").at(
            "ordinary2") == 2'073'600);
        CHECK(layeredTiers.at("native_4k_area_caps_pixels").at(
            "hero4") == 4'147'200);
        CHECK(layeredTiers.at("native_4k_area_caps_pixels").at(
            "cinematic8") == 8'294'400);
        const Json& atlas = layeredTiers.at(
            "atlas_preparation_contract");
        CHECK(atlas.at("storage_topology") ==
            "independent packed atlas per active quality tier");
        CHECK(atlas.at("tile_extent_pixels") == 16);
        CHECK(atlas.at("request_order_independent") == true);
        CHECK(atlas.at("optical_island_grouping") ==
            "transitive positive-area projected-screen overlap within one "
            "quality tier");
        CHECK(atlas.at("nested_or_intersecting_work") ==
            "share one packed optical-island rectangle while retaining "
            "unique work-table identities");
        CHECK(atlas.at("disjoint_or_edge_touching_work") ==
            "remain separate optical islands");
        CHECK(atlas.at("overlap_grouping_algorithm") ==
            "deterministic allocation-free union-find");
        CHECK(atlas.at("explicit_decision_per_in_capacity_request") == true);
        CHECK(atlas.at("dynamic_allocation") == false);
        CHECK(atlas.at("runtime_wiring_active") == true);
        CHECK(atlas.at("native_4k_capacity_extents").at(
            "ordinary2") == Json::array({ 3840, 528 }));
        CHECK(atlas.at("native_4k_capacity_extents").at(
            "hero4") == Json::array({ 3840, 1072 }));
        CHECK(atlas.at("native_4k_capacity_extents").at(
            "cinematic8") == Json::array({ 3840, 2160 }));
        const Json& indexedPeel = layeredTiers.at("indexed_peel_contract");
        CHECK(indexedPeel.at("work_identity_may_differ_from_previous") ==
            true);
        CHECK(indexedPeel.at("orientation_may_differ_from_previous") ==
            true);
        CHECK(indexedPeel.at(
            "missing_or_nonincreasing_previous_interface_rejected") == true);
        CHECK(indexedPeel.at(
            "ordinary2_requires_same_work_entry_exit_pair") == true);
        CHECK(indexedPeel.at("ordinary2_compatibility_flag") ==
            "RequirePairedOrientation");
        CHECK(indexedPeel.at("deep_capture_execution_active") == true);
        const Json& gpuStorage = layeredTiers.at("gpu_storage_contract");
        CHECK(gpuStorage.at("hero4_interface_products") == 4);
        CHECK(gpuStorage.at("cinematic8_interface_products") == 8);
        CHECK(gpuStorage.at("hero4_added_logical_resources") == 10);
        CHECK(gpuStorage.at("cinematic8_added_logical_resources") == 20);
        CHECK(gpuStorage.at("native_4k_logical_bytes_per_frame_context").at(
            "hero4") == 164'723'520);
        CHECK(gpuStorage.at("native_4k_logical_bytes_per_frame_context").at(
            "cinematic8") == 597'585'600);
        CHECK(gpuStorage.at(
            "hero4_termination_masks_per_frame_context") == 1);
        CHECK(gpuStorage.at(
            "cinematic8_termination_masks_per_frame_context") == 3);
        CHECK(gpuStorage.at("explicit_residency_request_available") == true);
        CHECK(gpuStorage.at("content_auto_activation") == true);
        CHECK(gpuStorage.at(
            "frame_targets_materialized_for_resident_tiers") == true);
        CHECK(gpuStorage.at(
            "per_interface_capture_framebuffers_materialized") == true);
        CHECK(gpuStorage.at(
            "per_tier_local_composition_framebuffer_materialized") == true);
        CHECK(gpuStorage.at(
            "previous_interface_descriptor_chain_materialized") == true);
        CHECK(gpuStorage.at(
            "hero4_capture_descriptor_sets_per_frame_context") == 4);
        CHECK(gpuStorage.at(
            "cinematic8_capture_descriptor_sets_per_frame_context") == 8);
        CHECK(gpuStorage.at(
            "incomplete_deep_target_chain_fails_topology_rebuild") == true);
        CHECK(gpuStorage.at(
            "descriptor_cleanup_precedes_target_cleanup") == true);
        CHECK(gpuStorage.at("tier_aware_capture_draw_plan_active") == true);
        CHECK(gpuStorage.at("hero4_capture_execution_active") == true);
        CHECK(gpuStorage.at("cinematic8_capture_execution_active") == true);
        CHECK(gpuStorage.at(
            "sequential_previous_interface_peeling") == true);
        CHECK(gpuStorage.at(
            "deep_packets_remain_on_compatibility_forward") == false);
        CHECK(gpuStorage.at(
            "accepted_hero4_packets_removed_from_compatibility_forward") ==
            true);
        CHECK(gpuStorage.at(
            "accepted_cinematic8_packets_removed_from_compatibility_forward") ==
            true);
        CHECK(gpuStorage.at(
            "rejected_deep_packets_remain_on_compatibility_forward") ==
            true);
        CHECK(gpuStorage.at("capture_gpu_range_prefixes").size() == 2);
        CHECK(gpuStorage.at("capture_counter_prefix") ==
            "transparent.layered.deep");
        CHECK(gpuStorage.at("capture_composition_execution_active") == true);
        const Json& deepLocalComposition = gpuStorage.at(
            "deep_local_composition_contract");
        CHECK(deepLocalComposition.at("execution_active") == true);
        CHECK(deepLocalComposition.at("fragment_shader") ==
            "assets/shaders/layered_deep_material_indexed.frag");
        CHECK(deepLocalComposition.at("fragment_shader_sha256") ==
            "f53e8a2d0418643251c77b3fe518666ba7f0d0afdd1b567dfe080acae6ba98cb");
        CHECK(deepLocalComposition.at("shared_material_body_sha256") ==
            "6cd9e77f0e304b5534537995e66ea44941f2ff427188193ebfc1ca672908fa7e");
        CHECK(deepLocalComposition.at("descriptor_capacity_interfaces") == 8);
        CHECK(deepLocalComposition.at("hero4_published_interfaces") == 4);
        CHECK(deepLocalComposition.at("cinematic8_published_interfaces") == 8);
        CHECK(deepLocalComposition.at("blend") ==
            "premultiplied ONE plus ONE_MINUS_SRC_ALPHA");
        CHECK(deepLocalComposition.at(
            "scene_resolve_execution_active") == true);
        CHECK(deepLocalComposition.at(
            "hero4_scene_resolve_execution_active") == true);
        CHECK(deepLocalComposition.at(
            "cinematic8_scene_resolve_execution_active") == true);
        CHECK(deepLocalComposition.at(
            "mixed_tier_global_order_active") == true);
        CHECK(deepLocalComposition.at(
            "visible_scene_color_uses_compatibility_path") == false);
        const Json& residual = deepLocalComposition.at(
            "bounded_residual_tail");
        CHECK(residual.at("execution_active") == true);
        CHECK(residual.at("fragment_shader") ==
            "assets/shaders/layered_deep_residual_material_indexed.frag");
        CHECK(residual.at("fragment_shader_sha256") ==
            "ea188611fa11f52133b2440f0f4b9c93439e0329193ba5156d7e9484107f6ca3");
        CHECK(residual.at("probe_draw_counter") ==
            "transparent.layered.deep.residual.probe_draws");
        CHECK(residual.at("hero4_sample_counter") ==
            "transparent.layered.hero4.residual_samples");
        CHECK(residual.at("cinematic8_sample_counter") ==
            "transparent.layered.cinematic8.residual_samples");
        CHECK(residual.at("steady_frame_dynamic_allocation") == false);
        CHECK(residual.at("additional_graph_resources") == 0);
        CHECK(residual.at("additional_graph_passes") == 0);
        CHECK(deepLocalComposition.at(
            "vulkan_visual_performance_qualified") == false);
        const Json& deepValidation = deepLocalComposition.at(
            "gpu_capture_validation");
        CHECK(deepValidation.at("passed") == true);
        CHECK(deepValidation.at("fixture") ==
            "hero4_nested_tetrahedra_v1");
        CHECK(deepValidation.at("maximum_observed_interfaces") == 4);
        CHECK(deepValidation.at("scene_resolve_draws") == 2);
        CHECK(deepValidation.at("compatibility_forward_draws") == 0);
        CHECK(deepValidation.at("interface_pixels") ==
            Json::array({ 10'922, 10'922, 3'890, 3'890 }));
        CHECK(deepValidation.at("paired_pixels") == 10'922);
        CHECK(deepValidation.at("nested_four_interface_pixels") == 3'890);
        CHECK(deepValidation.at("invalid_pixels") == 0);
        CHECK(deepValidation.at("local_color_pixels") == 10'922);
        CHECK(deepValidation.at("local_color_invalid_pixels") == 0);
        CHECK(deepValidation.at("validation_messages") == 0);
        CHECK(deepValidation.at("performance_evidence") == false);
        CHECK(deepValidation.at("visible_capture").at(
            "visually_inspected") == true);
        const Json& crossingValidation = deepLocalComposition.at(
            "hero4_crossing_gpu_capture_validation");
        CHECK(crossingValidation.at("passed") == true);
        CHECK(crossingValidation.at("fixture") ==
            "hero4_crossing_tetrahedra_v1");
        CHECK(crossingValidation.at("maximum_observed_interfaces") == 4);
        CHECK(crossingValidation.at("scene_resolve_draws") == 2);
        CHECK(crossingValidation.at("compatibility_forward_draws") == 0);
        CHECK(crossingValidation.at("interface_pixels") ==
            Json::array({ 14'211, 14'211, 7'706, 7'706 }));
        CHECK(crossingValidation.at("paired_pixels") == 14'211);
        CHECK(crossingValidation.at("crossing_pair_pixels") == 2'705);
        CHECK(crossingValidation.at("residual_samples_each_measured_frame") ==
            0);
        CHECK(crossingValidation.at("invalid_pixels") == 0);
        CHECK(crossingValidation.at("local_color_pixels") == 14'211);
        CHECK(crossingValidation.at("validation_messages") == 0);
        CHECK(crossingValidation.at("performance_evidence") == false);
        CHECK(crossingValidation.at("visible_capture").at(
            "visually_inspected") == true);
        const Json& earlyTerminationValidation = deepLocalComposition.at(
            "hero4_early_termination_gpu_capture_validation");
        CHECK(earlyTerminationValidation.at("passed") == true);
        CHECK(earlyTerminationValidation.at("fixture") ==
            "hero4_early_termination_tetrahedra_v1");
        CHECK(earlyTerminationValidation.at(
            "maximum_observed_interfaces") == 2);
        CHECK(earlyTerminationValidation.at("interface_pixels") ==
            Json::array({ 7'522, 7'522, 0, 0 }));
        CHECK(earlyTerminationValidation.at(
            "early_terminated_pixels") == 7'522);
        CHECK(earlyTerminationValidation.at(
            "terminated_occupied_tiles") == 43);
        CHECK(earlyTerminationValidation.at(
            "terminated_occupied_tiles_by_interface") ==
            Json::array({ 0, 43, 0, 0 }));
        CHECK(earlyTerminationValidation.at("invalid_pixels") == 0);
        CHECK(earlyTerminationValidation.at(
            "compatibility_forward_draws") == 0);
        CHECK(earlyTerminationValidation.at("validation_messages") == 0);
        const Json& cinematicValidation = deepLocalComposition.at(
            "cinematic8_gpu_capture_validation");
        CHECK(cinematicValidation.at("passed") == true);
        CHECK(cinematicValidation.at("fixture") ==
            "cinematic8_nested_tetrahedra_v1");
        CHECK(cinematicValidation.at("maximum_observed_interfaces") == 8);
        CHECK(cinematicValidation.at("scene_resolve_draws") == 4);
        CHECK(cinematicValidation.at("compatibility_forward_draws") == 0);
        CHECK(cinematicValidation.at("interface_pixels") == Json::array({
            15'042, 15'042, 9'216, 9'216, 4'680, 4'680, 1'636, 1'636 }));
        CHECK(cinematicValidation.at("maximum_tier_interface_pixels") ==
            1'636);
        CHECK(cinematicValidation.at("validation_messages") == 0);
        CHECK(cinematicValidation.at("visible_capture").at(
            "visually_inspected") == true);
        const Json& overflowValidation = deepLocalComposition.at(
            "cinematic8_overflow_residual_validation");
        CHECK(overflowValidation.at("passed") == true);
        CHECK(overflowValidation.at("fixture") ==
            "cinematic8_overflow_residual_v1");
        CHECK(overflowValidation.at("authored_shells") == 5);
        CHECK(overflowValidation.at("requested_interfaces") == 10);
        CHECK(overflowValidation.at("stored_interfaces") == 8);
        CHECK(overflowValidation.at("scene_resolve_draws") == 5);
        CHECK(overflowValidation.at("compatibility_forward_draws") == 0);
        CHECK(overflowValidation.at("saturated_residual_pixels") == 650);
        CHECK(overflowValidation.at(
            "residual_samples_each_measured_frame") == 1'300);
        CHECK(overflowValidation.at(
            "zero_control_residual_samples_each_measured_frame") == 0);
        CHECK(overflowValidation.at("residual_probe_draws") == 5);
        CHECK(overflowValidation.at("invalid_pixels") == 0);
        CHECK(overflowValidation.at("validation_messages") == 0);
        CHECK(overflowValidation.at("performance_evidence") == false);
        CHECK(overflowValidation.at("visible_capture").at(
            "visually_inspected") == true);
        const Json& mixedValidation = deepLocalComposition.at(
            "mixed_tier_gpu_validation");
        CHECK(mixedValidation.at("passed") == true);
        CHECK(mixedValidation.at("hero4_packets") == 2);
        CHECK(mixedValidation.at("cinematic8_packets") == 2);
        CHECK(mixedValidation.at("scene_resolve_draws") == 4);
        CHECK(mixedValidation.at("compatibility_forward_draws") == 0);
        CHECK(mixedValidation.at("gpu_range") ==
            "gpu.transparency.layered.deep.scene-resolve");
        const Json& lifecycleValidation = deepLocalComposition.at(
            "deep_tier_lifecycle_validation");
        CHECK(lifecycleValidation.at("passed") == true);
        CHECK(lifecycleValidation.at("cycles_each") == 2);
        CHECK(lifecycleValidation.at("event_frames") ==
            Json::array({ 0, 121, 123, 244, 246 }));
        CHECK(lifecycleValidation.at("hero4").at("retirements") == 2);
        CHECK(lifecycleValidation.at("hero4").at("reactivations") == 2);
        CHECK(lifecycleValidation.at("cinematic8").at("retirements") == 2);
        CHECK(lifecycleValidation.at("cinematic8").at("reactivations") == 2);
        CHECK(lifecycleValidation.at("validation_messages") == 0);
        const Json& native4k = deepLocalComposition.at(
            "cinematic8_native_4k_qualification");
        CHECK(native4k.at("passed") == true);
        CHECK(native4k.at("extent") == Json::array({ 3840, 2160 }));
        CHECK(native4k.at("independent_processes") == 5);
        CHECK(native4k.at("measured_frames_total") == 50'000);
        CHECK(native4k.at("profiles").size() == 5);
        CHECK(native4k.at("gpu_frame_median_of_medians_ns") ==
            1'680'448);
        CHECK(native4k.at("recorded_compatibility_forward_draws_each_frame") ==
            0);
        CHECK(native4k.at("dropped_frames") == 0);
        CHECK(gpuStorage.at("compatibility_forward_remains_visible") == true);
        CHECK(gpuStorage.at("dormant_pass_skip_dynamic_allocation") == false);
        CHECK(gpuStorage.at("default_graph_memory_delta_bytes") == 0);
        CHECK(layeredTiers.at("tests").size() == 12);
        CHECK(layeredTiers.at("default_graph_memory_delta_bytes") == 0);
        CHECK(layeredTiers.at("open_gates").empty());
        const Json& weightedOit = manifest.at(
            "m6_7_weighted_oit_candidate");
        CHECK(weightedOit.at("status") ==
            "backend_neutral_reference_contract");
        CHECK(weightedOit.at("explicit_only") == true);
        CHECK(weightedOit.at("opaque_depth_access") == "read_only");
        CHECK(weightedOit.at("accumulation").at(
            "color_and_weight_format") == "RGBA16_FLOAT");
        CHECK(weightedOit.at("accumulation").at(
            "revealage_format") == "R16_FLOAT");
        CHECK(weightedOit.at("accumulation").at(
            "qualified_maximum_fragments_per_pixel") == 4'096);
        CHECK(weightedOit.at(
            "native_4k_logical_bytes_per_frame_context") == 82'944'000);
        CHECK(weightedOit.at("owned_depth_bytes") == 0);
        CHECK(weightedOit.at("visible_rendering_changed") == false);
        CHECK(weightedOit.at("default_graph_memory_delta_bytes") == 0);
        CHECK(weightedOit.at("open_gates").size() == 4);
        return true;
    }

}

int main() {
    struct Test { const char* name; bool (*run)(); };
    constexpr std::array tests{
        Test{ "fixture matrix", fixtureMatrixIsCompleteAndDeterministic },
        Test{ "transport reference", transportReferenceIsAnalyticAndFinite },
        Test{ "run contract", runContractIsExplicitAndLocalInputsAreVerifiedWhenPresent },
    };
    for (const Test& test : tests) {
        std::cout << test.name << '\n';
        if (!test.run()) return 1;
    }
    std::cout << "M6 fixture contract tests passed\n";
    return 0;
}
