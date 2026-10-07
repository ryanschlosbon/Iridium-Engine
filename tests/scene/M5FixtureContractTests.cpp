#include "scene/authoring/CoreSceneComponentAdapters.h"
#include "scene/authoring/CookedSceneCompiler.h"
#include "scene/authoring/SourceSceneDocument.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"
#include "scene/components/LightComponent.h"
#include "scene/runtime/CookedScene.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

    using Json = nlohmann::json;

    #define CHECK(condition) do { if (!(condition)) { \
        std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
        return false; } } while (false)

    std::filesystem::path root() {
        return std::filesystem::path(PROJECT_ROOT_DIR);
    }

    // Historical evidence sections pin both fixture content and the engine code that
    // produced it. Engine code (C++, CMake, shader source and SPIR-V) legitimately
    // changes after evidence is recorded, so only fixture/evidence content is still
    // required to match (M7R R1 decision; provenance remains in the manifest).
    bool pinsEvidenceContent(const std::string& path) {
        for (const char* prefix : { "src/", "tests/", "tools/", "assets/shaders/" }) {
            if (path.starts_with(prefix)) return false;
        }
        return path != "CMakeLists.txt" && path != "CMakePresets.json";
    }

    std::vector<std::byte> readBytes(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("could not read " + path.string());
        const auto length = input.tellg();
        input.seekg(0);
        std::vector<std::byte> bytes(static_cast<size_t>(length));
        input.read(reinterpret_cast<char*>(bytes.data()), length);
        return bytes;
    }

    std::string readText(const std::filesystem::path& path) {
        const auto bytes = readBytes(path);
        return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
    }

    Json readJson(const std::filesystem::path& path) {
        return Json::parse(readText(path));
    }

    bool fixtureMatrixIsCompleteAndDeterministic() {
        const auto matrixPath = root() / "assets" / "benchmarks" / "m5" /
            "fixture-matrix.v1.json";
        const Json matrix = readJson(matrixPath);
        CHECK(matrix.at("schema_version") == 1);
        CHECK(matrix.at("milestone") == "M5.0");
        CHECK(matrix.at("color_domain") == "scene_linear_acescg_ap1");
        CHECK(matrix.at("generated_scene_policy").at("seed").is_number_unsigned());
        CHECK(matrix.at("generated_scene_policy").at("identity_order") ==
            "ascending_uuid");
        CHECK(matrix.at("generated_scene_policy").at("production_resolution") ==
            Json::array({ 3840, 2160 }));

        const std::set<std::string> required{
            "legacy_fixed_lighting_v1", "sun_sky_v1", "point_spot_v1",
            "cone_boundary_v1", "local_lights_64_v1", "local_lights_512_v1",
            "local_lights_4096_v1", "cluster_overflow_v1",
            "standard_deferred_forward_parity_v1", "emissive_no_implicit_gi_v1",
            "ibl_roughness_f0_f90_v1", "directional_shadow_motion_v1",
            "local_shadow_cache_v1", "reflection_probe_overlap_v1",
            "baked_contract_v1", "complex_closure_lighting_v1",
            "sample_car_lighting_local_v1"
        };
        std::set<std::string> actual;
        for (const Json& entry : matrix.at("cases")) {
            CHECK(entry.at("id").is_string());
            CHECK(entry.at("availability").is_string());
            CHECK(entry.at("active_from").is_string());
            CHECK(entry.at("purpose").is_string());
            CHECK(actual.insert(entry.at("id").get<std::string>()).second);
        }
        CHECK(actual == required);

        std::set<int> stressCounts;
        for (const Json& entry : matrix.at("cases")) {
            if (entry.at("id") == "local_lights_64_v1" ||
                entry.at("id") == "local_lights_512_v1" ||
                entry.at("id") == "local_lights_4096_v1") {
                stressCounts.insert(entry.at("recipe").at("local_light_count")
                    .get<int>());
            }
        }
        CHECK(stressCounts == std::set<int>({ 64, 512, 4096 }));
        const auto overflow = std::ranges::find_if(matrix.at("cases"),
            [](const Json& entry) { return entry.at("id") == "cluster_overflow_v1"; });
        CHECK(overflow != matrix.at("cases").end());
        CHECK(overflow->at("recipe").at("required_fallback_count") == 64);

        const Json& tracked = matrix.at("tracked_sources").at(0);
        const auto trackedPath = std::filesystem::weakly_canonical(
            matrixPath.parent_path() / std::filesystem::path(
                tracked.at("path").get<std::string>()));
        CHECK(std::filesystem::is_regular_file(trackedPath));
        CHECK(Iridium::sha256File(trackedPath) ==
            tracked.at("sha256").get<std::string>());
        CHECK(tracked.at("expected_entities") == 3);
        CHECK(tracked.at("expected_lights").at("directional") == 1);
        CHECK(tracked.at("expected_lights").at("point") == 1);
        CHECK(tracked.at("expected_lights").at("spot") == 1);
        return true;
    }

    bool sourceCookAndSourceFreeLoadAreFrozen() {
        const auto fixture = root() / "tests" / "scene" / "fixtures" /
            "m5_lighting_v1.iridium.scene.json";
        const Json contract = readJson(root() / "tests" / "scene" / "fixtures" /
            "m5_lighting_v1_contract.json");
        const Json& current = contract.at("m7_5Supersession");
        bool contractMatches = true;
        const auto compareContract = [&contractMatches, &current](
            std::string_view field, const auto& actual) {
            const auto expected = current.at(std::string(field))
                .template get<std::decay_t<decltype(actual)>>();
            if (actual == expected) return;
            std::cerr << "  contract mismatch " << field << ": expected "
                << expected << ", actual " << actual << '\n';
            contractMatches = false;
        };
        const auto sourceBytes = readBytes(fixture);
        CHECK(Iridium::sha256(sourceBytes) ==
            contract.at("sourceSha256").get<std::string>());

        auto registries = Iridium::createCoreSceneRegistryBundle();
        CHECK(registries);
        const std::string manifestHash =
            Iridium::runtimeComponentManifestHash(registries.runtime);
        compareContract("runtimeComponentManifestSha256", manifestHash);
        const auto parsed = Iridium::readSourceSceneSchema1(
            std::string(reinterpret_cast<const char*>(sourceBytes.data()),
                sourceBytes.size()), registries.runtime, registries.source);
        CHECK(parsed);
        CHECK(parsed.diagnostics.size() == 6);
        CHECK(std::ranges::count_if(parsed.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code ==
                    "light.v1_color_assumed_linear_rec709";
            }) == 3);
        CHECK(std::ranges::count_if(parsed.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code == "light.v1_intensity_unit_adopted";
            }) == 3);
        for (const auto& entity : parsed.document->entities) {
            const auto light = std::ranges::find_if(entity.components,
                [](const auto& component) {
                    return component.id.value() == "iridium.component.light";
                });
            CHECK(light != entity.components.end());
            CHECK(light->version == 2);
        }
        const auto canonical = Iridium::writeSourceSceneCanonical(
            *parsed.document, registries.runtime, registries.source);
        CHECK(canonical);
        const std::string canonicalHash = Iridium::sha256(std::as_bytes(std::span(
            canonical.bytes->data(), canonical.bytes->size())));
        compareContract("canonicalSha256", canonicalHash);

        auto staged = Iridium::stageSourceScene(
            *parsed.document, registries.runtime, registries.source);
        CHECK(staged);
        const auto assetGuid = Iridium::AssetGuid::parse(
            contract.at("sceneAssetGuid").get<std::string>());
        CHECK(assetGuid.has_value());
        const Iridium::CookedSceneCompileInput input{
            .sceneAssetGuid = *assetGuid,
            .sourceContentHash = contract.at("sourceSha256").get<std::string>(),
            .canonicalContentHash = canonicalHash,
            .target = {
                .platform = contract.at("target").at("platform").get<std::string>(),
                .profile = contract.at("target").at("profile").get<std::string>(),
                .qualityPolicy = contract.at("target").at("qualityPolicy")
                    .get<std::string>(),
            },
        };
        const auto first = Iridium::compileCookedScene(
            *staged.staging, registries.runtime, registries.source, input);
        const auto second = Iridium::compileCookedScene(
            *staged.staging, registries.runtime, registries.source, input);
        CHECK(first && second);
        compareContract("cookKey", first.artifact->cookKey);
        const auto firstBlob = Iridium::serializeCookedArtifact(*first.artifact);
        const auto secondBlob = Iridium::serializeCookedArtifact(*second.artifact);
        CHECK(firstBlob.bytes == secondBlob.bytes);
        compareContract("artifactSha256", firstBlob.artifactHash);
        compareContract("artifactBytes", firstBlob.bytes.size());

        const auto loaded = Iridium::stageCookedScene(firstBlob.bytes,
            registries.runtime, {
                .expectedSceneAssetGuid = *assetGuid,
                .expectedTarget = input.target,
                .expectedCookKey = first.artifact->cookKey,
                .expectedArtifactHash = firstBlob.artifactHash,
            });
        CHECK(loaded);
        CHECK(loaded.staging->world->registry().aliveCount() == 3);
        const auto* lights = loaded.staging->world->registry()
            .findPool<LightComponent>();
        CHECK(lights != nullptr);
        CHECK(lights->components.size() == 3);
        std::array<size_t, 3> counts{};
        for (const LightComponent& light : lights->components) {
            const auto type = static_cast<size_t>(light.type);
            CHECK(type < counts.size());
            ++counts[type];
        }
        constexpr std::array<size_t, 3> expectedCounts{ 1, 1, 1 };
        CHECK(counts == expectedCounts);
        CHECK(contractMatches);
        return true;
    }

    bool runAndCaptureContractIsExplicit() {
        const Json manifest = readJson(root() / "assets" / "benchmarks" / "m5" /
            "run-manifest.v1.json");
        CHECK(manifest.at("schema_version") == 1);
        CHECK(manifest.at("resolution") == Json::array({ 3840, 2160 }));
        CHECK(manifest.at("warmup_frames") == 500);
        CHECK(manifest.at("measured_frames") == 10000);
        CHECK(manifest.at("independent_processes") == 5);
        CHECK(manifest.at("statistics") ==
            Json::array({ "median", "p95", "p99" }));
        CHECK(manifest.at("capture_contract").at("scene-linear")
            .at("output_transform_applied") == false);
        for (const char* output : { "final-sdr", "scrgb", "hdr10" }) {
            CHECK(manifest.at("capture_contract").at(output)
                .at("output_transform_applied") == true);
        }
        CHECK(manifest.at("required_metadata").size() >= 16);
        CHECK(manifest.at("required_counters").size() >= 9);
        CHECK(manifest.at("current_defects").size() == 4);
        CHECK(manifest.at("current_defect_shader_hashes").at(
            "assets/shaders/include/canonical_lighting_body.glsl") ==
            "09a75d9e9940ec097213ce12e0f89b799f51f631780182dda9f9aa1b396cac6a");
        CHECK(manifest.at("m5_2Supersession").at("status").is_string());
        CHECK(manifest.at("m5_3Supersession").at("status").is_string());
        CHECK(manifest.at("m5_4Supersession").at("status").is_string());
        CHECK(manifest.at("m5_5Supersession").at("status").is_string());
        CHECK(manifest.at("m5_6Supersession").at("status").is_string());
        CHECK(manifest.at("m5_7Supersession").at("status").is_string());
        CHECK(manifest.at("m5_8Supersession").at("status").is_string());
        const Json& corrective = manifest.at("m5_13CorrectiveHardening");
        CHECK(corrective.at("status").is_string());
        const Json& correctiveHashes = corrective.at("current_contract_hashes");
        const Json& m6Supersession = manifest.at("m6_1Supersession");
        CHECK(m6Supersession.at("status").is_string());
        const Json& m6Hashes = m6Supersession.at("current_contract_hashes");
        const Json& m6_2Supersession = manifest.at("m6_2Supersession");
        CHECK(m6_2Supersession.at("status").is_string());
        const Json& m6_2Hashes =
            m6_2Supersession.at("current_contract_hashes");
        const Json& m6_3Supersession = manifest.at("m6_3Supersession");
        CHECK(m6_3Supersession.at("status").is_string());
        const Json& m6_3Hashes =
            m6_3Supersession.at("current_contract_hashes");
        const Json& m6_4Supersession = manifest.at("m6_4Supersession");
        CHECK(m6_4Supersession.at("status").is_string());
        const Json& m6_4Hashes =
            m6_4Supersession.at("current_contract_hashes");
        const Json& m6_5Supersession = manifest.at("m6_5Supersession");
        CHECK(m6_5Supersession.at("status").is_string());
        const Json& m6_5Hashes =
            m6_5Supersession.at("current_contract_hashes");
        const Json& m6_6Supersession = manifest.at("m6_6Supersession");
        CHECK(m6_6Supersession.at("status").is_string());
        const Json& m6_6Hashes =
            m6_6Supersession.at("current_contract_hashes");
        const Json& m6_7Supersession = manifest.at("m6_7Supersession");
        CHECK(m6_7Supersession.at("status").is_string());
        const Json& m6_7Hashes =
            m6_7Supersession.at("current_contract_hashes");
        const Json& m6_8Supersession = manifest.at("m6_8Supersession");
        CHECK(m6_8Supersession.at("status").is_string());
        const Json& m6_8Hashes =
            m6_8Supersession.at("current_contract_hashes");
        const Json& m7_7Supersession = manifest.at("m7_7Supersession");
        CHECK(m7_7Supersession.at("status").is_string());
        const Json& m7_7Hashes =
            m7_7Supersession.at("current_contract_hashes");
        const Json& m7_7BiasSupersession =
            manifest.at("m7_7BiasSupersession");
        CHECK(m7_7BiasSupersession.at("status").is_string());
        const Json& m7_7BiasHashes =
            m7_7BiasSupersession.at("current_contract_hashes");
        const Json& m7_7CasterVisibilitySupersession =
            manifest.at("m7_7CasterVisibilitySupersession");
        CHECK(m7_7CasterVisibilitySupersession.at("status").is_string());
        const Json& m7_7CasterVisibilityHashes =
            m7_7CasterVisibilitySupersession.at("current_contract_hashes");
        const Json& m7_7AffectedBoundsSupersession =
            manifest.at("m7_7AffectedBoundsSupersession");
        CHECK(m7_7AffectedBoundsSupersession.at("status").is_string());
        const Json& m7_7AffectedBoundsHashes =
            m7_7AffectedBoundsSupersession.at("current_contract_hashes");
        const Json& m7_7GpuSceneShadowSubmissionSupersession =
            manifest.at("m7_7GpuSceneShadowSubmissionSupersession");
        CHECK(m7_7GpuSceneShadowSubmissionSupersession.at("status").is_string());
        const Json& m7_7GpuSceneShadowSubmissionHashes =
            m7_7GpuSceneShadowSubmissionSupersession.at(
                "current_contract_hashes");
        const Json& m7_7DirectionalDeviceCommandSupersession =
            manifest.at("m7_7DirectionalDeviceCommandSupersession");
        CHECK(m7_7DirectionalDeviceCommandSupersession.at("status").is_string());
        const Json& m7_7DirectionalDeviceCommandHashes =
            m7_7DirectionalDeviceCommandSupersession.at(
                "current_contract_hashes");
        const Json& m7_7LocalShadowDeviceCommandSupersession =
            manifest.at("m7_7LocalShadowDeviceCommandSupersession");
        CHECK(m7_7LocalShadowDeviceCommandSupersession.at("status").is_string());
        const Json& m7_7LocalShadowDeviceCommandHashes =
            m7_7LocalShadowDeviceCommandSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ShadowCommandOracleGatingSupersession =
            manifest.at("m7_7ShadowCommandOracleGatingSupersession");
        CHECK(m7_7ShadowCommandOracleGatingSupersession.at("status").is_string());
        const Json& m7_7ShadowCommandOracleGatingHashes =
            m7_7ShadowCommandOracleGatingSupersession.at(
                "current_contract_hashes");
        const Json& m7_7IndependentProbeVisibilitySupersession =
            manifest.at("m7_7IndependentProbeVisibilitySupersession");
        CHECK(m7_7IndependentProbeVisibilitySupersession.at("status").
            is_string());
        const Json& m7_7IndependentProbeVisibilityHashes =
            m7_7IndependentProbeVisibilitySupersession.at(
                "current_contract_hashes");
        const Json& m7_7ProbeGpuSceneCapturePipelineSupersession =
            manifest.at("m7_7ProbeGpuSceneCapturePipelineSupersession");
        CHECK(m7_7ProbeGpuSceneCapturePipelineSupersession.at("status").
            is_string());
        const Json& m7_7ProbeGpuSceneCapturePipelineHashes =
            m7_7ProbeGpuSceneCapturePipelineSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ProbeDeviceCommandSupersession =
            manifest.at("m7_7ProbeDeviceCommandSupersession");
        CHECK(m7_7ProbeDeviceCommandSupersession.at("status").is_string());
        const Json& m7_7ProbeDeviceCommandHashes =
            m7_7ProbeDeviceCommandSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ProbeLodSupersession =
            manifest.at("m7_7ProbeLodSupersession");
        CHECK(m7_7ProbeLodSupersession.at("status").is_string());
        const Json& m7_7ProbeLodHashes =
            m7_7ProbeLodSupersession.at("current_contract_hashes");
        const Json& m7_7DirectionalShadowLodSupersession =
            manifest.at("m7_7DirectionalShadowLodSupersession");
        CHECK(m7_7DirectionalShadowLodSupersession.at("status").is_string());
        const Json& m7_7DirectionalShadowLodHashes =
            m7_7DirectionalShadowLodSupersession.at(
                "current_contract_hashes");
        const Json& m7_7PointShadowLodSupersession =
            manifest.at("m7_7PointShadowLodSupersession");
        CHECK(m7_7PointShadowLodSupersession.at("status").is_string());
        const Json& m7_7PointShadowLodHashes =
            m7_7PointShadowLodSupersession.at("current_contract_hashes");
        const Json& m7_7SpotShadowLodSupersession =
            manifest.at("m7_7SpotShadowLodSupersession");
        CHECK(m7_7SpotShadowLodSupersession.at("status").is_string());
        const Json& m7_7SpotShadowLodHashes =
            m7_7SpotShadowLodSupersession.at("current_contract_hashes");
        const Json& m7_7HeterogeneousShadowAdmissionSupersession =
            manifest.at("m7_7HeterogeneousShadowAdmissionSupersession");
        CHECK(m7_7HeterogeneousShadowAdmissionSupersession.at("status").
            is_string());
        const Json& m7_7HeterogeneousShadowAdmissionHashes =
            m7_7HeterogeneousShadowAdmissionSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ProbeLodWarmedAdmissionSupersession =
            manifest.at("m7_7ProbeLodWarmedAdmissionSupersession");
        CHECK(m7_7ProbeLodWarmedAdmissionSupersession.at("status").
            is_string());
        const Json& m7_7ProbeLodWarmedAdmissionHashes =
            m7_7ProbeLodWarmedAdmissionSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ShadowLodWarmedAdmissionSupersession =
            manifest.at("m7_7ShadowLodWarmedAdmissionSupersession");
        CHECK(m7_7ShadowLodWarmedAdmissionSupersession.at("status").
            is_string());
        const Json& m7_7ShadowLodWarmedAdmissionHashes =
            m7_7ShadowLodWarmedAdmissionSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ShadowMembershipCacheSupersession =
            manifest.at("m7_7ShadowMembershipCacheSupersession");
        CHECK(m7_7ShadowMembershipCacheSupersession.at("status").
            is_string());
        const Json& m7_7ShadowMembershipCacheHashes =
            m7_7ShadowMembershipCacheSupersession.at(
                "current_contract_hashes");
        const Json& m7_7OpaqueShadowPositionFetchSupersession =
            manifest.at("m7_7OpaqueShadowPositionFetchSupersession");
        CHECK(m7_7OpaqueShadowPositionFetchSupersession.at("status").
            is_string());
        const Json& m7_7OpaqueShadowPositionFetchHashes =
            m7_7OpaqueShadowPositionFetchSupersession.at(
                "current_contract_hashes");
        const Json& m7_7ShadowQualityClosureSupersession =
            manifest.at("m7_7ShadowQualityClosureSupersession");
        CHECK(m7_7ShadowQualityClosureSupersession.at("status").
            is_string());
        const Json& m7_7ShadowQualityClosureHashes =
            m7_7ShadowQualityClosureSupersession.at(
                "current_contract_hashes");
        const Json& m7_8VirtualShadowResidencyContractSupersession =
            manifest.at("m7_8VirtualShadowResidencyContractSupersession");
        CHECK(m7_8VirtualShadowResidencyContractSupersession.at("status").
            is_string());
        const Json& m7_8VirtualShadowResidencyContractHashes =
            m7_8VirtualShadowResidencyContractSupersession.at(
                "current_contract_hashes");
        const auto latestHash = [&](const std::string& path,
            const std::string& priorHash) {
            const std::string m6_1Hash = m6Hashes.contains(path)
                ? m6Hashes.at(path).get<std::string>() : priorHash;
            const std::string m6_2Hash = m6_2Hashes.contains(path)
                ? m6_2Hashes.at(path).get<std::string>() : m6_1Hash;
            const std::string m6_3Hash = m6_3Hashes.contains(path)
                ? m6_3Hashes.at(path).get<std::string>() : m6_2Hash;
            const std::string m6_4Hash = m6_4Hashes.contains(path)
                ? m6_4Hashes.at(path).get<std::string>() : m6_3Hash;
            const std::string m6_5Hash = m6_5Hashes.contains(path)
                ? m6_5Hashes.at(path).get<std::string>() : m6_4Hash;
            const std::string m6_6Hash = m6_6Hashes.contains(path)
                ? m6_6Hashes.at(path).get<std::string>() : m6_5Hash;
            const std::string m6_7Hash = m6_7Hashes.contains(path)
                ? m6_7Hashes.at(path).get<std::string>() : m6_6Hash;
            const std::string m6_8Hash = m6_8Hashes.contains(path)
                ? m6_8Hashes.at(path).get<std::string>() : m6_7Hash;
            const std::string m7_7Hash = m7_7Hashes.contains(path)
                ? m7_7Hashes.at(path).get<std::string>() : m6_8Hash;
            const std::string m7_7BiasHash = m7_7BiasHashes.contains(path)
                ? m7_7BiasHashes.at(path).get<std::string>() : m7_7Hash;
            const std::string m7_7CasterVisibilityHash =
                m7_7CasterVisibilityHashes.contains(path)
                ? m7_7CasterVisibilityHashes.at(path).get<std::string>()
                : m7_7BiasHash;
            const std::string m7_7AffectedBoundsHash =
                m7_7AffectedBoundsHashes.contains(path)
                ? m7_7AffectedBoundsHashes.at(path).get<std::string>()
                : m7_7CasterVisibilityHash;
            const std::string m7_7GpuSceneShadowSubmissionHash =
                m7_7GpuSceneShadowSubmissionHashes.contains(path)
                ? m7_7GpuSceneShadowSubmissionHashes.at(path).
                    get<std::string>()
                : m7_7AffectedBoundsHash;
            const std::string m7_7DirectionalDeviceCommandHash =
                m7_7DirectionalDeviceCommandHashes.contains(path)
                ? m7_7DirectionalDeviceCommandHashes.at(path).
                    get<std::string>()
                : m7_7GpuSceneShadowSubmissionHash;
            const std::string m7_7LocalShadowDeviceCommandHash =
                m7_7LocalShadowDeviceCommandHashes.contains(path)
                ? m7_7LocalShadowDeviceCommandHashes.at(path).
                    get<std::string>()
                : m7_7DirectionalDeviceCommandHash;
            const std::string m7_7ShadowCommandOracleGatingHash =
                m7_7ShadowCommandOracleGatingHashes.contains(path)
                ? m7_7ShadowCommandOracleGatingHashes.at(path).
                    get<std::string>()
                : m7_7LocalShadowDeviceCommandHash;
            const std::string m7_7IndependentProbeVisibilityHash =
                m7_7IndependentProbeVisibilityHashes.contains(path)
                ? m7_7IndependentProbeVisibilityHashes.at(path).
                    get<std::string>()
                : m7_7ShadowCommandOracleGatingHash;
            const std::string m7_7ProbeGpuSceneCapturePipelineHash =
                m7_7ProbeGpuSceneCapturePipelineHashes.contains(path)
                ? m7_7ProbeGpuSceneCapturePipelineHashes.at(path).
                    get<std::string>()
                : m7_7IndependentProbeVisibilityHash;
            const std::string m7_7ProbeDeviceCommandHash =
                m7_7ProbeDeviceCommandHashes.contains(path)
                ? m7_7ProbeDeviceCommandHashes.at(path).get<std::string>()
                : m7_7ProbeGpuSceneCapturePipelineHash;
            const std::string m7_7ProbeLodHash =
                m7_7ProbeLodHashes.contains(path)
                ? m7_7ProbeLodHashes.at(path).get<std::string>()
                : m7_7ProbeDeviceCommandHash;
            const std::string m7_7DirectionalShadowLodHash =
                m7_7DirectionalShadowLodHashes.contains(path)
                ? m7_7DirectionalShadowLodHashes.at(path).get<std::string>()
                : m7_7ProbeLodHash;
            const std::string m7_7PointShadowLodHash =
                m7_7PointShadowLodHashes.contains(path)
                ? m7_7PointShadowLodHashes.at(path).get<std::string>()
                : m7_7DirectionalShadowLodHash;
            const std::string m7_7SpotShadowLodHash =
                m7_7SpotShadowLodHashes.contains(path)
                ? m7_7SpotShadowLodHashes.at(path).get<std::string>()
                : m7_7PointShadowLodHash;
            const std::string m7_7HeterogeneousShadowAdmissionHash =
                m7_7HeterogeneousShadowAdmissionHashes.contains(path)
                ? m7_7HeterogeneousShadowAdmissionHashes.at(path).
                    get<std::string>()
                : m7_7SpotShadowLodHash;
            const std::string m7_7ProbeLodWarmedAdmissionHash =
                m7_7ProbeLodWarmedAdmissionHashes.contains(path)
                ? m7_7ProbeLodWarmedAdmissionHashes.at(path).
                    get<std::string>()
                : m7_7HeterogeneousShadowAdmissionHash;
            const std::string m7_7ShadowLodWarmedAdmissionHash =
                m7_7ShadowLodWarmedAdmissionHashes.contains(path)
                ? m7_7ShadowLodWarmedAdmissionHashes.at(path).
                    get<std::string>()
                : m7_7ProbeLodWarmedAdmissionHash;
            const std::string m7_7ShadowMembershipCacheHash =
                m7_7ShadowMembershipCacheHashes.contains(path)
                ? m7_7ShadowMembershipCacheHashes.at(path).
                    get<std::string>()
                : m7_7ShadowLodWarmedAdmissionHash;
            const std::string m7_7OpaqueShadowPositionFetchHash =
                m7_7OpaqueShadowPositionFetchHashes.contains(path)
                ? m7_7OpaqueShadowPositionFetchHashes.at(path).
                    get<std::string>()
                : m7_7ShadowMembershipCacheHash;
            const std::string m7_7ShadowQualityClosureHash =
                m7_7ShadowQualityClosureHashes.contains(path)
                ? m7_7ShadowQualityClosureHashes.at(path).
                    get<std::string>()
                : m7_7OpaqueShadowPositionFetchHash;
            return m7_8VirtualShadowResidencyContractHashes.contains(path)
                ? m7_8VirtualShadowResidencyContractHashes.at(path).
                    get<std::string>()
                : m7_7ShadowQualityClosureHash;
        };
        for (auto entry = correctiveHashes.begin();
            entry != correctiveHashes.end(); ++entry) {
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        }
        for (auto entry = m6Hashes.begin(); entry != m6Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_2Hashes.begin(); entry != m6_2Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_3Hashes.begin(); entry != m6_3Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_4Hashes.begin(); entry != m6_4Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_5Hashes.begin(); entry != m6_5Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_6Hashes.begin(); entry != m6_6Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_7Hashes.begin(); entry != m6_7Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m6_8Hashes.begin(); entry != m6_8Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7Hashes.begin(); entry != m7_7Hashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7BiasHashes.begin();
            entry != m7_7BiasHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7CasterVisibilityHashes.begin();
            entry != m7_7CasterVisibilityHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7AffectedBoundsHashes.begin();
            entry != m7_7AffectedBoundsHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7GpuSceneShadowSubmissionHashes.begin();
            entry != m7_7GpuSceneShadowSubmissionHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7DirectionalDeviceCommandHashes.begin();
            entry != m7_7DirectionalDeviceCommandHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7LocalShadowDeviceCommandHashes.begin();
            entry != m7_7LocalShadowDeviceCommandHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ShadowCommandOracleGatingHashes.begin();
            entry != m7_7ShadowCommandOracleGatingHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7IndependentProbeVisibilityHashes.begin();
            entry != m7_7IndependentProbeVisibilityHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ProbeGpuSceneCapturePipelineHashes.begin();
            entry != m7_7ProbeGpuSceneCapturePipelineHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ProbeDeviceCommandHashes.begin();
            entry != m7_7ProbeDeviceCommandHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ProbeLodHashes.begin();
            entry != m7_7ProbeLodHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7DirectionalShadowLodHashes.begin();
            entry != m7_7DirectionalShadowLodHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7PointShadowLodHashes.begin();
            entry != m7_7PointShadowLodHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7SpotShadowLodHashes.begin();
            entry != m7_7SpotShadowLodHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7HeterogeneousShadowAdmissionHashes.begin();
            entry != m7_7HeterogeneousShadowAdmissionHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ProbeLodWarmedAdmissionHashes.begin();
            entry != m7_7ProbeLodWarmedAdmissionHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ShadowLodWarmedAdmissionHashes.begin();
            entry != m7_7ShadowLodWarmedAdmissionHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ShadowMembershipCacheHashes.begin();
            entry != m7_7ShadowMembershipCacheHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7OpaqueShadowPositionFetchHashes.begin();
            entry != m7_7OpaqueShadowPositionFetchHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_7ShadowQualityClosureHashes.begin();
            entry != m7_7ShadowQualityClosureHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                latestHash(entry.key(), entry.value().get<std::string>()));
        for (auto entry = m7_8VirtualShadowResidencyContractHashes.begin();
            entry != m7_8VirtualShadowResidencyContractHashes.end(); ++entry)
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                entry.value().get<std::string>());
        const auto expectedCurrentHash = [&](const std::string& path,
            const std::string& acceptedHash) {
            const std::string corrected = correctiveHashes.contains(path)
                ? correctiveHashes.at(path).get<std::string>()
                : acceptedHash;
            return latestHash(path, corrected);
        };
        const Json& supersession = manifest.at("m5_9Supersession");
        for (const char* group : { "current_shader_hashes",
                "current_spirv_hashes" }) {
            for (auto entry = supersession.at(group).begin();
                entry != supersession.at(group).end(); ++entry) {
                CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                    expectedCurrentHash(entry.key(),
                        entry.value().get<std::string>()));
            }
        }
        const Json& acceptance = manifest.at("m5_11Acceptance");
        CHECK(acceptance.at("status").is_string());
        CHECK(acceptance.at("contract_hashes").size() >= 9);
        for (auto entry = acceptance.at("contract_hashes").begin();
            entry != acceptance.at("contract_hashes").end(); ++entry) {
            CHECK(!pinsEvidenceContent(entry.key()) ||
                Iridium::sha256File(root() / entry.key()) ==
                expectedCurrentHash(entry.key(),
                    entry.value().get<std::string>()));
        }
        return true;
    }

}

int main() {
    struct Test { const char* name; bool (*run)(); };
    constexpr std::array tests{
        Test{ "fixture matrix", fixtureMatrixIsCompleteAndDeterministic },
        Test{ "source cook and source-free load", sourceCookAndSourceFreeLoadAreFrozen },
        Test{ "run and capture contract", runAndCaptureContractIsExplicit },
    };
    for (const Test& test : tests) {
        std::cout << test.name << '\n';
        if (!test.run()) return 1;
    }
    std::cout << "M5 fixture contract tests passed\n";
    return 0;
}
