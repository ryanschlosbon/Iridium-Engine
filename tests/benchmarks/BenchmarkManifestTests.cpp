#include "benchmarks/BenchmarkManifest.h"
#include "core/ProjectAssetRoots.h"
#include "utils/Sha256.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    // A project-relative content path ("assets/models/..."): under assets/ it
    // resolves through the project and local asset roots.
    std::filesystem::path projectContentPath(const std::string& projectRelative) {
        const std::filesystem::path path(projectRelative);
        auto part = path.begin();
        if (part != path.end() && *part == "assets") {
            std::filesystem::path rootRelative;
            for (++part; part != path.end(); ++part) rootRelative /= *part;
            return resolveProjectAssetPath(rootRelative);
        }
        return std::filesystem::path(PROJECT_ROOT_DIR) / path;
    }

    std::filesystem::path manifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m0" / "manifest.v1.json";
    }

    std::filesystem::path m1ManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m1" / "manifest.v1.json";
    }

    std::filesystem::path m2FixtureMatrixPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m2" / "fixture-matrix.v1.json";
    }

    std::filesystem::path m2RunManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m2" / "run-manifest.v1.json";
    }

    std::filesystem::path m2MaterialGpuManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m2" / "material-gpu-manifest.v1.json";
    }

    std::filesystem::path m6PyramidManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m6-pyramid-manifest.v1.json";
    }

    std::filesystem::path m6Ordinary2RuntimeManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m6" /
            "ordinary2-runtime-manifest.v1.json";
    }

    std::filesystem::path m7ThreeDenseManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" /
            "m7-three-dense-assets-manifest.v1.json";
    }

    std::filesystem::path m7FixtureMatrixPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m7" / "fixture-matrix.v1.json";
    }

    std::filesystem::path m7OcclusionTemporalManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-occlusion-temporal-manifest.v1.json";
    }

    std::filesystem::path m7OcclusionMotionManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-occlusion-motion-manifest.v1.json";
    }

    std::filesystem::path m7OcclusionSmallObjectManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-occlusion-small-object-manifest.v1.json";
    }

    std::filesystem::path m7OcclusionDepthContentManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-occlusion-depth-content-manifest.v1.json";
    }

    std::filesystem::path m7OcclusionPerformanceManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-occlusion-performance-manifest.v1.json";
    }

    std::filesystem::path m7LodLitAdmissionManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-lod-lit-admission-manifest.v1.json";
    }

    std::filesystem::path m7DirectionalShadowGrazingManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-directional-shadow-grazing-manifest.v1.json";
    }

    std::filesystem::path m7DirectionalShadowLodManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-directional-shadow-lod-manifest.v1.json";
    }

    std::filesystem::path m7PointShadowLodManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-point-shadow-lod-manifest.v1.json";
    }

    std::filesystem::path m7SpotShadowLodManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-spot-shadow-lod-manifest.v1.json";
    }

    std::filesystem::path m7HeterogeneousShadowAdmissionManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-heterogeneous-shadow-admission-manifest.v1.json";
    }

    std::filesystem::path m7ProbeLodAdmissionManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-probe-lod-admission-manifest.v1.json";
    }

    std::filesystem::path m7ShadowLodWarmedAdmissionManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-shadow-lod-warmed-admission-manifest.v1.json";
    }

    std::filesystem::path m7ShadowQualityClosureManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) / "assets" /
            "m7-shadow-quality-closure-manifest.v1.json";
    }

    std::filesystem::path m7RunManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m7" / "m7.0-run-manifest.v1.json";
    }

    std::filesystem::path m9TemporalManifestPath() {
        return std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "benchmarks" / "m9" / "temporal-manifest.v1.json";
    }

    nlohmann::json loadJson(const std::filesystem::path& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Failed to open fixture: " + path.string());
        nlohmann::json result;
        input >> result;
        return result;
    }

    std::vector<std::byte> decodeDataUri(std::string_view uri) {
        const size_t delimiter = uri.find(',');
        if (delimiter == std::string_view::npos ||
            uri.substr(0, delimiter).find(";base64") == std::string_view::npos) {
            throw std::runtime_error("Fixture buffer must use an embedded base64 data URI");
        }
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<std::byte> decoded;
        uint32_t accumulator = 0;
        int bits = -8;
        for (const char character : uri.substr(delimiter + 1)) {
            if (character == '=') break;
            const size_t value = alphabet.find(character);
            if (value == std::string_view::npos) {
                throw std::runtime_error("Invalid base64 character in fixture buffer");
            }
            accumulator = (accumulator << 6u) | static_cast<uint32_t>(value);
            bits += 6;
            if (bits >= 0) {
                decoded.push_back(static_cast<std::byte>((accumulator >> bits) & 0xffu));
                bits -= 8;
            }
        }
        return decoded;
    }

    std::vector<uint16_t> readUint16Indices(const nlohmann::json& gltf,
        size_t accessorIndex) {
        const auto& accessor = gltf.at("accessors").at(accessorIndex);
        if (accessor.at("componentType").get<uint32_t>() != 5123u ||
            accessor.at("type").get<std::string>() != "SCALAR") {
            throw std::runtime_error("Expected an unsigned-short scalar index accessor");
        }
        const auto& view = gltf.at("bufferViews").at(
            accessor.at("bufferView").get<size_t>());
        const auto& buffer = gltf.at("buffers").at(view.at("buffer").get<size_t>());
        const std::vector<std::byte> bytes = decodeDataUri(
            buffer.at("uri").get<std::string>());
        const size_t offset = view.value("byteOffset", size_t{ 0 }) +
            accessor.value("byteOffset", size_t{ 0 });
        const size_t count = accessor.at("count").get<size_t>();
        if (offset > bytes.size() || count > (bytes.size() - offset) / 2) {
            throw std::runtime_error("Fixture index accessor exceeds its embedded buffer");
        }
        std::vector<uint16_t> result;
        result.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const size_t byteIndex = offset + index * 2;
            result.push_back(static_cast<uint16_t>(
                std::to_integer<uint8_t>(bytes[byteIndex]) |
                (static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[byteIndex + 1])) << 8u)));
        }
        return result;
    }

    bool containsText(const std::vector<std::string>& values, std::string_view text) {
        return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
            return value.find(text) != std::string::npos;
        });
    }

    bool testSha256KnownVector() {
        constexpr std::array bytes{ std::byte{ 'a' }, std::byte{ 'b' }, std::byte{ 'c' } };
        CHECK(sha256(bytes) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        return true;
    }

    bool testManifestAndContentVerification() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath());
        CHECK(manifest.schemaVersion == 1);
        CHECK(manifest.fixtures.size() == 6);
        CHECK(manifest.localDiagnostics.size() == 1);
        CHECK(manifest.localDiagnostics[0].id == "sample_car_local_v1");
        const BenchmarkFixture& fixture = findBenchmarkFixture(manifest, "material_lab_v1");
        CHECK(fixture.revision == 1);
        CHECK(fixture.required);
        CHECK(fixture.camera.id == "front_v1");
        CHECK(std::abs(fixture.camera.position.z - 7.0f) < 0.0001f);
        CHECK(fixture.warmupFrames == 500);
        CHECK(fixture.measuredFrames == 10000);
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 1));
        CHECK(fixture.sceneFactory.instanceScale == glm::vec3(1.0f));
        CHECK(sha256File(fixture.contentFiles[0].path) == fixture.contentFiles[0].sha256);
        CHECK(fixture.unavailableCapabilities.size() >= 6);
        const BenchmarkFixture& cpu = findBenchmarkFixture(manifest, "geometry_cpu_v1");
        CHECK(cpu.sceneFactory.instanceGrid == glm::uvec3(16, 8, 1));
        CHECK(cpu.sceneFactory.animateInstances);
        const BenchmarkFixture& temporal = findBenchmarkFixture(manifest,
            "temporal_proxy_v1");
        CHECK(temporal.sceneFactory.cameraCutEnabled);
        CHECK(temporal.sceneFactory.cameraCutFrame == 120);
        const BenchmarkCameraPose beforeCut = evaluateBenchmarkCamera(temporal, 119);
        const BenchmarkCameraPose atCut = evaluateBenchmarkCamera(temporal, 120);
        CHECK(std::abs(beforeCut.position.x - (-0.143f)) < 0.0001f);
        CHECK(atCut.position == glm::vec3(0.0f, 1.0f, 6.0f));
        CHECK(atCut.target == glm::vec3(0.0f));
        CHECK(std::abs(evaluateBenchmarkInstanceYOffset(
            temporal.sceneFactory, 30, 0) - 0.35f) < 0.0001f);
        return true;
    }

    bool testNestedTransparencyFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath());
        const BenchmarkFixture& fixture = findBenchmarkFixture(manifest, "transparency_v1");
        CHECK(fixture.revision == 2);
        CHECK(containsText(fixture.expectedBehavior, "concentric closed tetrahedral shells"));
        CHECK(containsText(fixture.expectedBehavior, "may hide or miscompose"));
        CHECK(!containsText(fixture.unavailableCapabilities, "nested closed shells"));
        CHECK(containsText(fixture.unavailableCapabilities,
            "correct nested closed-shell composition"));

        const nlohmann::json gltf = loadJson(fixture.sourceAsset);
        CHECK(gltf.at("materials").size() == 6);
        CHECK(gltf.at("meshes").size() == 6);
        CHECK(gltf.at("nodes").size() == 6);

        constexpr size_t Outer = 4;
        constexpr size_t Inner = 5;
        const auto& outerMaterial = gltf.at("materials").at(Outer);
        const auto& innerMaterial = gltf.at("materials").at(Inner);
        CHECK(outerMaterial.at("alphaMode") == "BLEND");
        CHECK(innerMaterial.at("alphaMode") == "BLEND");
        CHECK(outerMaterial.at("doubleSided").get<bool>());
        CHECK(innerMaterial.at("doubleSided").get<bool>());

        const auto& outerPrimitive = gltf.at("meshes").at(Outer).at("primitives").at(0);
        const auto& innerPrimitive = gltf.at("meshes").at(Inner).at("primitives").at(0);
        CHECK(gltf.at("meshes").at(Outer).at("primitives").size() == 1);
        CHECK(gltf.at("meshes").at(Inner).at("primitives").size() == 1);
        CHECK(outerPrimitive.at("material").get<size_t>() == Outer);
        CHECK(innerPrimitive.at("material").get<size_t>() == Inner);
        CHECK(outerPrimitive.at("material") != innerPrimitive.at("material"));
        CHECK(outerPrimitive.at("indices") == innerPrimitive.at("indices"));

        const auto& outerNode = gltf.at("nodes").at(Outer);
        const auto& innerNode = gltf.at("nodes").at(Inner);
        CHECK(outerNode.at("mesh").get<size_t>() == Outer);
        CHECK(innerNode.at("mesh").get<size_t>() == Inner);
        CHECK(outerNode.at("translation") == innerNode.at("translation"));
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK(outerNode.at("scale").at(axis).get<float>() >
                innerNode.at("scale").at(axis).get<float>());
        }

        const size_t indexAccessor = outerPrimitive.at("indices").get<size_t>();
        const std::vector<uint16_t> indices = readUint16Indices(gltf, indexAccessor);
        CHECK(indices.size() == 12);
        CHECK(gltf.at("accessors").at(
            outerPrimitive.at("attributes").at("POSITION").get<size_t>()).at("count") == 4);
        std::map<std::pair<uint16_t, uint16_t>, size_t> edgeUseCounts;
        for (size_t triangle = 0; triangle < indices.size(); triangle += 3) {
            CHECK(indices[triangle] != indices[triangle + 1]);
            CHECK(indices[triangle + 1] != indices[triangle + 2]);
            CHECK(indices[triangle + 2] != indices[triangle]);
            for (const auto edge : { std::pair{ indices[triangle], indices[triangle + 1] },
                std::pair{ indices[triangle + 1], indices[triangle + 2] },
                std::pair{ indices[triangle + 2], indices[triangle] } }) {
                ++edgeUseCounts[std::minmax(edge.first, edge.second)];
            }
        }
        CHECK(edgeUseCounts.size() == 6);
        CHECK(std::all_of(edgeUseCounts.begin(), edgeUseCounts.end(), [](const auto& edge) {
            return edge.second == 2;
        }));
        return true;
    }

    bool testOpaqueEmissiveRangeFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath());
        const BenchmarkFixture& fixture = findBenchmarkFixture(manifest,
            "opaque_emissive_v1");
        CHECK(fixture.revision == 1);
        CHECK(fixture.required);
        CHECK(fixture.camera.id == "front_emissive_range_v1");
        CHECK(fixture.constantEnvironmentLinear == glm::vec3(0.0f));
        CHECK(containsText(fixture.expectedBehavior,
            "0.0, 0.03125, 0.125, 0.5, 1.0, and 2.0"));
        CHECK(containsText(fixture.expectedBehavior, "emissive debug view"));
        CHECK(containsText(fixture.expectedBehavior, "does not illuminate"));

        const nlohmann::json gltf = loadJson(fixture.sourceAsset);
        CHECK(gltf.at("materials").size() == 6);
        CHECK(gltf.at("meshes").size() == 6);
        CHECK(gltf.at("nodes").size() == 6);
        constexpr std::array expectedStrengths{ 0.0, 0.03125, 0.125, 0.5, 1.0, 2.0 };
        double previous = -1.0;
        for (size_t index = 0; index < expectedStrengths.size(); ++index) {
            const auto& material = gltf.at("materials").at(index);
            CHECK(material.at("alphaMode") == "OPAQUE");
            CHECK(material.at("doubleSided").get<bool>());
            CHECK(material.at("emissiveFactor") == nlohmann::json::array({ 1.0, 1.0, 1.0 }));
            const double strength = material.at("extensions")
                .at("KHR_materials_emissive_strength").at("emissiveStrength").get<double>();
            CHECK(std::abs(strength - expectedStrengths[index]) < 0.000001);
            CHECK(strength > previous);
            previous = strength;

            const auto& primitive = gltf.at("meshes").at(index).at("primitives").at(0);
            CHECK(gltf.at("meshes").at(index).at("primitives").size() == 1);
            CHECK(primitive.at("material").get<size_t>() == index);
            CHECK(gltf.at("nodes").at(index).at("mesh").get<size_t>() == index);
        }
        return true;
    }

    bool testRepeatedLoadsAreIdentical() {
        const BenchmarkManifest firstManifest = loadBenchmarkManifest(manifestPath());
        const BenchmarkFixture firstCopy = findBenchmarkFixture(
            firstManifest, "material_lab_v1");
        const BenchmarkManifest secondManifest = loadBenchmarkManifest(manifestPath());
        const BenchmarkFixture& second = findBenchmarkFixture(secondManifest,
            "material_lab_v1");
        CHECK(firstCopy.id == second.id);
        CHECK(firstCopy.revision == second.revision);
        CHECK(firstCopy.sourceAsset == second.sourceAsset);
        CHECK(firstCopy.camera.position == second.camera.position);
        CHECK(firstCopy.camera.target == second.camera.target);
        CHECK(firstCopy.contentFiles[0].sha256 == second.contentFiles[0].sha256);
        return true;
    }

    bool testM1ColorVolumeFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(m1ManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(manifest,
            "color_volume_transparency_v1");
        CHECK(fixture.required);
        CHECK(fixture.outputLabel == "scene_linear_acescg_ap1");
        CHECK(sha256File(fixture.contentFiles[0].path) ==
            fixture.contentFiles[0].sha256);
        CHECK(containsText(fixture.expectedBehavior, "above 1.0"));
        const nlohmann::json gltf = loadJson(fixture.sourceAsset);
        CHECK(gltf.at("materials").size() == 13);
        CHECK(gltf.at("materials").at(0).at("alphaMode") == "OPAQUE");
        CHECK(gltf.at("materials").at(12).at("alphaMode") == "BLEND");
        CHECK(gltf.at("materials").at(12).at("extensions")
            .at("KHR_materials_transmission").at("transmissionFactor") == 1.0);
        CHECK(gltf.at("materials").at(12).at("extensions")
            .at("KHR_materials_emissive_strength").at("emissiveStrength") == 2.0);
        CHECK(gltf.at("materials").at(0).at("emissiveFactor") ==
            nlohmann::json::array({ 0.01, 0.01, 0.01 }));
        CHECK(gltf.at("materials").at(3).at("extensions")
            .at("KHR_materials_emissive_strength").at("emissiveStrength") == 8.0);
        CHECK(gltf.at("materials").at(4).at("emissiveFactor") ==
            nlohmann::json::array({ 1.0, 0.0, 0.0 }));
        CHECK(gltf.at("materials").at(8).at("extensions")
            .at("KHR_materials_emissive_strength").at("emissiveStrength") == 8.0);
        CHECK(gltf.at("nodes").size() == 13);
        return true;
    }

    bool testM2FixtureMatrixContract() {
        const nlohmann::json matrix = loadJson(m2FixtureMatrixPath());
        CHECK(matrix.at("schema_version") == 1);
        CHECK(matrix.at("milestone") == "M2");
        CHECK(matrix.at("source_baseline") ==
            "30252593f8fdd2de5dffbb8da31bb570ff49a7c0");
        CHECK(matrix.at("color_domain") == "scene_linear_acescg_ap1");

        std::set<std::string> requiredAxes;
        for (const auto& axis : matrix.at("required_axes")) {
            CHECK(requiredAxes.insert(axis.get<std::string>()).second);
        }
        CHECK(requiredAxes.size() == 14);

        std::set<std::string> coveredAxes;
        std::set<std::string> caseIds;
        for (const auto& fixture : matrix.at("cases")) {
            CHECK(caseIds.insert(fixture.at("id").get<std::string>()).second);
            CHECK(!fixture.at("expected_current").get<std::string>().empty());
            CHECK(!fixture.at("expected_m2").get<std::string>().empty());
            const std::string availability = fixture.at("availability");
            CHECK(availability == "existing_runtime" ||
                availability == "existing_source_only" ||
                availability == "contract_frozen_fixture_pending" ||
                availability == "optional_local");
            for (const auto& axis : fixture.at("axes")) {
                coveredAxes.insert(axis.get<std::string>());
            }
            if (fixture.contains("source_asset")) {
                const std::filesystem::path source = projectContentPath(
                    fixture.at("source_asset").get<std::string>());
                CHECK(std::filesystem::is_regular_file(source));
                CHECK(sha256File(source) ==
                    fixture.at("source_sha256").get<std::string>());
            }
            else {
                CHECK(availability == "contract_frozen_fixture_pending");
                CHECK(!fixture.at("source_contract").get<std::string>().empty());
            }
        }
        CHECK(caseIds.size() == 8);
        CHECK(coveredAxes == requiredAxes);
        CHECK(caseIds.contains("sample_car_local_v1"));
        return true;
    }

    bool testM2RunManifestContract() {
        const nlohmann::json manifest = loadJson(m2RunManifestPath());
        CHECK(manifest.at("schema_version") == 1);
        CHECK(manifest.at("milestone") == "M2.10");
        CHECK(manifest.at("resolution") == nlohmann::json::array({ 3840, 2160 }));
        CHECK(manifest.at("warmup_frames") == 500);
        CHECK(manifest.at("measured_frames") == 10000);
        CHECK(manifest.at("production_renderer")
            .at("render_graph_unconditional").get<bool>());
        CHECK(manifest.at("production_renderer")
            .at("gbuffer_layout_default") == "reference");
        CHECK(manifest.at("production_renderer")
            .at("deprecated_arguments_rejected").size() == 3);

        std::set<std::string> runIds;
        for (const auto& run : manifest.at("required_runs")) {
            CHECK(runIds.insert(run.at("id").get<std::string>()).second);
            const std::vector<std::string> arguments = run.at("arguments");
            CHECK(containsText(arguments, "--window-size"));
            CHECK(containsText(arguments, "3840x2160"));
            CHECK(containsText(arguments, "--profile-cpu-output"));
            CHECK(!run.at("required_artifacts").empty());
            if (run.at("build") == "x64-release") {
                CHECK(run.at("warmup_frames") == 500);
                CHECK(run.at("measured_frames") == 10000);
            }
            else {
                CHECK(run.at("warmup_frames") == 8);
                CHECK(run.at("measured_frames") == 1);
            }
        }
        CHECK(runIds.size() == 4);
        CHECK(runIds.contains("reference_release_4k_10000_v1"));
        CHECK(runIds.contains("compact_release_4k_10000_experiment_v1"));
        CHECK(runIds.contains("reference_debug_validation_scene_capture_v1"));
        CHECK(runIds.contains("reference_debug_validation_final_sdr_capture_v1"));
        CHECK(manifest.at("required_debug_views").size() == 6);
        CHECK(manifest.at("optional_local_runs").size() == 1);
        return true;
    }

    bool testM2MaterialGpuFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m2MaterialGpuManifestPath());
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "material_gpu_lab_v1");
        CHECK(fixture.revision == 1);
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(fixture.contentFiles[0].sha256 ==
            "f44ae95deb2ecbd0c7417d2a1976b67ee989040935872a4f63def463e029ffb5");
        return true;
    }

    bool testTrackedFixtureSidecars() {
        const std::array manifests{
            manifestPath(),
            m1ManifestPath(),
            m2MaterialGpuManifestPath(),
        };
        std::set<std::filesystem::path> sources;
        std::set<std::string> guids;
        for (const std::filesystem::path& path :
            manifests) {
            for (const BenchmarkFixture& fixture :
                loadBenchmarkManifest(path).fixtures) {
                sources.insert(fixture.sourceAsset);
            }
        }
        CHECK(sources.size() == 8);
        for (const std::filesystem::path& source :
            sources) {
            const std::filesystem::path sidecar =
                source.string() + ".iridium.meta";
            CHECK(std::filesystem::is_regular_file(sidecar));
            const nlohmann::json metadata =
                loadJson(sidecar);
            CHECK(metadata.at("schemaVersion") == 1);
            CHECK(metadata.at("assetType") ==
                "iridium.model");
            CHECK(metadata.at("importer").at("id") ==
                "iridium.gltf-model");
            CHECK(metadata.at("importer").at("version") ==
                7);
            CHECK(metadata.at("settings").at("values")
                .at("import_scale") == 1.0);
            const std::string rootGuid =
                metadata.at("assetGuid");
            CHECK(rootGuid.size() == 36);
            CHECK(rootGuid[14] == '7');
            CHECK(guids.insert(rootGuid).second);
            for (const nlohmann::json& subasset :
                metadata.at("subassets")) {
                const std::string guid =
                    subasset.at("guid");
                CHECK(guid.size() == 36);
                CHECK(guid[14] == '7');
                CHECK(guids.insert(guid).second);
                CHECK(!subasset.at("sourceKey")
                    .get<std::string>().empty());
                CHECK(subasset.at(
                    "structuralFingerprint")
                    .get<std::string>().size() == 64);
            }
        }
        return true;
    }

    bool testUnknownFixtureFails() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(manifestPath());
        try {
            (void)findBenchmarkFixture(manifest, "missing");
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    }

    bool testM6PyramidFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m6PyramidManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(manifest,
            "m6_rough_metric_refraction_v1");
        CHECK(fixture.revision == 1);
        CHECK(fixture.required);
        CHECK(fixture.camera.id == "oblique_emissive_grid_v1");
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(containsText(fixture.expectedBehavior,
            "0.1 metre authored sheet thickness"));
        CHECK(containsText(fixture.expectedBehavior,
            "one frozen AP1 color pyramid"));

        const nlohmann::json metadata = loadJson(
            fixture.sourceAsset.string() + ".iridium.meta");
        CHECK(metadata.at("settings").at("schemaVersion") == 2);
        const nlohmann::json& values = metadata.at("settings").at("values");
        CHECK(values.at("transparency_execution_mode") == "classified");
        const nlohmann::json& policy = values.at("transparency_policies").at(
            "019ffd80-0000-7004-8000-000000000005");
        CHECK(policy.at("class") == "thin_glass");
        CHECK(policy.at("quality") == "ordinary2");
        CHECK(std::abs(policy.at("thin_sheet_thickness_m").get<double>() - 0.1) <
            1.0e-9);
        return true;
    }

    bool testM6Ordinary2RuntimeFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m6Ordinary2RuntimeManifestPath());
        CHECK(manifest.fixtures.size() == 14);
        const BenchmarkFixture& ordinary = findBenchmarkFixture(manifest,
            "ordinary2_lit_closed_v1");
        const BenchmarkFixture& mirrored = findBenchmarkFixture(manifest,
            "ordinary2_lit_closed_mirrored_v1");
        const BenchmarkFixture& populated = findBenchmarkFixture(manifest,
            "ordinary2_lit_populated_grid_v1");
        const BenchmarkFixture& invalid = findBenchmarkFixture(manifest,
            "ordinary2_invalid_open_fallback_v1");
        const BenchmarkFixture& nested = findBenchmarkFixture(manifest,
            "hero4_nested_tetrahedra_v1");
        const BenchmarkFixture& crossing = findBenchmarkFixture(manifest,
            "hero4_crossing_tetrahedra_v1");
        const BenchmarkFixture& earlyTermination = findBenchmarkFixture(
            manifest, "hero4_early_termination_tetrahedra_v1");
        const BenchmarkFixture& cinematic = findBenchmarkFixture(manifest,
            "cinematic8_nested_tetrahedra_v1");
        const BenchmarkFixture& overflow = findBenchmarkFixture(manifest,
            "cinematic8_overflow_residual_v1");
        const BenchmarkFixture& mixed = findBenchmarkFixture(manifest,
            "mixed_deep_tier_tetrahedra_v1");
        const BenchmarkFixture& weighted = findBenchmarkFixture(manifest,
            "weighted_oit_particles_v1");
        const BenchmarkFixture& weighted4096 = findBenchmarkFixture(manifest,
            "weighted_oit_particles_4096_v1");
        const BenchmarkFixture& weighted65536 = findBenchmarkFixture(manifest,
            "weighted_oit_particles_65536_v1");
        const BenchmarkFixture& weightedHdr256 = findBenchmarkFixture(manifest,
            "weighted_oit_particles_hdr256_v1");
        CHECK(ordinary.sceneFactory.instanceScale == glm::vec3(1.0f));
        CHECK(mirrored.sceneFactory.instanceScale ==
            glm::vec3(-1.0f, 1.0f, 1.0f));
        CHECK(ordinary.sourceAsset == mirrored.sourceAsset);
        CHECK(ordinary.contentFiles.size() == 2);
        CHECK(mirrored.contentFiles.size() == 2);
        CHECK(containsText(mirrored.expectedBehavior,
            "negative runtime world-transform determinant"));
        CHECK(populated.sourceAsset == ordinary.sourceAsset);
        CHECK(populated.sceneFactory.instanceGrid == glm::uvec3(4, 2, 1));
        CHECK(populated.sceneFactory.instanceSpacing ==
            glm::vec3(0.50f, 0.50f, 0.0f));
        CHECK(populated.measuredFrames == 8);
        CHECK(containsText(populated.expectedBehavior,
            "960x540, 1600x900, and restored 1280x720"));
        CHECK(containsText(populated.expectedBehavior,
            "without compatibility draws"));
        CHECK(invalid.sceneFactory.instanceScale == glm::vec3(1.0f));
        CHECK(invalid.sourceAsset != ordinary.sourceAsset);
        CHECK(invalid.contentFiles.size() == 2);
        CHECK(containsText(invalid.expectedBehavior,
            "fallback-applied flag"));
        CHECK(nested.sceneFactory.instanceScale == glm::vec3(1.0f));
        CHECK(nested.contentFiles.size() == 2);
        CHECK(nested.measuredFrames == 4);
        CHECK(containsText(nested.expectedBehavior,
            "Entry(outer), Entry(inner), Exit(inner), Exit(outer)"));
        CHECK(containsText(nested.expectedBehavior,
            "finite premultiplied AP1 color"));
        const nlohmann::json nestedMetadata = loadJson(
            nested.sourceAsset.string() + ".iridium.meta");
        CHECK(nestedMetadata.at("settings").at("values").at(
            "transparency_execution_mode") == "classified");
        const auto& nestedPolicies = nestedMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(nestedPolicies.size() == 1);
        CHECK(nestedPolicies.begin().value().at("class") ==
            "layered_glass");
        CHECK(nestedPolicies.begin().value().at("quality") == "hero4");
        CHECK(crossing.contentFiles.size() == 2);
        CHECK(crossing.measuredFrames == 4);
        CHECK(containsText(crossing.expectedBehavior,
            "Entry(A), Entry(B), Exit(A), Exit(B)"));
        CHECK(containsText(crossing.expectedBehavior,
            "stable-identity crossing pairing"));
        const nlohmann::json crossingSource = loadJson(
            crossing.sourceAsset);
        CHECK(crossingSource.at("nodes").size() == 2);
        const nlohmann::json crossingMetadata = loadJson(
            crossing.sourceAsset.string() + ".iridium.meta");
        const auto& crossingPolicies = crossingMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(crossingPolicies.size() == 1);
        CHECK(crossingPolicies.begin().value().at("quality") == "hero4");
        CHECK(earlyTermination.contentFiles.size() == 2);
        CHECK(earlyTermination.measuredFrames == 4);
        CHECK(containsText(earlyTermination.expectedBehavior,
            "remaining transmission below 1/1024"));
        CHECK(containsText(earlyTermination.expectedBehavior,
            "suppress the deeper shell"));
        const nlohmann::json earlyTerminationSource = loadJson(
            earlyTermination.sourceAsset);
        CHECK(earlyTerminationSource.at("nodes").size() == 2);
        CHECK(earlyTerminationSource.at("materials").at(0).at(
            "extensions").at("KHR_materials_transmission").at(
                "transmissionFactor") == 0.0005);
        const nlohmann::json earlyTerminationMetadata = loadJson(
            earlyTermination.sourceAsset.string() + ".iridium.meta");
        const auto& earlyTerminationPolicies = earlyTerminationMetadata.at(
            "settings").at("values").at("transparency_policies");
        CHECK(earlyTerminationPolicies.size() == 1);
        CHECK(earlyTerminationPolicies.begin().value().at("quality") ==
            "hero4");
        CHECK(cinematic.contentFiles.size() == 2);
        CHECK(cinematic.measuredFrames == 4);
        CHECK(containsText(cinematic.expectedBehavior,
            "all eight interfaces"));
        const nlohmann::json cinematicMetadata = loadJson(
            cinematic.sourceAsset.string() + ".iridium.meta");
        const auto& cinematicPolicies = cinematicMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(cinematicPolicies.size() == 1);
        CHECK(cinematicPolicies.begin().value().at("quality") ==
            "cinematic8");
        CHECK(overflow.contentFiles.size() == 2);
        CHECK(overflow.measuredFrames == 4);
        CHECK(containsText(overflow.expectedBehavior,
            "stores exactly eight"));
        CHECK(containsText(overflow.expectedBehavior,
            "non-refractive residual operator"));
        const nlohmann::json overflowSource = loadJson(
            overflow.sourceAsset);
        CHECK(overflowSource.at("nodes").size() == 5);
        const nlohmann::json overflowMetadata = loadJson(
            overflow.sourceAsset.string() + ".iridium.meta");
        const auto& overflowPolicies = overflowMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(overflowPolicies.size() == 1);
        CHECK(overflowPolicies.begin().value().at("quality") ==
            "cinematic8");
        CHECK(mixed.sourceAsset == cinematic.sourceAsset);
        CHECK(mixed.contentFiles.size() == 2);
        CHECK(containsText(mixed.expectedBehavior,
            "global transparent order"));
        const nlohmann::json mixedMetadata = loadJson(
            mixed.contentFiles[1].path);
        const auto& mixedPolicies = mixedMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(mixedPolicies.size() == 3);
        uint32_t hero4Policies = 0u;
        uint32_t cinematic8Policies = 0u;
        for (const auto& [guid, policy] : mixedPolicies.items()) {
            (void)guid;
            if (policy.at("quality") == "hero4") ++hero4Policies;
            if (policy.at("quality") == "cinematic8")
                ++cinematic8Policies;
        }
        CHECK(hero4Policies == 2u);
        CHECK(cinematic8Policies == 1u);
        CHECK(weighted.contentFiles.size() == 2);
        CHECK(weighted.sceneFactory.instanceGrid == glm::uvec3(8, 8, 4));
        CHECK(weighted.sceneFactory.instanceSpacing ==
            glm::vec3(0.17f, 0.17f, 0.06f));
        CHECK(weighted.measuredFrames == 8);
        CHECK(containsText(weighted.expectedBehavior,
            "256 explicit particles"));
        CHECK(containsText(weighted.expectedBehavior,
            "does not activate refraction pyramids"));
        CHECK(containsText(weighted.expectedBehavior,
            "Seeds 0 through 63"));
        CHECK(weighted.unavailableCapabilities.empty());
        CHECK(weighted4096.sourceAsset == weighted.sourceAsset);
        CHECK(weighted4096.sceneFactory.instanceGrid ==
            glm::uvec3(16, 16, 16));
        CHECK(weighted4096.sceneFactory.renderInstanceBatch);
        CHECK(weighted4096.sceneFactory.instanceSpacing ==
            glm::vec3(0.085f, 0.085f, 0.015f));
        CHECK(weighted4096.sceneFactory.instanceScale == glm::vec3(0.5f));
        CHECK(benchmarkInstanceCount(
            weighted4096.sceneFactory.instanceGrid) == 4'096u);
        CHECK(containsText(weighted4096.expectedBehavior,
            "262144 bytes"));
        CHECK(weighted65536.sourceAsset == weighted.sourceAsset);
        CHECK(weighted65536.sceneFactory.instanceGrid ==
            glm::uvec3(32, 32, 64));
        CHECK(weighted65536.sceneFactory.renderInstanceBatch);
        CHECK(weighted65536.sceneFactory.instanceSpacing ==
            glm::vec3(0.0425f, 0.0425f, 0.00375f));
        CHECK(weighted65536.sceneFactory.instanceScale == glm::vec3(0.25f));
        CHECK(benchmarkInstanceCount(
            weighted65536.sceneFactory.instanceGrid) == 65'536u);
        CHECK(containsText(weighted65536.expectedBehavior,
            "4194304 bytes"));
        const nlohmann::json weightedSource = loadJson(
            weighted.sourceAsset);
        CHECK(weightedSource.at("materials").at(0).at("alphaMode") ==
            "BLEND");
        CHECK(weightedSource.at("materials").at(0).at("extensions").at(
            "KHR_materials_emissive_strength").at("emissiveStrength") ==
            16.0);
        const nlohmann::json weightedMetadata = loadJson(
            weighted.sourceAsset.string() + ".iridium.meta");
        CHECK(weightedMetadata.at("importer").at("version") == 7);
        CHECK(weightedMetadata.at("settings").at("values").at(
            "transparency_execution_mode") == "classified");
        const auto& weightedPolicies = weightedMetadata.at("settings").at(
            "values").at("transparency_policies");
        CHECK(weightedPolicies.size() == 1);
        CHECK(weightedPolicies.begin().value().at("class") ==
            "weighted_oit");
        CHECK(weightedHdr256.contentFiles.size() == 2);
        CHECK(weightedHdr256.sceneFactory.instanceGrid ==
            glm::uvec3(8, 8, 4));
        CHECK(weightedHdr256.measuredFrames == 1);
        const nlohmann::json weightedHdr256Source = loadJson(
            weightedHdr256.sourceAsset);
        CHECK(weightedHdr256Source.at("materials").at(0).at(
            "extensions").at("KHR_materials_emissive_strength").at(
                "emissiveStrength") == 256.0);
        const nlohmann::json weightedHdr256Metadata = loadJson(
            weightedHdr256.sourceAsset.string() + ".iridium.meta");
        CHECK(weightedHdr256Metadata.at("importer").at("version") == 7);
        const auto& weightedHdr256Policies = weightedHdr256Metadata.at(
            "settings").at("values").at("transparency_policies");
        CHECK(weightedHdr256Policies.size() == 1);
        CHECK(weightedHdr256Policies.begin().value().at("class") ==
            "weighted_oit");
        return true;
    }

    bool testInstanceCountOverflowIsRejected() {
        CHECK(benchmarkInstanceCount(glm::uvec3(16, 8, 1)) == 128);
        CHECK(benchmarkInstanceCount(glm::uvec3(0, 1, 1)) == 0);
        CHECK(benchmarkInstanceCount(glm::uvec3(
            0xffffffffu, 0xffffffffu, 1u)) == 0);
        return true;
    }

    bool testM7ThreeDenseFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7ThreeDenseManifestPath());
        CHECK(manifest.fixtures.size() == 6);
        const BenchmarkFixture& allVisible = findBenchmarkFixture(
            manifest, "m7_three_dense_all_visible_v1");
        const BenchmarkFixture& oneVisible = findBenchmarkFixture(
            manifest, "m7_three_dense_one_visible_v1");
        const BenchmarkFixture& depthRange = findBenchmarkFixture(
            manifest, "m7_three_dense_near_mid_far_v1");
        const BenchmarkFixture& staticFixture = findBenchmarkFixture(
            manifest, "m7_three_dense_static_v1");
        const BenchmarkFixture& moving = findBenchmarkFixture(
            manifest, "m7_three_dense_moving_v1");
        const BenchmarkFixture& stress = findBenchmarkFixture(
            manifest, "m7_many_instance_stress_v1");
        CHECK(allVisible.sceneFactory.instanceGrid == glm::uvec3(3, 1, 1));
        CHECK(allVisible.sceneFactory.instanceSpacing == glm::vec3(3.0f, 0.0f, 0.0f));
        CHECK(allVisible.contentFiles.size() == 2);
        CHECK(oneVisible.camera.verticalFovDegrees == 12.0f);
        CHECK(depthRange.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(depthRange.sceneFactory.instanceSpacing == glm::vec3(0.0f, 0.0f, 8.0f));
        CHECK(!staticFixture.sceneFactory.animateInstances);
        CHECK(moving.sceneFactory.animateInstances);
        CHECK(moving.sceneFactory.motionPeriodFrames == 240);
        CHECK(benchmarkInstanceCount(stress.sceneFactory.instanceGrid) == 256);
        CHECK(containsText(allVisible.expectedBehavior,
            "replacement for the unsaved owner-observed"));
        CHECK(containsText(staticFixture.expectedBehavior,
            "exactly zero"));

        const nlohmann::json matrix = loadJson(m7FixtureMatrixPath());
        CHECK(matrix.at("schema_version") == 1);
        CHECK(matrix.at("cases").size() == 16);
        std::set<std::string> ids;
        for (const auto& fixtureCase : matrix.at("cases")) {
            CHECK(ids.insert(fixtureCase.at("id").get<std::string>()).second);
            CHECK(!fixtureCase.at("purpose").get<std::string>().empty());
            CHECK(!fixtureCase.at("availability").get<std::string>().empty());
        }
        CHECK(ids.contains("large_occluder"));
        CHECK(ids.contains("shadow_only_caster"));
        CHECK(ids.contains("probe_only_off_camera"));
        CHECK(ids.contains("representative_m6_transparency"));
        CHECK(ids.contains("oversized_model_publication"));

        const nlohmann::json runManifest = loadJson(m7RunManifestPath());
        CHECK(runManifest.at("milestone_slice") == "M7.0");
        CHECK(runManifest.at("profiles").size() == 5);
        CHECK(runManifest.at("aggregate").at("steady_cpp_allocation_calls") == 0);
        CHECK(runManifest.at("validation_capture").at(
            "validation_messages_observed") == 0);
        return true;
    }

    bool testM7OcclusionTemporalFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7OcclusionTemporalManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_occlusion_depth_stack_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 4));
        CHECK(fixture.sceneFactory.instanceSpacing ==
            glm::vec3(0.0f, 0.0f, 4.0f));
        CHECK(fixture.sceneFactory.cameraCutEnabled);
        CHECK(fixture.sceneFactory.cameraCutFrame == 12);
        CHECK(evaluateBenchmarkCamera(fixture, 11).position ==
            glm::vec3(0.0f, 1.2f, 16.0f));
        CHECK(evaluateBenchmarkCamera(fixture, 12).position ==
            glm::vec3(12.0f, 2.5f, 12.0f));
        CHECK(containsText(fixture.expectedBehavior, "fail visible"));
        CHECK(containsText(fixture.expectedBehavior, "never removes geometry"));
        return true;
    }

    bool testM7OcclusionMotionFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7OcclusionMotionManifestPath());
        CHECK(manifest.fixtures.size() == 2);
        const BenchmarkFixture& occluder = findBenchmarkFixture(
            manifest, "m7_occluder_disocclusion_step_v1");
        CHECK(occluder.sceneFactory.objectStepEnabled);
        CHECK(occluder.sceneFactory.objectStepInstanceIndex == 3);
        CHECK(occluder.sceneFactory.objectStepFrame == 12);
        CHECK(evaluateBenchmarkInstanceOffset(
            occluder.sceneFactory, 11, 3) == glm::vec3(0.0f));
        CHECK(evaluateBenchmarkInstanceOffset(
            occluder.sceneFactory, 12, 3) == glm::vec3(6.0f, 0.0f, 0.0f));
        CHECK(evaluateBenchmarkInstanceOffset(
            occluder.sceneFactory, 12, 2) == glm::vec3(0.0f));

        const BenchmarkFixture& occludee = findBenchmarkFixture(
            manifest, "m7_occludee_reveal_step_v1");
        CHECK(occludee.sceneFactory.objectStepInstanceIndex == 0);
        CHECK(evaluateBenchmarkInstanceOffset(
            occludee.sceneFactory, 12, 0) == glm::vec3(-6.0f, 0.0f, 0.0f));
        CHECK(containsText(occluder.expectedBehavior, "fail visible"));
        CHECK(containsText(occludee.expectedBehavior, "fail visible"));
        return true;
    }

    bool testM7OcclusionSmallObjectFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7OcclusionSmallObjectManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_occlusion_subpixel_object_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 1));
        CHECK(fixture.sceneFactory.instanceScale == glm::vec3(0.001f));
        CHECK(containsText(fixture.expectedBehavior, "SmallBounds"));
        CHECK(containsText(fixture.expectedBehavior, "fail visible"));
        return true;
    }

    bool testM7OcclusionDepthContentFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7OcclusionDepthContentManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_occluder_visibility_step_v1");
        CHECK(fixture.sceneFactory.objectVisibilityStepEnabled);
        CHECK(fixture.sceneFactory.objectVisibilityStepInstanceIndex == 3);
        CHECK(fixture.sceneFactory.objectVisibilityStepFrame == 12);
        CHECK(!fixture.sceneFactory.objectVisibilityAfterStep);
        CHECK(containsText(fixture.expectedBehavior, "DepthContentChanged"));
        return true;
    }

    bool testM7OcclusionPerformanceFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7OcclusionPerformanceManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_occlusion_dense_depth_stack_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 64));
        CHECK(fixture.sceneFactory.instanceSpacing ==
            glm::vec3(0.0f, 0.0f, 4.0f));
        CHECK(fixture.sceneFactory.instanceScale == glm::vec3(1.0f));
        CHECK(fixture.sceneFactory.instanceScaleOverrideEnabled);
        CHECK(fixture.sceneFactory.instanceScaleOverrideIndex == 63);
        CHECK(fixture.sceneFactory.instanceScaleOverride == glm::vec3(3.0f));
        CHECK(benchmarkInstanceCount(fixture.sceneFactory.instanceGrid) == 64);
        CHECK(fixture.camera.position == glm::vec3(0.0f, 1.2f, 150.0f));
        CHECK(fixture.camera.farPlane == 400.0f);
        CHECK(fixture.warmupFrames == 100);
        CHECK(fixture.measuredFrames == 500);
        CHECK(containsText(fixture.expectedBehavior, "most farther opaque"));
        CHECK(containsText(fixture.expectedBehavior, "identical scene output"));
        return true;
    }

    bool testM7LodObliqueFixtureContract() {
        const auto manifest = loadBenchmarkManifest(std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "m7-lod-validation-manifest.v1.json");
        CHECK(manifest.fixtures.size() == 1);
        const auto& fixture = findBenchmarkFixture(manifest, "m7_lod_oblique_depth_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(0, 0, 12));
        CHECK(fixture.camera.position == glm::vec3(18, 8, 28));
        CHECK(fixture.contentFiles.size() == 2);
        CHECK(containsText(fixture.expectedBehavior, "without replacing or rewriting"));
        return true;
    }

    bool testM7LodHistoryFixtureContract() {
        const auto manifest = loadBenchmarkManifest(std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets" / "m7-lod-history-manifest.v1.json");
        const auto& fixture = findBenchmarkFixture(manifest, "m7_lod_history_oscillation_cut_v1");
        CHECK(fixture.sceneFactory.cameraOscillationAmplitude == glm::vec3(0, 0, 6));
        CHECK(fixture.sceneFactory.cameraOscillationPeriodFrames == 24);
        CHECK(evaluateBenchmarkCamera(fixture, 0).position == glm::vec3(18, 8, 28));
        CHECK(evaluateBenchmarkCamera(fixture, 6).position == glm::vec3(18, 8, 34));
        CHECK(evaluateBenchmarkCamera(fixture, 18).position == glm::vec3(18, 8, 22));
        CHECK(evaluateBenchmarkCamera(fixture, 24).position == evaluateBenchmarkCamera(fixture, 0).position);
        CHECK(evaluateBenchmarkCamera(fixture, 120).position == glm::vec3(18, 8, 22));
        CHECK(evaluateBenchmarkCamera(fixture, 126).position == glm::vec3(18, 8, 28));
        CHECK(evaluateBenchmarkCamera(fixture, 138).position == glm::vec3(18, 8, 16));
        return true;
    }

    bool testM7LodLitAdmissionFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7LodLitAdmissionManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_lod_lit_oblique_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(fixture.lights.size() == 3);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Spot);
        CHECK(fixture.lights[0].shadowQuality ==
            BenchmarkShadowQuality::Ultra);
        CHECK(fixture.lights[0].castsShadows);
        CHECK(fixture.lights[0].priority == 3);
        CHECK(fixture.lights[1].type == BenchmarkLightType::Point);
        CHECK(!fixture.lights[1].castsShadows);
        CHECK(fixture.lights[2].type == BenchmarkLightType::Directional);
        CHECK(fixture.lights[2].illuminanceLux == 30'000.0f);
        CHECK(fixture.constantEnvironmentLinear ==
            glm::vec3(0.012f, 0.016f, 0.024f));
        CHECK(containsText(fixture.expectedBehavior, "image-space error"));
        return true;
    }

    bool testM7DirectionalShadowGrazingFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7DirectionalShadowGrazingManifestPath());
        CHECK(manifest.fixtures.size() == 3);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_directional_shadow_grazing_v1");
        CHECK(fixture.lights.size() == 1);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Directional);
        CHECK(fixture.lights[0].rotationDegrees == glm::vec3(0, 110, 0));
        CHECK(fixture.lights[0].shadowQuality ==
            BenchmarkShadowQuality::Ultra);
        CHECK(fixture.lights[0].castsShadows);
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(sha256File(fixture.contentFiles[0].path) ==
            fixture.contentFiles[0].sha256);
        CHECK(containsText(fixture.expectedBehavior, "Receiver-plane"));
        CHECK(containsText(fixture.expectedBehavior, "front-facing"));
        const BenchmarkFixture& culling = findBenchmarkFixture(
            manifest, "m7_directional_shadow_caster_culling_v1");
        CHECK(culling.sceneFactory.instanceGrid == glm::uvec3(16, 1, 1));
        CHECK(culling.sceneFactory.instanceSpacing == glm::vec3(12, 0, 0));
        CHECK(culling.sceneFactory.animateInstances);
        CHECK(culling.lights.size() == 1);
        CHECK(culling.lights[0].type == BenchmarkLightType::Directional);
        CHECK(culling.lights[0].castsShadows);
        CHECK(containsText(culling.expectedBehavior, "conservatively rejected"));
        CHECK(containsText(culling.expectedBehavior, "Camera-invisible"));
        CHECK(containsText(culling.expectedBehavior, "fail-visible"));
        const BenchmarkFixture& affected = findBenchmarkFixture(
            manifest, "m7_directional_shadow_affected_bounds_v1");
        CHECK(affected.sceneFactory.objectStepEnabled);
        CHECK(affected.sceneFactory.objectStepInstanceIndex == 0u);
        CHECK(affected.sceneFactory.objectStepFrame == 10u);
        CHECK(affected.sceneFactory.objectStepOffset == glm::vec3(90, 0, 0));
        CHECK(!affected.sceneFactory.animateInstances);
        CHECK(containsText(affected.expectedBehavior, "Unaffected cascades"));
        return true;
    }

    bool testM7DirectionalShadowLodFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7DirectionalShadowLodManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_directional_shadow_lod_near_mid_far_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(0, 0, 8));
        CHECK(fixture.lights.size() == 1);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Directional);
        CHECK(fixture.lights[0].shadowQuality ==
            BenchmarkShadowQuality::Ultra);
        CHECK(fixture.lights[0].castsShadows);
        CHECK(fixture.contentFiles.size() == 2);
        for (const BenchmarkContentFile& content : fixture.contentFiles)
            CHECK(sha256File(content.path) == content.sha256);
        CHECK(containsText(fixture.expectedBehavior,
            "cascade world-units-per-texel"));
        CHECK(containsText(fixture.expectedBehavior,
            "complete device-generated command multiset"));
        return true;
    }

    bool testM7PointShadowLodFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7PointShadowLodManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_point_shadow_lod_near_mid_far_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(0, 0, 12));
        CHECK(fixture.lights.size() == 1);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Point);
        CHECK(fixture.lights[0].shadowQuality == BenchmarkShadowQuality::High);
        CHECK(fixture.lights[0].castsShadows);
        CHECK(fixture.lights[0].rangeMeters == 60.0f);
        CHECK(fixture.contentFiles.size() == 2);
        for (const BenchmarkContentFile& content : fixture.contentFiles)
            CHECK(sha256File(content.path) == content.sha256);
        CHECK(containsText(fixture.expectedBehavior, "face-invariant radial LOD"));
        CHECK(containsText(fixture.expectedBehavior,
            "complete device-generated point-shadow command multisets"));
        return true;
    }

    bool testM7SpotShadowLodFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7SpotShadowLodManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_spot_shadow_lod_near_mid_far_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(1, 1, 3));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(0, 0, 12));
        CHECK(fixture.lights.size() == 1);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Spot);
        CHECK(fixture.lights[0].shadowQuality == BenchmarkShadowQuality::High);
        CHECK(fixture.lights[0].castsShadows);
        CHECK(fixture.lights[0].rangeMeters == 60.0f);
        CHECK(fixture.contentFiles.size() == 2);
        for (const BenchmarkContentFile& content : fixture.contentFiles)
            CHECK(sha256File(content.path) == content.sha256);
        CHECK(containsText(fixture.expectedBehavior,
            "spotlight clip volume"));
        CHECK(containsText(fixture.expectedBehavior,
            "complete device-generated spot-shadow command multisets"));
        return true;
    }

    bool testM7HeterogeneousShadowAdmissionFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7HeterogeneousShadowAdmissionManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_heterogeneous_shadow_warm_motion_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(8, 1, 4));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(4, 0, 4));
        CHECK(fixture.sceneFactory.animateInstances);
        CHECK(fixture.sceneFactory.motionAmplitude == 0.15f);
        CHECK(fixture.sceneFactory.motionPeriodFrames == 120u);
        CHECK(fixture.lights.size() == 5);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Directional &&
                    light.castsShadows;
            }) == 1);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Spot &&
                    light.castsShadows;
            }) == 2);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Point &&
                    light.castsShadows;
            }) == 2);
        CHECK(fixture.warmupFrames == 120);
        CHECK(fixture.measuredFrames == 600);
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(sha256File(fixture.contentFiles[0].path) ==
            fixture.contentFiles[0].sha256);
        CHECK(containsText(fixture.expectedBehavior,
            "isolated conventional-shadow reference"));
        CHECK(containsText(fixture.expectedBehavior, "zero overflow"));
        return true;
    }

    bool testM7ProbeLodAdmissionFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7ProbeLodAdmissionManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_probe_lod_reflection_motion_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(3, 1, 1));
        CHECK(fixture.sceneFactory.animateInstances);
        CHECK(fixture.warmupFrames == 120);
        CHECK(fixture.measuredFrames == 600);
        CHECK(fixture.reflectionProbeCapture.has_value());
        const BenchmarkReflectionProbeCapture& probe =
            *fixture.reflectionProbeCapture;
        CHECK(probe.position == glm::vec3(0.0f, 3.0f, 4.0f));
        CHECK(probe.updateMode ==
            BenchmarkReflectionProbeUpdateMode::Realtime);
        CHECK(probe.resolution == 1'024u);
        CHECK(probe.nearPlane == 0.1f);
        CHECK(probe.farPlane == 150.0f);
        CHECK(probe.influenceRadiusMeters == 1'000.0f);
        CHECK(probe.priority == 2);
        CHECK(probe.captureSky);
        CHECK(fixture.contentFiles.size() == 2);
        CHECK(containsText(fixture.expectedBehavior,
            "standalone non-renderable owner"));
        CHECK(containsText(fixture.expectedBehavior,
            "realtime capture cadence"));
        return true;
    }

    bool testM7ShadowLodWarmedAdmissionFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7ShadowLodWarmedAdmissionManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_shadow_lod_warm_motion_v1");
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(4, 1, 2));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(7, 0, 9));
        CHECK(fixture.sceneFactory.animateInstances);
        CHECK(fixture.sceneFactory.motionAmplitude == 0.15f);
        CHECK(fixture.sceneFactory.motionPeriodFrames == 120u);
        CHECK(fixture.lights.size() == 5);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Directional &&
                    light.castsShadows;
            }) == 1);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Spot &&
                    light.castsShadows;
            }) == 2);
        CHECK(std::ranges::count_if(fixture.lights,
            [](const BenchmarkLight& light) {
                return light.type == BenchmarkLightType::Point &&
                    light.castsShadows;
            }) == 2);
        CHECK(fixture.warmupFrames == 120);
        CHECK(fixture.measuredFrames == 600);
        CHECK(fixture.contentFiles.size() == 2);
        for (const BenchmarkContentFile& content : fixture.contentFiles)
            CHECK(sha256File(content.path) == content.sha256);
        CHECK(containsText(fixture.expectedBehavior,
            "independently selects geometry"));
        CHECK(containsText(fixture.expectedBehavior, "zero overflow"));
        return true;
    }

    bool testM7ShadowQualityClosureFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m7ShadowQualityClosureManifestPath());
        CHECK(manifest.fixtures.size() == 1);
        const BenchmarkFixture& fixture = findBenchmarkFixture(
            manifest, "m7_shadow_quality_closure_v1");
        CHECK(fixture.camera.position == glm::vec3(0, 0, 28));
        CHECK(fixture.camera.farPlane == 180.0f);
        CHECK(fixture.sceneFactory.instanceGrid == glm::uvec3(3, 1, 1));
        CHECK(fixture.sceneFactory.instanceSpacing == glm::vec3(8, 0, 0));
        CHECK(fixture.sceneFactory.cameraOscillationAmplitude ==
            glm::vec3(0, 0, 20));
        CHECK(fixture.sceneFactory.cameraOscillationPeriodFrames == 120u);
        CHECK(evaluateBenchmarkCamera(fixture, 30).position ==
            glm::vec3(0, 0, 48));
        CHECK(evaluateBenchmarkCamera(fixture, 90).position ==
            glm::vec3(0, 0, 8));
        CHECK(fixture.lights.size() == 1);
        CHECK(fixture.lights[0].type == BenchmarkLightType::Directional);
        CHECK(fixture.lights[0].rotationDegrees == glm::vec3(0, 110, 0));
        CHECK(fixture.lights[0].shadowQuality ==
            BenchmarkShadowQuality::Ultra);
        CHECK(fixture.warmupFrames == 120u);
        CHECK(fixture.measuredFrames == 240u);
        CHECK(fixture.contentFiles.size() == 1);
        CHECK(sha256File(fixture.contentFiles[0].path) ==
            fixture.contentFiles[0].sha256);
        CHECK(containsText(fixture.expectedBehavior, "staircase-shaped"));
        CHECK(containsText(fixture.expectedBehavior, "fully clipped"));
        CHECK(containsText(fixture.expectedBehavior, "Double-sided"));
        CHECK(containsText(fixture.expectedBehavior, "peter-panning"));
        CHECK(containsText(fixture.expectedBehavior, "byte-identical"));
        return true;
    }

    // --- M9 G6b: composition scene factory, camera paths and the temporal set ---

    bool near(float lhs, float rhs, float tolerance = 1.0e-5f) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    bool near(const glm::vec3& lhs, const glm::vec3& rhs,
        float tolerance = 1.0e-5f) {
        return near(lhs.x, rhs.x, tolerance) && near(lhs.y, rhs.y, tolerance) &&
            near(lhs.z, rhs.z, tolerance);
    }

    // TransformComponent's convention: T * Rz * Ry * Rx.
    glm::mat3 eulerZyxMatrix(const glm::vec3& degrees) {
        glm::mat4 matrix(1.0f);
        matrix = glm::rotate(matrix, glm::radians(degrees.z), glm::vec3(0, 0, 1));
        matrix = glm::rotate(matrix, glm::radians(degrees.y), glm::vec3(0, 1, 0));
        matrix = glm::rotate(matrix, glm::radians(degrees.x), glm::vec3(1, 0, 0));
        return glm::mat3(matrix);
    }

    bool nearMatrix(const glm::mat3& lhs, const glm::mat3& rhs) {
        for (int column = 0; column < 3; ++column) {
            if (!near(lhs[column], rhs[column], 1.0e-4f)) return false;
        }
        return true;
    }

    // A one-fixture manifest around `factory`, next to a placeholder source,
    // loaded without content-hash verification.
    BenchmarkManifest loadSyntheticFactory(const std::string& name,
        const nlohmann::json& factory) {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            "iridium-benchmark-manifest-tests";
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "source.gltf") << "{}";
        nlohmann::json root = nlohmann::json::parse(R"json({
            "schema_version": 1,
            "fixtures": [{
                "id": "synthetic_v1",
                "revision": 1,
                "source_asset": "source.gltf",
                "environment": { "kind": "procedural_constant",
                    "constant_linear_rgb": [0.1, 0.1, 0.1] },
                "camera": { "id": "synthetic_camera", "position": [0.0, 1.0, 5.0],
                    "target": [0.0, 1.0, 0.0], "up": [0.0, 1.0, 0.0],
                    "vertical_fov_degrees": 40.0, "near": 0.1, "far": 100.0 },
                "output_label": "synthetic",
                "warmup_frames": 0,
                "measured_frames": 1,
                "content_files": [{ "path": "source.gltf",
                    "sha256": "0000000000000000000000000000000000000000000000000000000000000000" }]
            }]
        })json");
        root["fixtures"][0]["scene_factory"] = factory;
        const std::filesystem::path path = directory / (name + ".json");
        std::ofstream(path) << root.dump(2);
        return loadBenchmarkManifest(path, false);
    }

    // Manifest content resolution: the manifest directory first, then the same
    // relative location in the local asset library, with the escape check
    // applied to whichever root matched.
    bool testContentResolutionThroughLocalRoot() {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            ("iridium-benchmark-local-root-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path project = base / "project";
        const std::filesystem::path library = base / "library";
        const std::filesystem::path assets = project / "assets";
        const auto write = [](const std::filesystem::path& path) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << "{}";
        };
        write(assets / "benchmarks" / "m9" / "fixture.gltf");
        write(assets / "shared.gltf");
        write(library / "shared.gltf");
        write(library / "models" / "car" / "car.gltf");
        write(library / "benchmarks" / "m9" / "local-only.gltf");
        write(base / "outside.gltf");
        std::ofstream(project / "iridium.local.json") <<
            "{ \"localAssetRoot\": \"" + library.generic_string() + "\" }";
        const ProjectAssetRoots roots =
            ProjectAssetRoots::fromProjectRoot(project, std::nullopt);
        const ProjectAssetRoots projectOnly =
            ProjectAssetRoots::fromProjectRoot(base / "no-config", std::nullopt);
        const auto canonical = [](const std::filesystem::path& path) {
            return std::filesystem::weakly_canonical(path);
        };
        const auto rejected = [&](const std::filesystem::path& directory,
            const std::filesystem::path& relative) {
            try {
                (void)resolveBenchmarkContentPath(directory, relative, roots);
            }
            catch (const std::exception&) {
                return true;
            }
            return false;
        };
        bool passed = roots.localAssetRoot().has_value();
        // Manifest-local content and project content win over the library.
        passed = passed && resolveBenchmarkContentPath(assets / "benchmarks" / "m9",
            "fixture.gltf", roots) == canonical(assets / "benchmarks/m9/fixture.gltf");
        passed = passed && resolveBenchmarkContentPath(assets, "shared.gltf", roots) ==
            canonical(assets / "shared.gltf");
        // Missing in the project: the same relative location in the library.
        passed = passed && resolveBenchmarkContentPath(assets, "models/car/car.gltf",
            roots) == canonical(library / "models/car/car.gltf");
        passed = passed && resolveBenchmarkContentPath(assets / "benchmarks" / "m9",
            "local-only.gltf", roots) == canonical(library / "benchmarks/m9/local-only.gltf");
        // Missing everywhere: the manifest-relative path, as before.
        passed = passed && resolveBenchmarkContentPath(assets, "models/none.gltf",
            roots) == canonical(assets / "models/none.gltf");
        // Without a local root nothing changes.
        passed = passed && resolveBenchmarkContentPath(assets, "models/car/car.gltf",
            projectOnly) == canonical(assets / "models/car/car.gltf");
        // A manifest outside the project asset root never maps into the library.
        passed = passed && resolveBenchmarkContentPath(project / "tests",
            "models/car/car.gltf", roots) == canonical(project / "tests/models/car/car.gltf");
        // Escapes and absolute paths are rejected for either root.
        passed = passed && rejected(assets, "../outside.gltf");
        passed = passed && rejected(assets / "benchmarks", "../shared.gltf");
        passed = passed && rejected(assets, library / "shared.gltf");
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
        return passed;
    }

    bool syntheticFactoryRejected(const std::string& name,
        const nlohmann::json& factory) {
        try {
            (void)loadSyntheticFactory(name, factory);
        }
        catch (const std::exception&) {
            return true;
        }
        return false;
    }

    bool testCompositionSchemaContract() {
        const BenchmarkManifest manifest = loadSyntheticFactory("composition",
            nlohmann::json::parse(R"json({
                "kind": "composition",
                "entities": [
                    { "id": "static", "node": 0 },
                    { "id": "placed", "node": 3, "transform": {
                        "translation": [1.0, 2.0, 3.0],
                        "rotation_degrees": [10.0, 20.0, 30.0],
                        "scale": [2.0, 2.0, 2.0] },
                      "motion": { "kind": "none" } },
                    { "id": "slide", "node": 1, "transform": { "translation": [1.0, 0.0, 0.0] },
                      "motion": { "kind": "linear", "velocity_per_frame": [0.5, 0.0, 0.0],
                        "period_frames": 10 } },
                    { "id": "spin", "node": 2, "transform": { "rotation_degrees": [0.0, 30.0, 0.0] },
                      "motion": { "kind": "rotation", "axis": [0.0, 0.0, 2.0],
                        "degrees_per_frame": 1.5 } },
                    { "id": "jump", "node": 2,
                      "motion": { "kind": "keyframes", "keyframes": [
                        { "frame": 0, "translation": [0.0, 0.0, 0.0] },
                        { "frame": 10, "translation": [10.0, 0.0, 0.0] },
                        { "frame": 20, "translation": [-5.0, 0.0, 0.0], "teleport": true },
                        { "frame": 30, "translation": [-5.0, 4.0, 0.0] } ] } }
                ],
                "camera_motion": { "path": {
                    "keyframes": [
                        { "frame": 0, "position": [0.0, 1.0, 5.0], "target": [0.0, 1.0, 0.0] },
                        { "frame": 10, "position": [10.0, 1.0, 5.0], "target": [10.0, 1.0, 0.0] },
                        { "frame": 20, "position": [0.0, 5.0, 9.0], "target": [0.0, 0.0, 0.0] },
                        { "frame": 40, "position": [0.0, 9.0, 9.0], "target": [0.0, 0.0, 0.0] } ],
                    "cuts": [20, 40] } }
            })json"));
        const BenchmarkFixture& fixture = manifest.fixtures.at(0);
        const BenchmarkSceneFactory& factory = fixture.sceneFactory;
        CHECK(factory.kind == BenchmarkSceneFactoryKind::Composition);
        CHECK(factory.compositionEntities.size() == 5);
        // Grid defaults are untouched by a composition factory.
        CHECK(factory.instanceGrid == glm::uvec3(1u));
        CHECK(!factory.animateInstances && !factory.cameraCutEnabled);

        const BenchmarkCompositionEntity& still = factory.compositionEntities[0];
        CHECK(still.id == "static" && still.sourceNode == 0u);
        CHECK(still.motion.kind == BenchmarkEntityMotionKind::None);
        CHECK(still.translation == glm::vec3(0.0f) && still.scale == glm::vec3(1.0f));
        const BenchmarkCompositionEntity& placed = factory.compositionEntities[1];
        CHECK(placed.sourceNode == 3u);
        CHECK(placed.translation == glm::vec3(1.0f, 2.0f, 3.0f));
        CHECK(placed.rotationDegrees == glm::vec3(10.0f, 20.0f, 30.0f));
        CHECK(placed.scale == glm::vec3(2.0f));
        const BenchmarkEntityPose placedPose =
            evaluateBenchmarkCompositionEntity(placed, 1234);
        CHECK(placedPose.translation == placed.translation);
        CHECK(placedPose.rotationDegrees == placed.rotationDegrees);
        CHECK(!placedPose.teleported);

        const BenchmarkCompositionEntity& slide = factory.compositionEntities[2];
        CHECK(slide.motion.kind == BenchmarkEntityMotionKind::Linear);
        CHECK(slide.motion.velocityPerFrame == glm::vec3(0.5f, 0.0f, 0.0f));
        CHECK(slide.motion.periodFrames == 10u);
        CHECK(near(evaluateBenchmarkCompositionEntity(slide, 4).translation,
            glm::vec3(3.0f, 0.0f, 0.0f)));
        CHECK(!evaluateBenchmarkCompositionEntity(slide, 0).teleported);
        CHECK(!evaluateBenchmarkCompositionEntity(slide, 9).teleported);
        // The periodic wrap of a moving linear path is a teleport.
        CHECK(evaluateBenchmarkCompositionEntity(slide, 10).teleported);
        CHECK(near(evaluateBenchmarkCompositionEntity(slide, 10).translation,
            glm::vec3(1.0f, 0.0f, 0.0f)));

        const BenchmarkCompositionEntity& spin = factory.compositionEntities[3];
        CHECK(spin.motion.kind == BenchmarkEntityMotionKind::Rotation);
        CHECK(spin.motion.rotationAxis == glm::vec3(0.0f, 0.0f, 1.0f));
        CHECK(spin.motion.rotationDegreesPerFrame == 1.5f);
        for (const uint64_t frame : { 0ull, 1ull, 60ull, 119ull, 240ull, 1'000'001ull }) {
            const BenchmarkEntityPose pose =
                evaluateBenchmarkCompositionEntity(spin, frame);
            const float degrees = static_cast<float>(
                std::fmod(1.5 * static_cast<double>(frame), 360.0));
            const glm::mat3 expected = glm::mat3(glm::rotate(glm::mat4(1.0f),
                glm::radians(degrees), glm::vec3(0, 0, 1))) *
                eulerZyxMatrix(spin.rotationDegrees);
            CHECK(nearMatrix(eulerZyxMatrix(pose.rotationDegrees), expected));
            CHECK(!pose.teleported);
        }

        const BenchmarkCompositionEntity& jump = factory.compositionEntities[4];
        CHECK(jump.motion.kind == BenchmarkEntityMotionKind::Keyframes);
        CHECK(jump.motion.keyframes.size() == 4);
        CHECK(jump.motion.keyframes[2].teleport);
        CHECK(near(evaluateBenchmarkCompositionEntity(jump, 5).translation,
            glm::vec3(5.0f, 0.0f, 0.0f)));
        // The teleport segment holds the previous keyframe, then jumps.
        CHECK(evaluateBenchmarkCompositionEntity(jump, 19).translation ==
            glm::vec3(10.0f, 0.0f, 0.0f));
        CHECK(!evaluateBenchmarkCompositionEntity(jump, 19).teleported);
        CHECK(evaluateBenchmarkCompositionEntity(jump, 20).translation ==
            glm::vec3(-5.0f, 0.0f, 0.0f));
        CHECK(evaluateBenchmarkCompositionEntity(jump, 20).teleported);
        CHECK(!evaluateBenchmarkCompositionEntity(jump, 21).teleported);
        CHECK(near(evaluateBenchmarkCompositionEntity(jump, 25).translation,
            glm::vec3(-5.0f, 2.0f, 0.0f)));
        CHECK(evaluateBenchmarkCompositionEntity(jump, 500).translation ==
            glm::vec3(-5.0f, 4.0f, 0.0f));

        CHECK(factory.cameraPathEnabled);
        CHECK(factory.cameraPathKeyframes.size() == 4);
        CHECK(factory.cameraPathCuts == std::vector<uint64_t>({ 20u, 40u }));
        CHECK(factory.cameraPathPeriodFrames == 0u);
        CHECK(near(evaluateBenchmarkCamera(fixture, 5).position,
            glm::vec3(5.0f, 1.0f, 5.0f)));
        CHECK(near(evaluateBenchmarkCamera(fixture, 5).target,
            glm::vec3(5.0f, 1.0f, 0.0f)));
        CHECK(evaluateBenchmarkCamera(fixture, 19).position ==
            glm::vec3(10.0f, 1.0f, 5.0f));
        CHECK(evaluateBenchmarkCamera(fixture, 20).position ==
            glm::vec3(0.0f, 5.0f, 9.0f));
        CHECK(evaluateBenchmarkCamera(fixture, 39).position ==
            glm::vec3(0.0f, 5.0f, 9.0f));
        CHECK(evaluateBenchmarkCamera(fixture, 100).position ==
            glm::vec3(0.0f, 9.0f, 9.0f));
        // Each cut bumps the history-reset revision to its ordinal.
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 0) == 0u);
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 19) == 0u);
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 20) == 1u);
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 39) == 1u);
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 40) == 2u);
        CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, 9'999) == 2u);

        // Determinism: repeated evaluation and a reload agree bit for bit.
        const BenchmarkManifest again = loadSyntheticFactory("composition_again",
            nlohmann::json::parse(R"json({
                "kind": "composition",
                "entities": [{ "id": "spin", "node": 2,
                    "transform": { "rotation_degrees": [0.0, 30.0, 0.0] },
                    "motion": { "kind": "rotation", "axis": [0.0, 0.0, 2.0],
                        "degrees_per_frame": 1.5 } }]
            })json"));
        const BenchmarkEntityPose first = evaluateBenchmarkCompositionEntity(spin, 77);
        const BenchmarkEntityPose second = evaluateBenchmarkCompositionEntity(
            again.fixtures[0].sceneFactory.compositionEntities[0], 77);
        CHECK(first.rotationDegrees == second.rotationDegrees);
        return true;
    }

    bool testPeriodicCameraPathAndWrapContract() {
        const BenchmarkManifest manifest = loadSyntheticFactory("periodic",
            nlohmann::json::parse(R"json({
                "kind": "instanced_grid",
                "instance_grid": [1, 1, 1],
                "instance_spacing": [0.0, 0.0, 0.0],
                "camera_motion": { "path": {
                    "period_frames": 100,
                    "keyframes": [
                        { "frame": 0, "position": [0.0, 1.0, 5.0], "target": [0.0, 1.0, 0.0] },
                        { "frame": 50, "position": [5.0, 1.0, 5.0], "target": [0.0, 1.0, 0.0] },
                        { "frame": 60, "position": [0.0, 3.0, 5.0], "target": [0.0, 1.0, 0.0] } ],
                    "cuts": [0, 60] } }
            })json"));
        const BenchmarkFixture& fixture = manifest.fixtures.at(0);
        // A camera path also applies to the instanced-grid factory.
        CHECK(fixture.sceneFactory.kind == BenchmarkSceneFactoryKind::InstancedGrid);
        CHECK(fixture.sceneFactory.cameraPathEnabled);
        CHECK(fixture.sceneFactory.cameraPathPeriodFrames == 100u);
        CHECK(evaluateBenchmarkCamera(fixture, 99).position ==
            glm::vec3(0.0f, 3.0f, 5.0f));
        CHECK(evaluateBenchmarkCamera(fixture, 100).position ==
            glm::vec3(0.0f, 1.0f, 5.0f));
        CHECK(near(evaluateBenchmarkCamera(fixture, 125).position,
            glm::vec3(2.5f, 1.0f, 5.0f)));
        // The frame-0 cut is the wrap: first at 100, never at absolute frame 0.
        constexpr std::array<std::pair<uint64_t, uint64_t>, 8> revisions{ {
            { 0, 0 }, { 59, 0 }, { 60, 1 }, { 99, 1 }, { 100, 2 }, { 159, 2 },
            { 160, 3 }, { 1'000, 20 } } };
        for (const auto& [frame, revision] : revisions) {
            CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture, frame) ==
                revision);
        }
        return true;
    }

    bool testCompositionSchemaRejections() {
        const auto composition = [](const char* entity) {
            nlohmann::json factory = nlohmann::json::parse(
                R"json({ "kind": "composition" })json");
            factory["entities"] = nlohmann::json::array(
                { nlohmann::json::parse(entity) });
            return factory;
        };
        const auto cameraPath = [](const char* path) {
            nlohmann::json factory = nlohmann::json::parse(R"json({
                "kind": "composition", "entities": [{ "id": "a", "node": 0 }] })json");
            factory["camera_motion"] = nlohmann::json::parse(path);
            return factory;
        };
        CHECK(syntheticFactoryRejected("reject_kind",
            nlohmann::json::parse(R"json({ "kind": "spiral" })json")));
        CHECK(syntheticFactoryRejected("reject_empty",
            nlohmann::json::parse(R"json({ "kind": "composition", "entities": [] })json")));
        CHECK(syntheticFactoryRejected("reject_grid_key", nlohmann::json::parse(R"json({
            "kind": "composition", "entities": [{ "id": "a", "node": 0 }],
            "instance_grid": [1, 1, 1] })json")));
        CHECK(syntheticFactoryRejected("reject_duplicate", nlohmann::json::parse(R"json({
            "kind": "composition",
            "entities": [{ "id": "a", "node": 0 }, { "id": "a", "node": 1 }] })json")));
        CHECK(syntheticFactoryRejected("reject_source_asset", composition(
            R"json({ "id": "a", "node": 0, "source_asset": "other.gltf" })json")));
        CHECK(syntheticFactoryRejected("reject_entity_key", composition(
            R"json({ "id": "a", "node": 0, "velocity": [1, 0, 0] })json")));
        CHECK(syntheticFactoryRejected("reject_zero_scale", composition(
            R"json({ "id": "a", "node": 0, "transform": { "scale": [1, 0, 1] } })json")));
        CHECK(syntheticFactoryRejected("reject_motion_kind", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "orbit" } })json")));
        CHECK(syntheticFactoryRejected("reject_motion_key", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "linear",
                "velocity_per_frame": [1, 0, 0], "teleports": [3] } })json")));
        CHECK(syntheticFactoryRejected("reject_zero_axis", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "rotation",
                "axis": [0, 0, 0], "degrees_per_frame": 1 } })json")));
        CHECK(syntheticFactoryRejected("reject_first_keyframe", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes", "keyframes": [
                { "frame": 5, "translation": [0, 0, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_keyframe_order", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes", "keyframes": [
                { "frame": 0, "translation": [0, 0, 0] },
                { "frame": 9, "translation": [1, 0, 0] },
                { "frame": 9, "translation": [2, 0, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_wrap_teleport", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes", "keyframes": [
                { "frame": 0, "translation": [0, 0, 0], "teleport": true },
                { "frame": 9, "translation": [1, 0, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_unflagged_wrap", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes",
                "period_frames": 20, "keyframes": [
                { "frame": 0, "translation": [0, 0, 0] },
                { "frame": 9, "translation": [1, 0, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_keyframe_past_period", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes",
                "period_frames": 5, "keyframes": [
                { "frame": 0, "translation": [0, 0, 0] },
                { "frame": 9, "translation": [0, 0, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_path_with_velocity", cameraPath(
            R"json({ "velocity_per_frame": [1, 0, 0], "path": { "keyframes": [
                { "frame": 0, "position": [0, 1, 5], "target": [0, 1, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_cut_off_keyframe", cameraPath(
            R"json({ "path": { "cuts": [5], "keyframes": [
                { "frame": 0, "position": [0, 1, 5], "target": [0, 1, 0] },
                { "frame": 10, "position": [1, 1, 5], "target": [1, 1, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_cut_zero_aperiodic", cameraPath(
            R"json({ "path": { "cuts": [0], "keyframes": [
                { "frame": 0, "position": [0, 1, 5], "target": [0, 1, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_camera_wrap", cameraPath(
            R"json({ "path": { "period_frames": 20, "keyframes": [
                { "frame": 0, "position": [0, 1, 5], "target": [0, 1, 0] },
                { "frame": 10, "position": [1, 1, 5], "target": [1, 1, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_degenerate_keyframe", cameraPath(
            R"json({ "path": { "keyframes": [
                { "frame": 0, "position": [0, 1, 0], "target": [0, 5, 0] } ] } })json")));
        CHECK(syntheticFactoryRejected("reject_path_key", cameraPath(
            R"json({ "path": { "cut": [10], "keyframes": [
                { "frame": 0, "position": [0, 1, 5], "target": [0, 1, 0] } ] } })json")));
        // Control: the same shapes are accepted when well formed.
        CHECK(!syntheticFactoryRejected("accept_control", composition(
            R"json({ "id": "a", "node": 0, "motion": { "kind": "keyframes",
                "period_frames": 20, "keyframes": [
                { "frame": 0, "translation": [0, 0, 0], "teleport": true },
                { "frame": 9, "translation": [1, 0, 0] } ] } })json")));
        return true;
    }

    // Every pre-M9 manifest keeps its parse: grid factory, no path, no
    // composition, and the legacy single-cut history revision (0, then 1).
    bool testLegacyManifestsParseUnchanged() {
        const std::array manifests{
            manifestPath(), m1ManifestPath(), m2MaterialGpuManifestPath(),
            m6PyramidManifestPath(), m6Ordinary2RuntimeManifestPath(),
            m7ThreeDenseManifestPath(), m7OcclusionTemporalManifestPath(),
            m7OcclusionMotionManifestPath(), m7OcclusionSmallObjectManifestPath(),
            m7OcclusionDepthContentManifestPath(),
            m7OcclusionPerformanceManifestPath(), m7LodLitAdmissionManifestPath(),
            m7DirectionalShadowGrazingManifestPath(),
            m7DirectionalShadowLodManifestPath(), m7PointShadowLodManifestPath(),
            m7SpotShadowLodManifestPath(),
            m7HeterogeneousShadowAdmissionManifestPath(),
            m7ProbeLodAdmissionManifestPath(),
            m7ShadowLodWarmedAdmissionManifestPath(),
            m7ShadowQualityClosureManifestPath(),
        };
        size_t fixtures = 0;
        size_t cuts = 0;
        for (const std::filesystem::path& path : manifests) {
            for (const BenchmarkFixture& fixture :
                loadBenchmarkManifest(path).fixtures) {
                ++fixtures;
                const BenchmarkSceneFactory& factory = fixture.sceneFactory;
                CHECK(factory.kind == BenchmarkSceneFactoryKind::InstancedGrid);
                CHECK(factory.compositionEntities.empty());
                CHECK(!factory.cameraPathEnabled);
                CHECK(factory.cameraPathKeyframes.empty());
                CHECK(factory.cameraPathCuts.empty());
                cuts += factory.cameraCutEnabled ? 1u : 0u;
                const uint64_t cut = factory.cameraCutFrame;
                for (const uint64_t frame : { uint64_t{ 0 }, cut > 0 ? cut - 1 : 0,
                        cut, cut + 1, cut + 100'000 }) {
                    const uint64_t legacy =
                        factory.cameraCutEnabled && frame >= cut ? 1u : 0u;
                    CHECK(evaluateBenchmarkViewHistoryResetRevision(fixture,
                        frame) == legacy);
                }
            }
        }
        CHECK(fixtures >= 20);
        CHECK(cuts >= 2);
        return true;
    }

    bool testM9TemporalFixtureContract() {
        const BenchmarkManifest manifest = loadBenchmarkManifest(
            m9TemporalManifestPath());
        constexpr std::array ids{
            "m9_tf_thin_v1", "m9_tf_foliage_v1", "m9_tf_disocclude_v1",
            "m9_tf_pan_v1", "m9_tf_emissive_v1", "m9_tf_glass_v1",
            "m9_tf_specular_v1", "m9_tf_static_v1", "m9_tf_hdr_v1",
            "m9_tf_teleport_v1", "m9_tf_reactive_v1" };
        CHECK(manifest.fixtures.size() == ids.size());
        std::set<std::filesystem::path> sources;
        for (const char* id : ids) {
            const BenchmarkFixture& fixture = findBenchmarkFixture(manifest, id);
            CHECK(fixture.revision == 1);
            CHECK(fixture.required);
            CHECK(fixture.sceneFactory.kind ==
                BenchmarkSceneFactoryKind::Composition);
            CHECK(fixture.warmupFrames == 120u);
            CHECK(fixture.measuredFrames == 600u);
            CHECK(fixture.lights.size() == 1);
            CHECK(fixture.lights[0].type == BenchmarkLightType::Directional);
            CHECK(fixture.camera.verticalFovDegrees == 40.0f);
            CHECK(!fixture.expectedBehavior.empty());
            // Engine-authored source plus its sidecar, both hash-pinned.
            CHECK(fixture.contentFiles.size() == 2);
            for (const BenchmarkContentFile& content : fixture.contentFiles)
                CHECK(sha256File(content.path) == content.sha256);
            CHECK(fixture.contentFiles[1].path.string() ==
                fixture.sourceAsset.string() + ".iridium.meta");
            CHECK(sources.insert(fixture.sourceAsset).second);

            // Each entity names an existing top-level node; every node is used.
            const nlohmann::json gltf = loadJson(fixture.sourceAsset);
            CHECK(gltf.at("asset").at("generator").get<std::string>().find(
                "M9 G6b") != std::string::npos);
            const size_t nodeCount = gltf.at("nodes").size();
            CHECK(gltf.at("scenes").at(0).at("nodes").size() == nodeCount);
            std::set<uint32_t> usedNodes;
            for (const nlohmann::json& node : gltf.at("nodes")) {
                CHECK(!node.contains("matrix") && !node.contains("translation") &&
                    !node.contains("rotation") && !node.contains("scale") &&
                    !node.contains("children"));
            }
            for (const BenchmarkCompositionEntity& entity :
                fixture.sceneFactory.compositionEntities) {
                CHECK(entity.sourceNode < nodeCount);
                usedNodes.insert(entity.sourceNode);
            }
            CHECK(usedNodes.size() == nodeCount);
            for (const nlohmann::json& buffer : gltf.at("buffers")) {
                CHECK(buffer.at("uri").get<std::string>().starts_with(
                    "data:application/octet-stream;base64,"));
                CHECK(decodeDataUri(buffer.at("uri").get<std::string>()).size() ==
                    buffer.at("byteLength").get<size_t>());
            }
            for (const nlohmann::json& image :
                gltf.value("images", nlohmann::json::array())) {
                const std::vector<std::byte> png = decodeDataUri(
                    image.at("uri").get<std::string>());
                CHECK(png.size() > 8 && std::to_integer<uint8_t>(png[1]) == 'P' &&
                    std::to_integer<uint8_t>(png[2]) == 'N' &&
                    std::to_integer<uint8_t>(png[3]) == 'G');
            }
        }

        const BenchmarkFixture& thin = findBenchmarkFixture(manifest, "m9_tf_thin_v1");
        CHECK(thin.sceneFactory.compositionEntities.size() == 3);
        CHECK(thin.sceneFactory.cameraPathEnabled);
        CHECK(thin.sceneFactory.cameraPathPeriodFrames == 480u);
        CHECK(thin.sceneFactory.cameraPathCuts.empty());
        CHECK(evaluateBenchmarkCamera(thin, 240).position ==
            glm::vec3(0.8f, 1.6f, 6.0f));
        CHECK(near(evaluateBenchmarkCamera(thin, 120).position,
            glm::vec3(0.0f, 1.6f, 6.0f)));
        CHECK(evaluateBenchmarkViewHistoryResetRevision(thin, 5'000) == 0u);

        const BenchmarkFixture& foliage = findBenchmarkFixture(manifest,
            "m9_tf_foliage_v1");
        CHECK(foliage.sceneFactory.compositionEntities[1].motion.kind ==
            BenchmarkEntityMotionKind::Rotation);
        CHECK(foliage.sceneFactory.compositionEntities[2].motion.kind ==
            BenchmarkEntityMotionKind::None);
        CHECK(!foliage.sceneFactory.cameraPathEnabled);
        const nlohmann::json foliageGltf = loadJson(foliage.sourceAsset);
        bool maskedLeaf = false;
        for (const nlohmann::json& material : foliageGltf.at("materials")) {
            maskedLeaf = maskedLeaf || (material.value("alphaMode", "") == "MASK" &&
                material.at("alphaCutoff") == 0.5 &&
                material.at("pbrMetallicRoughness").contains("baseColorTexture"));
        }
        CHECK(maskedLeaf);

        const BenchmarkFixture& disocclude = findBenchmarkFixture(manifest,
            "m9_tf_disocclude_v1");
        const BenchmarkCompositionEntity& occluder =
            disocclude.sceneFactory.compositionEntities[3];
        CHECK(occluder.id == "occluder");
        CHECK(occluder.motion.periodFrames == 240u);
        CHECK(evaluateBenchmarkCompositionEntity(occluder, 120).translation ==
            glm::vec3(3.5f, 0.0f, 1.5f));
        CHECK(near(evaluateBenchmarkCompositionEntity(occluder, 60).translation,
            glm::vec3(0.0f, 0.0f, 1.5f)));
        // A continuous back-and-forth wrap is not a teleport.
        CHECK(!evaluateBenchmarkCompositionEntity(occluder, 240).teleported);

        const BenchmarkFixture& pan = findBenchmarkFixture(manifest, "m9_tf_pan_v1");
        CHECK(pan.sceneFactory.cameraPathCuts ==
            std::vector<uint64_t>({ 0u, 90u, 180u }));
        CHECK(pan.sceneFactory.cameraPathPeriodFrames == 240u);
        CHECK(evaluateBenchmarkCamera(pan, 89).target ==
            glm::vec3(6.0f, 1.2f, 0.0f));
        CHECK(evaluateBenchmarkCamera(pan, 90).position ==
            glm::vec3(6.0f, 2.5f, 5.0f));
        CHECK(evaluateBenchmarkCamera(pan, 240).target ==
            glm::vec3(-6.0f, 1.2f, 0.0f));
        constexpr std::array<std::pair<uint64_t, uint64_t>, 9> panRevisions{ {
            { 0, 0 }, { 89, 0 }, { 90, 1 }, { 179, 1 }, { 180, 2 }, { 239, 2 },
            { 240, 3 }, { 330, 4 }, { 480, 6 } } };
        for (const auto& [frame, revision] : panRevisions) {
            CHECK(evaluateBenchmarkViewHistoryResetRevision(pan, frame) == revision);
        }

        const BenchmarkFixture& emissive = findBenchmarkFixture(manifest,
            "m9_tf_emissive_v1");
        const nlohmann::json emissiveGltf = loadJson(emissive.sourceAsset);
        double strongest = 0.0;
        for (const nlohmann::json& material : emissiveGltf.at("materials")) {
            if (!material.contains("extensions")) continue;
            strongest = std::max(strongest, material.at("extensions")
                .at("KHR_materials_emissive_strength").at("emissiveStrength")
                .get<double>());
        }
        CHECK(strongest == 40.0);

        const BenchmarkFixture& glass = findBenchmarkFixture(manifest, "m9_tf_glass_v1");
        const nlohmann::json glassGltf = loadJson(glass.sourceAsset);
        const nlohmann::json glassMeta = loadJson(glass.contentFiles[1].path);
        CHECK(glassMeta.at("settings").at("schemaVersion") == 2);
        CHECK(glassMeta.at("settings").at("values")
            .at("transparency_execution_mode") == "classified");
        size_t glassMaterials = 0;
        for (size_t index = 0; index < glassGltf.at("materials").size(); ++index) {
            const nlohmann::json& material = glassGltf.at("materials").at(index);
            if (material.value("alphaMode", "") != "BLEND") continue;
            ++glassMaterials;
            CHECK(material.at("extensions").at("KHR_materials_transmission")
                .at("transmissionFactor") == 1.0);
            std::string materialGuid;
            for (const nlohmann::json& subasset : glassMeta.at("subassets")) {
                if (subasset.at("sourceKey") == "materials/" + std::to_string(index))
                    materialGuid = subasset.at("guid");
            }
            CHECK(glassMeta.at("settings").at("values").at("transparency_policies")
                .at(materialGuid).at("class") == "thin_glass");
        }
        CHECK(glassMaterials == 1);

        const BenchmarkFixture& specular = findBenchmarkFixture(manifest,
            "m9_tf_specular_v1");
        CHECK(specular.sceneFactory.cameraPathPeriodFrames == 1'440u);
        CHECK(specular.sceneFactory.cameraPathKeyframes.size() == 37);
        CHECK(near(evaluateBenchmarkCamera(specular, 1'440).position,
            specular.camera.position));
        CHECK(evaluateBenchmarkCamera(specular, 400).target ==
            glm::vec3(0.0f, 1.0f, 0.0f));
        const BenchmarkCompositionEntity& torus =
            specular.sceneFactory.compositionEntities[2];
        CHECK(torus.motion.kind == BenchmarkEntityMotionKind::Rotation);
        const glm::vec3 axis = glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f));
        CHECK(near(torus.motion.rotationAxis, axis));
        CHECK(nearMatrix(eulerZyxMatrix(
                evaluateBenchmarkCompositionEntity(torus, 360).rotationDegrees),
            glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), axis))));

        const BenchmarkFixture& still = findBenchmarkFixture(manifest, "m9_tf_static_v1");
        CHECK(!still.sceneFactory.cameraPathEnabled);
        CHECK(std::ranges::all_of(still.sceneFactory.compositionEntities,
            [](const BenchmarkCompositionEntity& entity) {
                return entity.motion.kind == BenchmarkEntityMotionKind::None;
            }));

        const BenchmarkFixture& hdr = findBenchmarkFixture(manifest, "m9_tf_hdr_v1");
        const BenchmarkCompositionEntity& highlights =
            hdr.sceneFactory.compositionEntities[2];
        CHECK(evaluateBenchmarkCompositionEntity(highlights, 120).translation ==
            glm::vec3(0.04f, 1.0f, 0.0f));
        const nlohmann::json hdrGltf = loadJson(hdr.sourceAsset);
        strongest = 0.0;
        for (const nlohmann::json& material : hdrGltf.at("materials")) {
            if (!material.contains("extensions")) continue;
            strongest = std::max(strongest, material.at("extensions")
                .at("KHR_materials_emissive_strength").at("emissiveStrength")
                .get<double>());
        }
        CHECK(strongest == 4096.0);

        const BenchmarkFixture& teleport = findBenchmarkFixture(manifest,
            "m9_tf_teleport_v1");
        const BenchmarkCompositionEntity& cube =
            teleport.sceneFactory.compositionEntities[2];
        CHECK(cube.motion.kind == BenchmarkEntityMotionKind::Keyframes);
        CHECK(cube.motion.periodFrames == 240u);
        CHECK(near(evaluateBenchmarkCompositionEntity(cube, 30).translation,
            glm::vec3(-2.0f, 0.6f, 0.8f)));
        CHECK(evaluateBenchmarkCompositionEntity(cube, 60).translation ==
            glm::vec3(-0.8f, 0.6f, 0.8f));
        CHECK(evaluateBenchmarkCompositionEntity(cube, 61).translation ==
            glm::vec3(1.6f, 0.6f, 0.8f));
        CHECK(evaluateBenchmarkCompositionEntity(cube, 200).translation ==
            glm::vec3(0.0f, 0.6f, 1.8f));
        std::vector<uint64_t> teleports;
        for (uint64_t frame = 0; frame < 600; ++frame) {
            if (evaluateBenchmarkCompositionEntity(cube, frame).teleported)
                teleports.push_back(frame);
        }
        CHECK(teleports == std::vector<uint64_t>(
            { 61u, 121u, 240u, 301u, 361u, 480u, 541u }));

        // M9.3: a still camera; tinted thin glass (active volume), an Auto
        // (SortedSurface) card and an explicit WeightedOIT card, all moving.
        const BenchmarkFixture& reactive = findBenchmarkFixture(manifest,
            "m9_tf_reactive_v1");
        CHECK(!reactive.sceneFactory.cameraPathEnabled);
        CHECK(reactive.sceneFactory.compositionEntities.size() == 5);
        for (size_t index = 2; index < 5; ++index)
            CHECK(reactive.sceneFactory.compositionEntities[index].motion.kind ==
                BenchmarkEntityMotionKind::Keyframes);
        const nlohmann::json reactiveGltf = loadJson(reactive.sourceAsset);
        const nlohmann::json reactiveMeta = loadJson(reactive.contentFiles[1].path);
        std::map<std::string, std::string> reactiveClasses;
        for (size_t index = 0; index < reactiveGltf.at("materials").size(); ++index) {
            const nlohmann::json& material = reactiveGltf.at("materials").at(index);
            if (material.value("alphaMode", "") != "BLEND") continue;
            std::string materialClass = "auto";
            for (const nlohmann::json& subasset : reactiveMeta.at("subassets")) {
                if (subasset.at("sourceKey") != "materials/" + std::to_string(index)) continue;
                const nlohmann::json& policies = reactiveMeta.at("settings").at("values")
                    .at("transparency_policies");
                if (policies.contains(subasset.at("guid").get<std::string>()))
                    materialClass = policies.at(subasset.at("guid").get<std::string>())
                        .at("class").get<std::string>();
            }
            reactiveClasses[material.at("name").get<std::string>()] = materialClass;
            if (materialClass == "thin_glass")
                CHECK(material.at("extensions").at("KHR_materials_volume")
                    .at("thicknessFactor").get<double>() > 0.0);
        }
        CHECK(reactiveClasses.size() == 3);
        CHECK(std::ranges::count_if(reactiveClasses, [](const auto& entry) {
            return entry.second == "thin_glass"; }) == 1);
        CHECK(std::ranges::count_if(reactiveClasses, [](const auto& entry) {
            return entry.second == "auto"; }) == 1);
        CHECK(std::ranges::count_if(reactiveClasses, [](const auto& entry) {
            return entry.second == "weighted_oit"; }) == 1);
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[] = {
        { "SHA-256 known vector", testSha256KnownVector },
        { "manifest and content verification", testManifestAndContentVerification },
        { "content resolution through the local asset root", testContentResolutionThroughLocalRoot },
        { "nested transparency fixture contract", testNestedTransparencyFixtureContract },
        { "opaque emissive range fixture contract", testOpaqueEmissiveRangeFixtureContract },
        { "M1 color-volume fixture contract", testM1ColorVolumeFixtureContract },
        { "M2 fixture matrix contract", testM2FixtureMatrixContract },
        { "M2 run manifest contract", testM2RunManifestContract },
        { "M2.4 material GPU fixture contract", testM2MaterialGpuFixtureContract },
        { "tracked fixture sidecars", testTrackedFixtureSidecars },
        { "M6 pyramid fixture contract", testM6PyramidFixtureContract },
        { "M6 Ordinary2 runtime fixture contract",
            testM6Ordinary2RuntimeFixtureContract },
        { "M7 three dense fixture contract", testM7ThreeDenseFixtureContract },
        { "M7 occlusion temporal fixture contract",
            testM7OcclusionTemporalFixtureContract },
        { "M7 occlusion motion fixture contract",
            testM7OcclusionMotionFixtureContract },
        { "M7 occlusion small-object fixture contract",
            testM7OcclusionSmallObjectFixtureContract },
        { "M7 occlusion depth-content fixture contract",
            testM7OcclusionDepthContentFixtureContract },
        { "M7 occlusion performance fixture contract",
            testM7OcclusionPerformanceFixtureContract },
        { "M7 LOD oblique fixture contract", testM7LodObliqueFixtureContract },
        { "M7 LOD history fixture contract", testM7LodHistoryFixtureContract },
        { "M7 LOD lit admission fixture contract",
            testM7LodLitAdmissionFixtureContract },
        { "M7 directional-shadow grazing fixture contract",
            testM7DirectionalShadowGrazingFixtureContract },
        { "M7 directional-shadow LOD fixture contract",
            testM7DirectionalShadowLodFixtureContract },
        { "M7 point-shadow LOD fixture contract",
            testM7PointShadowLodFixtureContract },
        { "M7 spot-shadow LOD fixture contract",
            testM7SpotShadowLodFixtureContract },
        { "M7 heterogeneous-shadow admission fixture contract",
            testM7HeterogeneousShadowAdmissionFixtureContract },
        { "M7 probe-LOD admission fixture contract",
            testM7ProbeLodAdmissionFixtureContract },
        { "M7 shadow-LOD warmed admission fixture contract",
            testM7ShadowLodWarmedAdmissionFixtureContract },
        { "M7 shadow-quality closure fixture contract",
            testM7ShadowQualityClosureFixtureContract },
        { "composition schema contract", testCompositionSchemaContract },
        { "periodic camera path and wrap contract",
            testPeriodicCameraPathAndWrapContract },
        { "composition schema rejections", testCompositionSchemaRejections },
        { "legacy manifests parse unchanged", testLegacyManifestsParseUnchanged },
        { "M9 temporal fixture contract", testM9TemporalFixtureContract },
        { "repeated loads are identical", testRepeatedLoadsAreIdentical },
        { "unknown fixture fails", testUnknownFixtureFails },
        { "instance count overflow is rejected", testInstanceCountOverflowIsRejected },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) std::cout << "[PASS] " << test.name << '\n';
            else { ++failures; std::cerr << "[FAIL] " << test.name << '\n'; }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
        << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
