#include "benchmarks/BenchmarkManifest.h"

#include "utils/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>
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

        BenchmarkLightType readLightType(const Json& value,
            const std::string& fixtureId) {
            const std::string type = lowercase(value.get<std::string>());
            if (type == "directional") return BenchmarkLightType::Directional;
            if (type == "point") return BenchmarkLightType::Point;
            if (type == "spot") return BenchmarkLightType::Spot;
            throw std::runtime_error("Unsupported benchmark light type in fixture: " +
                fixtureId);
        }

        BenchmarkShadowQuality readShadowQuality(const Json& value,
            const std::string& fixtureId) {
            const std::string quality = lowercase(value.get<std::string>());
            if (quality == "low") return BenchmarkShadowQuality::Low;
            if (quality == "medium") return BenchmarkShadowQuality::Medium;
            if (quality == "high") return BenchmarkShadowQuality::High;
            if (quality == "ultra") return BenchmarkShadowQuality::Ultra;
            throw std::runtime_error(
                "Unsupported benchmark shadow quality in fixture: " + fixtureId);
        }

        BenchmarkReflectionProbeUpdateMode readReflectionProbeUpdateMode(
            const Json& value, const std::string& fixtureId) {
            const std::string mode = lowercase(value.get<std::string>());
            if (mode == "on_demand")
                return BenchmarkReflectionProbeUpdateMode::OnDemand;
            if (mode == "realtime")
                return BenchmarkReflectionProbeUpdateMode::Realtime;
            throw std::runtime_error(
                "Unsupported benchmark reflection-probe update mode in fixture: " +
                fixtureId);
        }

        bool supportedReflectionProbeResolution(uint32_t value) noexcept {
            return value == 128u || value == 256u || value == 512u ||
                value == 1'024u || value == 2'048u || value == 4'096u;
        }

        // The M9 schema objects are new, so they reject unknown keys: a
        // misspelled motion or cut field must fail loudly, never parse as a
        // static fixture. (Older objects keep their permissive parsing.)
        void requireKnownKeys(const Json& object,
            std::initializer_list<std::string_view> known,
            const std::string& context, const std::string& fixtureId) {
            if (!object.is_object()) {
                throw std::runtime_error(context + " must be an object: " +
                    fixtureId);
            }
            for (const auto& item : object.items()) {
                if (std::find(known.begin(), known.end(), item.key()) ==
                    known.end()) {
                    throw std::runtime_error("Unknown key '" + item.key() +
                        "' in " + context + ": " + fixtureId);
                }
            }
        }

        bool nonzeroScale(const glm::vec3& scale) noexcept {
            return std::abs(scale.x) > 1.0e-7f && std::abs(scale.y) > 1.0e-7f &&
                std::abs(scale.z) > 1.0e-7f;
        }

        // Keyframes start at frame 0 and strictly increase. A periodic
        // sequence ends at or before its period; a frame-0 flag marks the
        // wrap as discontinuous and is valid only when periodic.
        template <typename Keyframe>
        void validateKeyframeFrames(const std::vector<Keyframe>& keyframes,
            uint64_t periodFrames, const std::string& context,
            const std::string& fixtureId) {
            if (keyframes.empty() || keyframes.front().frame != 0u) {
                throw std::runtime_error(context +
                    " keyframes must be nonempty and start at frame 0: " +
                    fixtureId);
            }
            for (size_t index = 1; index < keyframes.size(); ++index) {
                if (keyframes[index].frame <= keyframes[index - 1].frame) {
                    throw std::runtime_error(context +
                        " keyframe frames must strictly increase: " + fixtureId);
                }
            }
            if (periodFrames != 0u && keyframes.back().frame > periodFrames) {
                throw std::runtime_error(context +
                    " keyframes must not extend past period_frames: " +
                    fixtureId);
            }
        }

        BenchmarkEntityMotion readEntityMotion(const Json& source,
            const std::string& context, const std::string& fixtureId) {
            BenchmarkEntityMotion motion{};
            const std::string kind = source.at("kind").get<std::string>();
            if (kind == "none") {
                requireKnownKeys(source, { "kind" }, context, fixtureId);
                return motion;
            }
            if (kind == "linear") {
                requireKnownKeys(source,
                    { "kind", "velocity_per_frame", "period_frames" },
                    context, fixtureId);
                motion.kind = BenchmarkEntityMotionKind::Linear;
                motion.velocityPerFrame = readVec3(
                    source.at("velocity_per_frame"),
                    "motion.velocity_per_frame");
                motion.periodFrames = source.value("period_frames",
                    uint64_t{ 0 });
                if (!finiteVec3(motion.velocityPerFrame) ||
                    (source.contains("period_frames") &&
                        motion.periodFrames == 0u)) {
                    throw std::runtime_error("Invalid linear " + context +
                        ": " + fixtureId);
                }
                return motion;
            }
            if (kind == "rotation") {
                requireKnownKeys(source,
                    { "kind", "axis", "degrees_per_frame" }, context,
                    fixtureId);
                motion.kind = BenchmarkEntityMotionKind::Rotation;
                const glm::vec3 axis = readVec3(source.at("axis"),
                    "motion.axis");
                motion.rotationDegreesPerFrame =
                    source.at("degrees_per_frame").get<float>();
                if (!finiteVec3(axis) || glm::length(axis) <= 1.0e-6f ||
                    !std::isfinite(motion.rotationDegreesPerFrame)) {
                    throw std::runtime_error("Invalid rotation " + context +
                        ": " + fixtureId);
                }
                motion.rotationAxis = glm::normalize(axis);
                return motion;
            }
            if (kind == "keyframes") {
                requireKnownKeys(source,
                    { "kind", "keyframes", "period_frames" }, context,
                    fixtureId);
                motion.kind = BenchmarkEntityMotionKind::Keyframes;
                motion.periodFrames = source.value("period_frames",
                    uint64_t{ 0 });
                if (source.contains("period_frames") &&
                    motion.periodFrames == 0u) {
                    throw std::runtime_error("Invalid keyframe period in " +
                        context + ": " + fixtureId);
                }
                const Json& keyframes = source.at("keyframes");
                if (!keyframes.is_array() || keyframes.size() > 4'096u) {
                    throw std::runtime_error(context +
                        " keyframes must be an array of at most 4096 entries: " +
                        fixtureId);
                }
                for (const Json& keyframeSource : keyframes) {
                    requireKnownKeys(keyframeSource,
                        { "frame", "translation", "teleport" },
                        context + " keyframe", fixtureId);
                    BenchmarkTranslationKeyframe keyframe{};
                    keyframe.frame = keyframeSource.at("frame").get<uint64_t>();
                    keyframe.translation = readVec3(
                        keyframeSource.at("translation"),
                        "keyframes.translation");
                    keyframe.teleport = keyframeSource.value("teleport", false);
                    if (!finiteVec3(keyframe.translation)) {
                        throw std::runtime_error("Non-finite keyframe in " +
                            context + ": " + fixtureId);
                    }
                    motion.keyframes.push_back(keyframe);
                }
                validateKeyframeFrames(motion.keyframes, motion.periodFrames,
                    context, fixtureId);
                const BenchmarkTranslationKeyframe& first =
                    motion.keyframes.front();
                const BenchmarkTranslationKeyframe& last =
                    motion.keyframes.back();
                if (first.teleport && motion.periodFrames == 0u) {
                    throw std::runtime_error(
                        "A frame-0 teleport requires period_frames in " +
                        context + ": " + fixtureId);
                }
                if (motion.periodFrames != 0u && last.frame ==
                        motion.periodFrames && last.teleport) {
                    throw std::runtime_error(
                        "A teleport at period_frames is never shown; flag the "
                        "frame-0 keyframe instead in " + context + ": " +
                        fixtureId);
                }
                if (motion.periodFrames != 0u && !first.teleport &&
                    last.translation != first.translation) {
                    throw std::runtime_error(
                        "A discontinuous periodic wrap must be flagged as a "
                        "frame-0 teleport in " + context + ": " + fixtureId);
                }
                return motion;
            }
            throw std::runtime_error("Unsupported " + context + " kind '" +
                kind + "': " + fixtureId);
        }

        std::vector<BenchmarkCompositionEntity> readCompositionEntities(
            const Json& factory, const std::string& fixtureId) {
            requireKnownKeys(factory, { "kind", "entities", "camera_motion" },
                "composition scene_factory", fixtureId);
            const Json& entities = factory.at("entities");
            if (!entities.is_array() || entities.empty() ||
                entities.size() > 4'096u) {
                throw std::runtime_error(
                    "Composition entities must be an array of 1 to 4096 entries: " +
                    fixtureId);
            }
            std::vector<BenchmarkCompositionEntity> result;
            result.reserve(entities.size());
            std::set<std::string> ids;
            for (const Json& source : entities) {
                if (source.is_object() && source.contains("source_asset")) {
                    // One --cooked-model-artifact backs a fixture; entities
                    // select top-level nodes of the fixture's source_asset.
                    throw std::runtime_error(
                        "Composition entities select a 'node' of the fixture "
                        "source_asset; per-entity source_asset is unsupported: " +
                        fixtureId);
                }
                requireKnownKeys(source, { "id", "node", "transform", "motion" },
                    "composition entity", fixtureId);
                BenchmarkCompositionEntity entity{};
                entity.id = source.at("id").get<std::string>();
                if (entity.id.empty() || !ids.insert(entity.id).second) {
                    throw std::runtime_error(
                        "Composition entity IDs must be nonempty and unique: " +
                        fixtureId);
                }
                const std::string context = "composition entity '" +
                    entity.id + "'";
                entity.sourceNode = source.at("node").get<uint32_t>();
                if (source.contains("transform")) {
                    const Json& transform = source.at("transform");
                    requireKnownKeys(transform,
                        { "translation", "rotation_degrees", "scale" },
                        context + " transform", fixtureId);
                    entity.translation = readVec3(transform.value(
                        "translation", Json::array({ 0.0, 0.0, 0.0 })),
                        "transform.translation");
                    entity.rotationDegrees = readVec3(transform.value(
                        "rotation_degrees", Json::array({ 0.0, 0.0, 0.0 })),
                        "transform.rotation_degrees");
                    entity.scale = readVec3(transform.value(
                        "scale", Json::array({ 1.0, 1.0, 1.0 })),
                        "transform.scale");
                    if (!finiteVec3(entity.translation) ||
                        !finiteVec3(entity.rotationDegrees) ||
                        !finiteVec3(entity.scale) ||
                        !nonzeroScale(entity.scale)) {
                        throw std::runtime_error("Invalid transform for " +
                            context + ": " + fixtureId);
                    }
                }
                if (source.contains("motion")) {
                    entity.motion = readEntityMotion(source.at("motion"),
                        context + " motion", fixtureId);
                }
                result.push_back(std::move(entity));
            }
            return result;
        }

        void readCameraPath(const Json& path, const BenchmarkCamera& camera,
            BenchmarkSceneFactory& factory, const std::string& fixtureId) {
            requireKnownKeys(path, { "keyframes", "cuts", "period_frames" },
                "camera_motion.path", fixtureId);
            factory.cameraPathEnabled = true;
            factory.cameraPathPeriodFrames = path.value("period_frames",
                uint64_t{ 0 });
            if (path.contains("period_frames") &&
                factory.cameraPathPeriodFrames == 0u) {
                throw std::runtime_error(
                    "Invalid camera path period_frames: " + fixtureId);
            }
            const Json& keyframes = path.at("keyframes");
            if (!keyframes.is_array() || keyframes.size() > 4'096u) {
                throw std::runtime_error(
                    "Camera path keyframes must be an array of at most 4096 entries: " +
                    fixtureId);
            }
            for (const Json& source : keyframes) {
                requireKnownKeys(source, { "frame", "position", "target" },
                    "camera path keyframe", fixtureId);
                BenchmarkCameraKeyframe keyframe{};
                keyframe.frame = source.at("frame").get<uint64_t>();
                keyframe.position = readVec3(source.at("position"),
                    "camera_motion.path.position");
                keyframe.target = readVec3(source.at("target"),
                    "camera_motion.path.target");
                const glm::vec3 forward = keyframe.target - keyframe.position;
                if (!finiteVec3(keyframe.position) ||
                    !finiteVec3(keyframe.target) ||
                    glm::length(forward) <= 0.0f ||
                    glm::length(glm::cross(forward, camera.up)) <= 0.0f) {
                    throw std::runtime_error(
                        "Invalid camera path keyframe: " + fixtureId);
                }
                factory.cameraPathKeyframes.push_back(keyframe);
            }
            validateKeyframeFrames(factory.cameraPathKeyframes,
                factory.cameraPathPeriodFrames, "camera_motion.path",
                fixtureId);
            factory.cameraPathCuts = path.value("cuts",
                std::vector<uint64_t>{});
            bool wrapCut = false;
            for (size_t index = 0; index < factory.cameraPathCuts.size();
                ++index) {
                const uint64_t cut = factory.cameraPathCuts[index];
                if (index > 0 && cut <= factory.cameraPathCuts[index - 1]) {
                    throw std::runtime_error(
                        "Camera path cuts must strictly increase: " + fixtureId);
                }
                if (cut == 0u) {
                    if (factory.cameraPathPeriodFrames == 0u) {
                        throw std::runtime_error(
                            "A frame-0 camera cut requires period_frames: " +
                            fixtureId);
                    }
                    wrapCut = true;
                    continue;
                }
                const bool onKeyframe = std::ranges::any_of(
                    factory.cameraPathKeyframes,
                    [cut](const BenchmarkCameraKeyframe& keyframe) {
                        return keyframe.frame == cut;
                    });
                if (!onKeyframe || (factory.cameraPathPeriodFrames != 0u &&
                        cut >= factory.cameraPathPeriodFrames)) {
                    throw std::runtime_error(
                        "Each camera path cut must be a keyframe frame inside "
                        "the period: " + fixtureId);
                }
            }
            const BenchmarkCameraKeyframe& first =
                factory.cameraPathKeyframes.front();
            const BenchmarkCameraKeyframe& last =
                factory.cameraPathKeyframes.back();
            if (factory.cameraPathPeriodFrames != 0u && !wrapCut &&
                (last.position != first.position ||
                    last.target != first.target)) {
                throw std::runtime_error(
                    "A discontinuous periodic camera wrap must be a frame-0 cut: " +
                    fixtureId);
            }
        }

        // Linear interpolation over [frame(i), frame(i+1)); a step keyframe
        // (teleport or cut) holds the previous value until its own frame.
        template <typename Keyframe, typename IsStep>
        std::pair<size_t, float> keyframeSegment(
            const std::vector<Keyframe>& keyframes, uint64_t frame,
            IsStep isStep) noexcept {
            if (frame >= keyframes.back().frame) {
                return { keyframes.size() - 1u, 0.0f };
            }
            const auto next = std::ranges::upper_bound(keyframes, frame, {},
                &Keyframe::frame);
            const size_t upper = static_cast<size_t>(
                next - keyframes.begin());
            const size_t lower = upper - 1u;
            if (isStep(keyframes[upper])) return { lower, 0.0f };
            const double span = static_cast<double>(
                keyframes[upper].frame - keyframes[lower].frame);
            const double offset = static_cast<double>(
                frame - keyframes[lower].frame);
            return { lower, static_cast<float>(offset / span) };
        }

        // Euler degrees with R = Rz(z) * Ry(y) * Rx(x), TransformComponent's
        // convention; double precision so the result is stable per frame.
        using Matrix3d = std::array<std::array<double, 3>, 3>;

        Matrix3d multiply(const Matrix3d& lhs, const Matrix3d& rhs) noexcept {
            Matrix3d result{};
            for (size_t row = 0; row < 3; ++row) {
                for (size_t column = 0; column < 3; ++column) {
                    for (size_t inner = 0; inner < 3; ++inner) {
                        result[row][column] += lhs[row][inner] *
                            rhs[inner][column];
                    }
                }
            }
            return result;
        }

        Matrix3d axisAngle(glm::dvec3 axis, double radians) noexcept {
            const double c = std::cos(radians);
            const double s = std::sin(radians);
            const double t = 1.0 - c;
            return { {
                { t * axis.x * axis.x + c, t * axis.x * axis.y - s * axis.z,
                    t * axis.x * axis.z + s * axis.y },
                { t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,
                    t * axis.y * axis.z - s * axis.x },
                { t * axis.x * axis.z - s * axis.y,
                    t * axis.y * axis.z + s * axis.x, t * axis.z * axis.z + c },
            } };
        }

        Matrix3d eulerZyx(const glm::vec3& degrees) noexcept {
            const double radiansPerDegree = std::numbers::pi / 180.0;
            return multiply(multiply(
                axisAngle({ 0.0, 0.0, 1.0 }, degrees.z * radiansPerDegree),
                axisAngle({ 0.0, 1.0, 0.0 }, degrees.y * radiansPerDegree)),
                axisAngle({ 1.0, 0.0, 0.0 }, degrees.x * radiansPerDegree));
        }

        glm::vec3 toEulerZyx(const Matrix3d& rotation) noexcept {
            const double degreesPerRadian = 180.0 / std::numbers::pi;
            const double sinY = std::clamp(-rotation[2][0], -1.0, 1.0);
            double x = 0.0;
            double z = 0.0;
            const double y = std::asin(sinY);
            if (std::abs(sinY) < 1.0 - 1.0e-12) {
                x = std::atan2(rotation[2][1], rotation[2][2]);
                z = std::atan2(rotation[1][0], rotation[0][0]);
            }
            else {
                // Gimbal lock: fold the X angle into Z.
                z = std::atan2(-rotation[0][1], rotation[1][1]);
            }
            return glm::vec3(static_cast<float>(x * degreesPerRadian),
                static_cast<float>(y * degreesPerRadian),
                static_cast<float>(z * degreesPerRadian));
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
            const std::string factoryKind = factory.at("kind").get<std::string>();
            if (factoryKind == "composition") {
                fixture.sceneFactory.kind =
                    BenchmarkSceneFactoryKind::Composition;
                fixture.sceneFactory.compositionEntities =
                    readCompositionEntities(factory, fixture.id);
            }
            else if (factoryKind != "instanced_grid") {
                throw std::runtime_error("Unsupported benchmark scene factory: " + fixture.id);
            }
            else {
            fixture.sceneFactory.instanceGrid = readUVec3(
                factory.at("instance_grid"), "scene_factory.instance_grid");
            fixture.sceneFactory.instanceSpacing = readVec3(
                factory.at("instance_spacing"), "scene_factory.instance_spacing");
            fixture.sceneFactory.instanceScale = readVec3(
                factory.value("instance_scale",
                    Json::array({ 1.0, 1.0, 1.0 })),
                "scene_factory.instance_scale");
            const std::string instanceSubmission = factory.value(
                "instance_submission", "entities");
            if (instanceSubmission == "entities") {
                fixture.sceneFactory.renderInstanceBatch = false;
            }
            else if (instanceSubmission == "render_batch") {
                fixture.sceneFactory.renderInstanceBatch = true;
            }
            else {
                    throw std::runtime_error(
                        "Unsupported benchmark instance submission: " +
                        fixture.id);
                }
                if (!finiteVec3(fixture.sceneFactory.instanceSpacing) ||
                    !finiteVec3(fixture.sceneFactory.instanceScale) ||
                    std::abs(fixture.sceneFactory.instanceScale.x) <= 1.0e-7f ||
                    std::abs(fixture.sceneFactory.instanceScale.y) <= 1.0e-7f ||
                    std::abs(fixture.sceneFactory.instanceScale.z) <= 1.0e-7f) {
                    throw std::runtime_error("Benchmark instance spacing/scale must be finite and scale must be nonzero: " +
                        fixture.id);
                }
                const uint64_t instanceCount = benchmarkInstanceCount(
                    fixture.sceneFactory.instanceGrid);
                if (instanceCount == 0) {
                    throw std::runtime_error("Benchmark instance count is out of range: " + fixture.id);
                }
                if (factory.contains("instance_scale_override")) {
                    const Json& scaleOverride = factory.at(
                        "instance_scale_override");
                    fixture.sceneFactory.instanceScaleOverrideEnabled = true;
                    fixture.sceneFactory.instanceScaleOverrideIndex =
                        scaleOverride.at("instance_index").get<size_t>();
                    fixture.sceneFactory.instanceScaleOverride = readVec3(
                        scaleOverride.at("scale"),
                        "scene_factory.instance_scale_override.scale");
                    const glm::vec3& scale =
                        fixture.sceneFactory.instanceScaleOverride;
                    if (fixture.sceneFactory.instanceScaleOverrideIndex >=
                            instanceCount ||
                        !finiteVec3(scale) ||
                        std::abs(scale.x) <= 1.0e-7f ||
                        std::abs(scale.y) <= 1.0e-7f ||
                        std::abs(scale.z) <= 1.0e-7f) {
                        throw std::runtime_error(
                            "Invalid benchmark instance-scale override: " +
                            fixture.id);
                    }
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
                    if (motion.contains("step")) {
                        const Json& step = motion.at("step");
                        fixture.sceneFactory.objectStepEnabled = true;
                        fixture.sceneFactory.objectStepInstanceIndex =
                            step.at("instance_index").get<size_t>();
                        fixture.sceneFactory.objectStepFrame =
                            step.at("frame").get<uint64_t>();
                        fixture.sceneFactory.objectStepOffset = readVec3(
                            step.at("offset"), "object_motion.step.offset");
                        if (fixture.sceneFactory.objectStepInstanceIndex >= instanceCount ||
                            !finiteVec3(fixture.sceneFactory.objectStepOffset)) {
                            throw std::runtime_error(
                                "Invalid benchmark object-motion step: " + fixture.id);
                        }
                    }
                }
                if (factory.contains("object_visibility")) {
                    const Json& visibility = factory.at("object_visibility");
                    const Json& step = visibility.at("step");
                    fixture.sceneFactory.objectVisibilityStepEnabled = true;
                    fixture.sceneFactory.objectVisibilityStepInstanceIndex =
                        step.at("instance_index").get<size_t>();
                    fixture.sceneFactory.objectVisibilityStepFrame =
                        step.at("frame").get<uint64_t>();
                    fixture.sceneFactory.objectVisibilityAfterStep =
                        step.at("enabled").get<bool>();
                    if (fixture.sceneFactory.objectVisibilityStepInstanceIndex >=
                            instanceCount) {
                        throw std::runtime_error(
                            "Invalid benchmark object-visibility step: " +
                            fixture.id);
                    }
                }
                if (fixture.sceneFactory.renderInstanceBatch &&
                    (fixture.sceneFactory.animateInstances ||
                        fixture.sceneFactory.objectStepEnabled ||
                        fixture.sceneFactory.objectVisibilityStepEnabled ||
                        fixture.sceneFactory.instanceScaleOverrideEnabled)) {
                    throw std::runtime_error(
                        "Render-batch benchmark per-instance overrides are not implemented: " +
                        fixture.id);
                }
            }
            if (factory.contains("camera_motion") &&
                factory.at("camera_motion").contains("path")) {
                const Json& motion = factory.at("camera_motion");
                requireKnownKeys(motion, { "path" }, "camera_motion with a path",
                    fixture.id);
                readCameraPath(motion.at("path"), fixture.camera,
                    fixture.sceneFactory, fixture.id);
            }
            else if (factory.contains("camera_motion")) {
                const Json& motion = factory.at("camera_motion");
                fixture.sceneFactory.cameraVelocityPerFrame = readVec3(
                    motion.value("velocity_per_frame", Json::array({ 0.0, 0.0, 0.0 })),
                    "camera_motion.velocity_per_frame");
                if (!finiteVec3(fixture.sceneFactory.cameraVelocityPerFrame)) {
                    throw std::runtime_error("Benchmark camera velocity must be finite: " +
                        fixture.id);
                }
                if (motion.contains("oscillation")) {
                    const Json& oscillation = motion.at("oscillation");
                    fixture.sceneFactory.cameraOscillationAmplitude = readVec3(
                        oscillation.at("amplitude"), "camera_motion.oscillation.amplitude");
                    fixture.sceneFactory.cameraOscillationPeriodFrames = oscillation.at("period_frames").get<uint64_t>();
                    if (!finiteVec3(fixture.sceneFactory.cameraOscillationAmplitude) ||
                        fixture.sceneFactory.cameraOscillationPeriodFrames == 0)
                        throw std::runtime_error("Invalid benchmark camera oscillation: " + fixture.id);
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

            if (source.contains("lights")) {
                const Json& lights = source.at("lights");
                if (!lights.is_array() || lights.size() > 256u) {
                    throw std::runtime_error(
                        "Benchmark lights must be an array of at most 256 entries: " +
                        fixture.id);
                }
                fixture.lights.reserve(lights.size());
                for (const Json& lightSource : lights) {
                    BenchmarkLight light{};
                    light.type = readLightType(lightSource.at("type"), fixture.id);
                    light.position = readVec3(lightSource.at("position"),
                        "lights.position");
                    light.rotationDegrees = readVec3(
                        lightSource.at("rotation_degrees"),
                        "lights.rotation_degrees");
                    light.colorLinearRec709 = readVec3(
                        lightSource.at("color_linear_rec709"),
                        "lights.color_linear_rec709");
                    light.illuminanceLux = lightSource.value(
                        "illuminance_lux", 100'000.0f);
                    light.luminousIntensityCandela = lightSource.value(
                        "luminous_intensity_candela", 10'000.0f);
                    light.rangeMeters = lightSource.value("range_meters", 10.0f);
                    light.sourceRadiusMeters = lightSource.value(
                        "source_radius_meters", 0.05f);
                    light.innerConeDegrees = lightSource.value(
                        "inner_cone_degrees", 12.5f);
                    light.outerConeDegrees = lightSource.value(
                        "outer_cone_degrees", 45.0f);
                    light.castsShadows = lightSource.value("casts_shadows", true);
                    light.shadowQuality = readShadowQuality(
                        lightSource.value("shadow_quality", Json("high")),
                        fixture.id);
                    light.priority = lightSource.value("priority", int32_t{ 0 });

                    if (!finiteVec3(light.position) ||
                        !finiteVec3(light.rotationDegrees) ||
                        !finiteVec3(light.colorLinearRec709) ||
                        glm::any(glm::lessThan(light.colorLinearRec709,
                            glm::vec3(0.0f))) ||
                        !std::isfinite(light.illuminanceLux) ||
                        !std::isfinite(light.luminousIntensityCandela) ||
                        !std::isfinite(light.rangeMeters) ||
                        !std::isfinite(light.sourceRadiusMeters) ||
                        !std::isfinite(light.innerConeDegrees) ||
                        !std::isfinite(light.outerConeDegrees) ||
                        light.illuminanceLux < 0.0f ||
                        light.luminousIntensityCandela < 0.0f ||
                        light.rangeMeters <= 0.0f ||
                        light.sourceRadiusMeters < 0.0f ||
                        light.innerConeDegrees <= 0.0f ||
                        light.innerConeDegrees > light.outerConeDegrees ||
                        light.outerConeDegrees >= 180.0f) {
                        throw std::runtime_error(
                            "Invalid benchmark light parameters in fixture: " +
                            fixture.id);
                    }
                    fixture.lights.push_back(light);
                }
            }

            if (source.contains("reflection_probe_capture")) {
                const Json& probeSource =
                    source.at("reflection_probe_capture");
                const std::string owner = lowercase(
                    probeSource.value("owner", "standalone"));
                if (owner != "standalone") {
                    throw std::runtime_error(
                        "Benchmark reflection-probe capture owner must be standalone: " +
                        fixture.id);
                }
                BenchmarkReflectionProbeCapture probe{};
                probe.position = readVec3(probeSource.at("position"),
                    "reflection_probe_capture.position");
                probe.updateMode = readReflectionProbeUpdateMode(
                    probeSource.value("update_mode", Json("on_demand")),
                    fixture.id);
                probe.resolution = probeSource.value(
                    "resolution", uint32_t{ 512 });
                probe.nearPlane = probeSource.value("near", 0.1f);
                probe.farPlane = probeSource.value("far", 100.0f);
                probe.influenceRadiusMeters = probeSource.value(
                    "influence_radius_meters", 1'000.0f);
                probe.priority = probeSource.value("priority", int32_t{ 2 });
                probe.captureSky = probeSource.value("capture_sky", true);
                if (!finiteVec3(probe.position) ||
                    !supportedReflectionProbeResolution(probe.resolution) ||
                    !std::isfinite(probe.nearPlane) ||
                    !std::isfinite(probe.farPlane) ||
                    !std::isfinite(probe.influenceRadiusMeters) ||
                    probe.nearPlane <= 0.0f ||
                    probe.farPlane <= probe.nearPlane ||
                    probe.influenceRadiusMeters <= 0.0f) {
                    throw std::runtime_error(
                        "Invalid benchmark reflection-probe capture: " +
                        fixture.id);
                }
                fixture.reflectionProbeCapture = probe;
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
        const BenchmarkSceneFactory& factory = fixture.sceneFactory;
        if (factory.cameraPathEnabled) {
            const std::vector<BenchmarkCameraKeyframe>& keyframes =
                factory.cameraPathKeyframes;
            const uint64_t localFrame = factory.cameraPathPeriodFrames != 0u
                ? frameIndex % factory.cameraPathPeriodFrames : frameIndex;
            const auto [index, weight] = keyframeSegment(keyframes, localFrame,
                [&factory](const BenchmarkCameraKeyframe& keyframe) {
                    return std::ranges::find(factory.cameraPathCuts,
                        keyframe.frame) != factory.cameraPathCuts.end();
                });
            const BenchmarkCameraKeyframe& from = keyframes[index];
            if (weight == 0.0f) return { from.position, from.target };
            const BenchmarkCameraKeyframe& to = keyframes[index + 1u];
            return { glm::mix(from.position, to.position, weight),
                glm::mix(from.target, to.target, weight) };
        }
        BenchmarkCameraPose pose{ fixture.camera.position, fixture.camera.target };
        uint64_t segmentFrame = frameIndex;
        if (fixture.sceneFactory.cameraCutEnabled &&
            frameIndex >= fixture.sceneFactory.cameraCutFrame) {
            pose.position = fixture.sceneFactory.cameraCutPosition;
            pose.target = fixture.sceneFactory.cameraCutTarget;
            segmentFrame = frameIndex - fixture.sceneFactory.cameraCutFrame;
        }
        glm::vec3 offset = fixture.sceneFactory.cameraVelocityPerFrame *
            static_cast<float>(segmentFrame);
        if (fixture.sceneFactory.cameraOscillationPeriodFrames != 0) {
            const double phase = 2.0 * std::numbers::pi * static_cast<double>(
                segmentFrame % fixture.sceneFactory.cameraOscillationPeriodFrames) /
                static_cast<double>(fixture.sceneFactory.cameraOscillationPeriodFrames);
            offset += fixture.sceneFactory.cameraOscillationAmplitude * static_cast<float>(std::sin(phase));
        }
        pose.position += offset;
        pose.target += offset;
        return pose;
    }

    uint64_t evaluateBenchmarkViewHistoryResetRevision(
        const BenchmarkFixture& fixture, uint64_t frameIndex) noexcept {
        const BenchmarkSceneFactory& factory = fixture.sceneFactory;
        if (!factory.cameraPathEnabled) {
            return factory.cameraCutEnabled &&
                frameIndex >= factory.cameraCutFrame ? 1u : 0u;
        }
        const uint64_t period = factory.cameraPathPeriodFrames;
        const uint64_t cycles = period != 0u ? frameIndex / period : 0u;
        const uint64_t localFrame = period != 0u ? frameIndex % period
            : frameIndex;
        uint64_t revision = 0;
        for (const uint64_t cut : factory.cameraPathCuts) {
            // A frame-0 cut is the periodic wrap: it first occurs at `period`.
            if (cut == 0u) revision += cycles;
            else revision += cycles + (cut <= localFrame ? 1u : 0u);
        }
        return revision;
    }

    BenchmarkEntityPose evaluateBenchmarkCompositionEntity(
        const BenchmarkCompositionEntity& entity, uint64_t frameIndex) noexcept {
        BenchmarkEntityPose pose{ entity.translation, entity.rotationDegrees,
            entity.scale, false };
        const BenchmarkEntityMotion& motion = entity.motion;
        const uint64_t localFrame = motion.periodFrames != 0u
            ? frameIndex % motion.periodFrames : frameIndex;
        const bool wrapped = motion.periodFrames != 0u && localFrame == 0u &&
            frameIndex != 0u;
        switch (motion.kind) {
        case BenchmarkEntityMotionKind::None:
            break;
        case BenchmarkEntityMotionKind::Linear:
            pose.translation += motion.velocityPerFrame *
                static_cast<float>(localFrame);
            pose.teleported = wrapped &&
                motion.velocityPerFrame != glm::vec3(0.0f);
            break;
        case BenchmarkEntityMotionKind::Rotation: {
            const double degrees = std::fmod(
                static_cast<double>(motion.rotationDegreesPerFrame) *
                    static_cast<double>(frameIndex), 360.0);
            if (degrees != 0.0) {
                const Matrix3d spin = axisAngle(glm::dvec3(motion.rotationAxis),
                    degrees * std::numbers::pi / 180.0);
                pose.rotationDegrees = toEulerZyx(multiply(spin,
                    eulerZyx(entity.rotationDegrees)));
            }
            break;
        }
        case BenchmarkEntityMotionKind::Keyframes: {
            const auto [index, weight] = keyframeSegment(motion.keyframes,
                localFrame, [](const BenchmarkTranslationKeyframe& keyframe) {
                    return keyframe.teleport;
                });
            const BenchmarkTranslationKeyframe& from = motion.keyframes[index];
            pose.translation = weight == 0.0f ? from.translation
                : glm::mix(from.translation,
                    motion.keyframes[index + 1u].translation, weight);
            pose.teleported = (index > 0u && from.frame == localFrame &&
                    from.teleport) ||
                (wrapped && motion.keyframes.front().teleport);
            break;
        }
        }
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

    glm::vec3 evaluateBenchmarkInstanceOffset(const BenchmarkSceneFactory& factory,
        uint64_t frameIndex, size_t instanceIndex) noexcept {
        glm::vec3 offset(0.0f);
        offset.y = evaluateBenchmarkInstanceYOffset(factory, frameIndex,
            instanceIndex);
        if (factory.objectStepEnabled &&
            instanceIndex == factory.objectStepInstanceIndex &&
            frameIndex >= factory.objectStepFrame) {
            offset += factory.objectStepOffset;
        }
        return offset;
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
