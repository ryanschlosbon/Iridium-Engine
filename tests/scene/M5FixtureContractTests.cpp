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
        const Json& current = contract.at("m5_10Supersession");
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
        for (auto entry = correctiveHashes.begin();
            entry != correctiveHashes.end(); ++entry) {
            CHECK(Iridium::sha256File(root() / entry.key()) ==
                entry.value().get<std::string>());
        }
        const auto expectedCurrentHash = [&](const std::string& path,
            const std::string& acceptedHash) {
            return correctiveHashes.contains(path)
                ? correctiveHashes.at(path).get<std::string>()
                : acceptedHash;
        };
        const Json& supersession = manifest.at("m5_9Supersession");
        for (const char* group : { "current_shader_hashes",
                "current_spirv_hashes" }) {
            for (auto entry = supersession.at(group).begin();
                entry != supersession.at(group).end(); ++entry) {
                CHECK(Iridium::sha256File(root() / entry.key()) ==
                    expectedCurrentHash(entry.key(),
                        entry.value().get<std::string>()));
            }
        }
        const Json& acceptance = manifest.at("m5_11Acceptance");
        CHECK(acceptance.at("status").is_string());
        CHECK(acceptance.at("contract_hashes").size() >= 9);
        for (auto entry = acceptance.at("contract_hashes").begin();
            entry != acceptance.at("contract_hashes").end(); ++entry) {
            CHECK(Iridium::sha256File(root() / entry.key()) ==
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
