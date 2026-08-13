#include "benchmarks/BenchmarkManifest.h"
#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
                const std::filesystem::path source =
                    std::filesystem::path(PROJECT_ROOT_DIR) /
                    fixture.at("source_asset").get<std::string>();
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
                3);
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

    bool testInstanceCountOverflowIsRejected() {
        CHECK(benchmarkInstanceCount(glm::uvec3(16, 8, 1)) == 128);
        CHECK(benchmarkInstanceCount(glm::uvec3(0, 1, 1)) == 0);
        CHECK(benchmarkInstanceCount(glm::uvec3(
            0xffffffffu, 0xffffffffu, 1u)) == 0);
        return true;
    }

} // namespace

int main() {
    struct TestCase { const char* name; bool (*run)(); };
    constexpr TestCase tests[] = {
        { "SHA-256 known vector", testSha256KnownVector },
        { "manifest and content verification", testManifestAndContentVerification },
        { "nested transparency fixture contract", testNestedTransparencyFixtureContract },
        { "opaque emissive range fixture contract", testOpaqueEmissiveRangeFixtureContract },
        { "M1 color-volume fixture contract", testM1ColorVolumeFixtureContract },
        { "M2 fixture matrix contract", testM2FixtureMatrixContract },
        { "M2 run manifest contract", testM2RunManifestContract },
        { "M2.4 material GPU fixture contract", testM2MaterialGpuFixtureContract },
        { "tracked fixture sidecars", testTrackedFixtureSidecars },
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
