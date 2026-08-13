#include "benchmarks/BenchmarkManifest.h"

#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace Iridium {

    namespace {

        using Json = nlohmann::json;

        glm::vec3 readVec3(const Json& value, const char* field) {
            if (!value.is_array() || value.size() != 3) {
                throw std::runtime_error(std::string(field) + " must contain three numbers");
            }
            return { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
        }

        glm::uvec3 readUVec3(const Json& value, const char* field) {
            if (!value.is_array() || value.size() != 3) {
                throw std::runtime_error(std::string(field) +
                    " must contain three unsigned integers");
            }
            return { value[0].get<uint32_t>(), value[1].get<uint32_t>(),
                value[2].get<uint32_t>() };
        }

        bool finiteVec3(const glm::vec3& value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        bool pathWithin(const std::filesystem::path& candidate,
            const std::filesystem::path& root) {
            auto candidatePart = candidate.begin();
            for (auto rootPart = root.begin(); rootPart != root.end();
                ++rootPart, ++candidatePart) {
                if (candidatePart == candidate.end() || *candidatePart != *rootPart) return false;
            }
            return true;
        }

        std::filesystem::path resolveContentPath(const std::filesystem::path& root,
            const std::filesystem::path& relative) {
            if (relative.is_absolute()) {
                throw std::runtime_error("Benchmark content paths must be relative");
            }
            const auto resolved = std::filesystem::weakly_canonical(root / relative);
            if (!pathWithin(resolved, root)) {
                throw std::runtime_error("Benchmark content path escapes manifest directory: " +
                    relative.string());
            }
            return resolved;
        }

        std::string lowercase(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

    } // namespace

    BenchmarkManifest loadBenchmarkManifest(const std::filesystem::path& path,
        bool verifyContentHashes) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Failed to open benchmark manifest: " + path.string());
        Json root;
        input >> root;

        BenchmarkManifest manifest{};
        manifest.schemaVersion = root.at("schema_version").get<uint32_t>();
        if (manifest.schemaVersion != 1) {
            throw std::runtime_error("Unsupported benchmark manifest schema version");
        }
        manifest.sourcePath = std::filesystem::weakly_canonical(path);
        const std::filesystem::path manifestRoot = manifest.sourcePath.parent_path();
        std::set<std::string> fixtureIds;
        for (const Json& source : root.at("fixtures")) {
            BenchmarkFixture fixture{};
            fixture.id = source.at("id").get<std::string>();
            if (fixture.id.empty() || !fixtureIds.insert(fixture.id).second) {
                throw std::runtime_error("Benchmark fixture IDs must be nonempty and unique");
            }
            fixture.revision = source.at("revision").get<uint32_t>();
            if (fixture.revision == 0) {
                throw std::runtime_error("Benchmark fixture revision must be nonzero: " + fixture.id);
            }
            fixture.required = source.value("required", true);
            fixture.sourceAsset = resolveContentPath(manifestRoot,
                source.at("source_asset").get<std::string>());
            const Json& environment = source.at("environment");
            if (environment.at("kind").get<std::string>() != "procedural_constant") {
                throw std::runtime_error("Unsupported benchmark environment: " + fixture.id);
            }
            fixture.constantEnvironmentLinear = readVec3(
                environment.at("constant_linear_rgb"),
                "constant_linear_rgb");
            if (!finiteVec3(fixture.constantEnvironmentLinear) ||
                fixture.constantEnvironmentLinear.x < 0.0f ||
                fixture.constantEnvironmentLinear.y < 0.0f ||
                fixture.constantEnvironmentLinear.z < 0.0f) {
                throw std::runtime_error("Benchmark environment must be nonnegative: " +
                    fixture.id);
            }

            const Json& camera = source.at("camera");
            fixture.camera.id = camera.at("id").get<std::string>();
            fixture.camera.position = readVec3(camera.at("position"), "camera.position");
            fixture.camera.target = readVec3(camera.at("target"), "camera.target");
            fixture.camera.up = readVec3(camera.at("up"), "camera.up");
            fixture.camera.verticalFovDegrees = camera.at("vertical_fov_degrees").get<float>();
            fixture.camera.nearPlane = camera.at("near").get<float>();
            fixture.camera.farPlane = camera.at("far").get<float>();
            if (fixture.camera.id.empty() || !finiteVec3(fixture.camera.position) ||
                !finiteVec3(fixture.camera.target) || !finiteVec3(fixture.camera.up) ||
                !std::isfinite(fixture.camera.verticalFovDegrees) ||
                !std::isfinite(fixture.camera.nearPlane) ||
                !std::isfinite(fixture.camera.farPlane) ||
                fixture.camera.verticalFovDegrees <= 0.0f ||
                fixture.camera.verticalFovDegrees >= 180.0f || fixture.camera.nearPlane <= 0.0f ||
                fixture.camera.farPlane <= fixture.camera.nearPlane ||
                glm::length(fixture.camera.target - fixture.camera.position) <= 0.0f ||
                glm::length(fixture.camera.up) <= 0.0f ||
                glm::length(glm::cross(fixture.camera.target - fixture.camera.position,
                    fixture.camera.up)) <= 0.0f) {
                throw std::runtime_error("Invalid benchmark camera: " + fixture.id);
            }

            const Json& factory = source.at("scene_factory");
            if (factory.at("kind").get<std::string>() != "instanced_grid") {
                throw std::runtime_error("Unsupported benchmark scene factory: " + fixture.id);
            }
            fixture.sceneFactory.instanceGrid = readUVec3(
                factory.at("instance_grid"), "scene_factory.instance_grid");
            fixture.sceneFactory.instanceSpacing = readVec3(
                factory.at("instance_spacing"), "scene_factory.instance_spacing");
            if (!finiteVec3(fixture.sceneFactory.instanceSpacing)) {
                throw std::runtime_error("Benchmark instance spacing must be finite: " +
                    fixture.id);
            }
            const uint64_t instanceCount = benchmarkInstanceCount(
                fixture.sceneFactory.instanceGrid);
            if (instanceCount == 0) {
                throw std::runtime_error("Benchmark instance count is out of range: " + fixture.id);
            }
            if (factory.contains("object_motion")) {
                const Json& motion = factory.at("object_motion");
                fixture.sceneFactory.animateInstances = motion.value("enabled", false);
                fixture.sceneFactory.motionAmplitude = motion.value("amplitude", 0.0f);
                fixture.sceneFactory.motionPeriodFrames = motion.value("period_frames", 1ull);
                if (!std::isfinite(fixture.sceneFactory.motionAmplitude) ||
                    (fixture.sceneFactory.animateInstances &&
                        fixture.sceneFactory.motionPeriodFrames == 0)) {
                    throw std::runtime_error("Benchmark motion period must be nonzero: " + fixture.id);
                }
            }
            if (factory.contains("camera_motion")) {
                const Json& motion = factory.at("camera_motion");
                fixture.sceneFactory.cameraVelocityPerFrame = readVec3(
                    motion.value("velocity_per_frame", Json::array({ 0.0, 0.0, 0.0 })),
                    "camera_motion.velocity_per_frame");
                if (!finiteVec3(fixture.sceneFactory.cameraVelocityPerFrame)) {
                    throw std::runtime_error("Benchmark camera velocity must be finite: " +
                        fixture.id);
                }
                if (motion.contains("cut")) {
                    const Json& cut = motion.at("cut");
                    fixture.sceneFactory.cameraCutEnabled = true;
                    fixture.sceneFactory.cameraCutFrame = cut.at("frame").get<uint64_t>();
                    fixture.sceneFactory.cameraCutPosition = readVec3(
                        cut.at("position"), "camera_motion.cut.position");
                    fixture.sceneFactory.cameraCutTarget = readVec3(
                        cut.at("target"), "camera_motion.cut.target");
                    if (!finiteVec3(fixture.sceneFactory.cameraCutPosition) ||
                        !finiteVec3(fixture.sceneFactory.cameraCutTarget) ||
                        glm::length(fixture.sceneFactory.cameraCutTarget -
                        fixture.sceneFactory.cameraCutPosition) <= 0.0f) {
                        throw std::runtime_error("Invalid benchmark camera cut: " + fixture.id);
                    }
                }
            }

            fixture.outputLabel = source.at("output_label").get<std::string>();
            fixture.warmupFrames = source.at("warmup_frames").get<uint64_t>();
            fixture.measuredFrames = source.at("measured_frames").get<uint64_t>();
            if (fixture.measuredFrames == 0) {
                throw std::runtime_error("Benchmark measured_frames must be nonzero: " + fixture.id);
            }
            fixture.expectedBehavior = source.value("expected_behavior",
                std::vector<std::string>{});
            fixture.unavailableCapabilities = source.value("unavailable_capabilities",
                std::vector<std::string>{});

            bool sourceAssetDeclared = false;
            for (const Json& file : source.at("content_files")) {
                BenchmarkContentFile content{};
                const auto relative = std::filesystem::path(file.at("path").get<std::string>());
                content.relativePath = relative;
                content.path = resolveContentPath(manifestRoot, relative);
                content.sha256 = lowercase(file.at("sha256").get<std::string>());
                if (content.sha256.size() != 64 ||
                    !std::all_of(content.sha256.begin(), content.sha256.end(), [](unsigned char value) {
                        return std::isxdigit(value) != 0;
                    })) {
                    throw std::runtime_error("Invalid SHA-256 for " + relative.string());
                }
                if (!std::filesystem::is_regular_file(content.path)) {
                    throw std::runtime_error("Missing benchmark content: " + content.path.string());
                }
                if (verifyContentHashes && sha256File(content.path) != content.sha256) {
                    throw std::runtime_error("Benchmark content hash mismatch: " +
                        content.path.string());
                }
                sourceAssetDeclared = sourceAssetDeclared ||
                    content.path == fixture.sourceAsset;
                fixture.contentFiles.push_back(std::move(content));
            }
            if (!sourceAssetDeclared) {
                throw std::runtime_error("source_asset must appear in content_files: " + fixture.id);
            }
            manifest.fixtures.push_back(std::move(fixture));
        }
        if (manifest.fixtures.empty()) {
            throw std::runtime_error("Benchmark manifest has no fixtures");
        }
        for (const Json& source : root.value("optional_local_diagnostics", Json::array())) {
            BenchmarkManifest::LocalDiagnostic diagnostic{};
            diagnostic.id = source.at("id").get<std::string>();
            diagnostic.category = source.at("category").get<std::string>();
            diagnostic.sourceAsset = source.at("source_asset").get<std::string>();
            diagnostic.license = source.at("license").get<std::string>();
            diagnostic.treeSha256 = lowercase(source.at("tree_sha256").get<std::string>());
            diagnostic.invocation = source.at("invocation").get<std::string>();
            diagnostic.expectedBehavior = source.value("expected_behavior",
                std::vector<std::string>{});
            diagnostic.unavailableCapabilities = source.value("unavailable_capabilities",
                std::vector<std::string>{});
            if (diagnostic.id.empty() || diagnostic.category.empty() ||
                diagnostic.treeSha256.size() != 64 ||
                !std::all_of(diagnostic.treeSha256.begin(), diagnostic.treeSha256.end(),
                    [](unsigned char value) { return std::isxdigit(value) != 0; })) {
                throw std::runtime_error("Invalid optional local diagnostic record");
            }
            manifest.localDiagnostics.push_back(std::move(diagnostic));
        }
        return manifest;
    }

    const BenchmarkFixture& findBenchmarkFixture(const BenchmarkManifest& manifest,
        const std::string& id) {
        const auto fixture = std::find_if(manifest.fixtures.begin(), manifest.fixtures.end(),
            [&](const BenchmarkFixture& candidate) { return candidate.id == id; });
        if (fixture == manifest.fixtures.end()) {
            throw std::runtime_error("Unknown benchmark fixture: " + id);
        }
        return *fixture;
    }

    BenchmarkCameraPose evaluateBenchmarkCamera(const BenchmarkFixture& fixture,
        uint64_t frameIndex) noexcept {
        BenchmarkCameraPose pose{ fixture.camera.position, fixture.camera.target };
        uint64_t segmentFrame = frameIndex;
        if (fixture.sceneFactory.cameraCutEnabled &&
            frameIndex >= fixture.sceneFactory.cameraCutFrame) {
            pose.position = fixture.sceneFactory.cameraCutPosition;
            pose.target = fixture.sceneFactory.cameraCutTarget;
            segmentFrame = frameIndex - fixture.sceneFactory.cameraCutFrame;
        }
        const glm::vec3 offset = fixture.sceneFactory.cameraVelocityPerFrame *
            static_cast<float>(segmentFrame);
        pose.position += offset;
        pose.target += offset;
        return pose;
    }

    float evaluateBenchmarkInstanceYOffset(const BenchmarkSceneFactory& factory,
        uint64_t frameIndex, size_t instanceIndex) noexcept {
        if (!factory.animateInstances || factory.motionPeriodFrames == 0) return 0.0f;
        const double turnsPerFrame = 2.0 * std::numbers::pi /
            static_cast<double>(factory.motionPeriodFrames);
        const double phase = static_cast<double>(frameIndex) +
            static_cast<double>(instanceIndex % 17) * 3.0;
        return factory.motionAmplitude *
            static_cast<float>(std::sin(phase * turnsPerFrame));
    }

    uint64_t benchmarkInstanceCount(glm::uvec3 grid) noexcept {
        uint64_t result = 1;
        for (const uint32_t dimension : { grid.x, grid.y, grid.z }) {
            if (dimension == 0 || dimension > 100000 / result) return 0;
            result *= dimension;
        }
        return result;
    }

} // namespace Iridium
