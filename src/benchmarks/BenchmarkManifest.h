#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Iridium {

    class ProjectAssetRoots;

    struct BenchmarkContentFile {
        std::filesystem::path relativePath;
        std::filesystem::path path;
        std::string sha256;
    };

    struct BenchmarkCamera {
        std::string id;
        glm::vec3 position{ 0.0f, 0.0f, 3.0f };
        glm::vec3 target{ 0.0f };
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        float verticalFovDegrees = 45.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
    };

    enum class BenchmarkLightType : uint8_t {
        Directional,
        Point,
        Spot,
    };

    enum class BenchmarkShadowQuality : uint8_t {
        Low,
        Medium,
        High,
        Ultra,
    };

    // Explicit fixture-owned lights keep visual/performance evidence independent
    // from fixture-name conventions and editor defaults.
    struct BenchmarkLight {
        BenchmarkLightType type = BenchmarkLightType::Directional;
        glm::vec3 position{ 0.0f };
        glm::vec3 rotationDegrees{ 0.0f };
        glm::vec3 colorLinearRec709{ 1.0f };
        float illuminanceLux = 100'000.0f;
        float luminousIntensityCandela = 10'000.0f;
        float rangeMeters = 10.0f;
        float sourceRadiusMeters = 0.05f;
        float innerConeDegrees = 12.5f;
        float outerConeDegrees = 45.0f;
        bool castsShadows = true;
        BenchmarkShadowQuality shadowQuality = BenchmarkShadowQuality::High;
        int32_t priority = 0;
    };

    enum class BenchmarkReflectionProbeUpdateMode : uint8_t {
        OnDemand,
        Realtime,
    };

    // A standalone capture owner is intentionally separate from fixture
    // geometry: all model instances remain eligible for the six capture faces,
    // and the published result can be evaluated in the main-view reflections.
    struct BenchmarkReflectionProbeCapture {
        glm::vec3 position{ 0.0f };
        BenchmarkReflectionProbeUpdateMode updateMode =
            BenchmarkReflectionProbeUpdateMode::OnDemand;
        uint32_t resolution = 512;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
        float influenceRadiusMeters = 1'000.0f;
        int32_t priority = 2;
        bool captureSky = true;
    };

    enum class BenchmarkSceneFactoryKind : uint8_t {
        // One cooked model instanced over a grid (the M0-M7 factory).
        InstancedGrid,
        // M9 G6b: separately placed and animated entities, each drawing one
        // top-level glTF node of the fixture's single source asset.
        Composition,
    };

    enum class BenchmarkEntityMotionKind : uint8_t {
        None,
        // translation = base + velocity * frame (wrapped by periodFrames).
        Linear,
        // World-axis rotation about the entity pivot, degrees per frame.
        Rotation,
        // Linearly interpolated translation keyframes; a keyframe flagged as a
        // teleport is reached by a discontinuous jump at its frame.
        Keyframes,
    };

    struct BenchmarkTranslationKeyframe {
        uint64_t frame = 0;
        glm::vec3 translation{ 0.0f };
        // The step from frame-1 to frame is discontinuous (the previous
        // keyframe's value is held until this frame). On the frame-0 keyframe
        // of a periodic motion it marks the wrap as a teleport.
        bool teleport = false;
    };

    struct BenchmarkEntityMotion {
        BenchmarkEntityMotionKind kind = BenchmarkEntityMotionKind::None;
        glm::vec3 velocityPerFrame{ 0.0f };
        glm::vec3 rotationAxis{ 0.0f, 1.0f, 0.0f };
        float rotationDegreesPerFrame = 0.0f;
        std::vector<BenchmarkTranslationKeyframe> keyframes;
        // Zero: not periodic. Otherwise the motion is evaluated at
        // frame % periodFrames (Linear and Keyframes only).
        uint64_t periodFrames = 0;
    };

    struct BenchmarkCompositionEntity {
        // Unique within the fixture; seeds the entity's stable scene identity.
        std::string id;
        // Top-level glTF node of the fixture source asset. Node transforms are
        // baked at cook time, so the node's geometry is authored about the
        // entity pivot and `translation/rotation/scale` place it.
        uint32_t sourceNode = 0;
        glm::vec3 translation{ 0.0f };
        // TransformComponent convention: R = Rz * Ry * Rx, degrees.
        glm::vec3 rotationDegrees{ 0.0f };
        glm::vec3 scale{ 1.0f };
        BenchmarkEntityMotion motion;
    };

    struct BenchmarkCameraKeyframe {
        uint64_t frame = 0;
        glm::vec3 position{ 0.0f };
        glm::vec3 target{ 0.0f };
    };

    struct BenchmarkSceneFactory {
        BenchmarkSceneFactoryKind kind = BenchmarkSceneFactoryKind::InstancedGrid;
        std::vector<BenchmarkCompositionEntity> compositionEntities;
        glm::uvec3 instanceGrid{ 1, 1, 1 };
        glm::vec3 instanceSpacing{ 0.0f };
        glm::vec3 instanceScale{ 1.0f };
        bool instanceScaleOverrideEnabled = false;
        size_t instanceScaleOverrideIndex = 0;
        glm::vec3 instanceScaleOverride{ 1.0f };
        bool renderInstanceBatch = false;
        bool animateInstances = false;
        float motionAmplitude = 0.0f;
        uint64_t motionPeriodFrames = 1;
        bool objectStepEnabled = false;
        size_t objectStepInstanceIndex = 0;
        uint64_t objectStepFrame = 0;
        glm::vec3 objectStepOffset{ 0.0f };
        bool objectVisibilityStepEnabled = false;
        size_t objectVisibilityStepInstanceIndex = 0;
        uint64_t objectVisibilityStepFrame = 0;
        bool objectVisibilityAfterStep = true;
        glm::vec3 cameraVelocityPerFrame{ 0.0f };
        glm::vec3 cameraOscillationAmplitude{ 0.0f };
        uint64_t cameraOscillationPeriodFrames = 1;
        bool cameraCutEnabled = false;
        uint64_t cameraCutFrame = 0;
        glm::vec3 cameraCutPosition{ 0.0f };
        glm::vec3 cameraCutTarget{ 0.0f };
        // camera_motion.path (exclusive with velocity, oscillation and cut):
        // linearly interpolated keyframes. Each cut frame is a keyframe
        // reached by a jump, and bumps the view history-reset revision.
        bool cameraPathEnabled = false;
        std::vector<BenchmarkCameraKeyframe> cameraPathKeyframes;
        std::vector<uint64_t> cameraPathCuts;
        uint64_t cameraPathPeriodFrames = 0;
    };

    struct BenchmarkFixture {
        std::string id;
        uint32_t revision = 0;
        bool required = true;
        std::filesystem::path sourceAsset;
        glm::vec3 constantEnvironmentLinear{ 0.18f };
        BenchmarkCamera camera;
        BenchmarkSceneFactory sceneFactory;
        std::vector<BenchmarkLight> lights;
        std::optional<BenchmarkReflectionProbeCapture>
            reflectionProbeCapture;
        std::string outputLabel;
        uint64_t warmupFrames = 500;
        uint64_t measuredFrames = 10000;
        std::vector<BenchmarkContentFile> contentFiles;
        std::vector<std::string> expectedBehavior;
        std::vector<std::string> unavailableCapabilities;
    };

    struct BenchmarkManifest {
        uint32_t schemaVersion = 0;
        std::filesystem::path sourcePath;
        std::vector<BenchmarkFixture> fixtures;
        struct LocalDiagnostic {
            std::string id;
            std::string category;
            std::filesystem::path sourceAsset;
            std::string license;
            std::string treeSha256;
            std::string invocation;
            std::vector<std::string> expectedBehavior;
            std::vector<std::string> unavailableCapabilities;
        };
        std::vector<LocalDiagnostic> localDiagnostics;
    };

    struct BenchmarkCameraPose {
        glm::vec3 position{ 0.0f };
        glm::vec3 target{ 0.0f };
    };

    struct BenchmarkEntityPose {
        glm::vec3 translation{ 0.0f };
        glm::vec3 rotationDegrees{ 0.0f };
        glm::vec3 scale{ 1.0f };
        // The pose at this frame is discontinuous with the previous frame's
        // (a teleport keyframe or a discontinuous periodic wrap). Consumers
        // reset per-instance temporal history on it.
        bool teleported = false;
    };

    [[nodiscard]] BenchmarkManifest loadBenchmarkManifest(
        const std::filesystem::path& path, bool verifyContentHashes = true);
    // A manifest content path, relative to the manifest's directory and never
    // escaping it. When the target is missing there and the directory lies inside
    // the project asset root, the same relative location under the local asset
    // library is used when it exists (licensed third-party content lives there),
    // with the escape check applied to that root. Otherwise the manifest-relative
    // path is returned, so missing-content errors name it as before.
    // loadBenchmarkManifest resolves through ProjectAssetRoots::current().
    [[nodiscard]] std::filesystem::path resolveBenchmarkContentPath(
        const std::filesystem::path& manifestDirectory,
        const std::filesystem::path& relative,
        const ProjectAssetRoots& roots);
    [[nodiscard]] const BenchmarkFixture& findBenchmarkFixture(
        const BenchmarkManifest& manifest, const std::string& id);
    [[nodiscard]] BenchmarkCameraPose evaluateBenchmarkCamera(
        const BenchmarkFixture& fixture, uint64_t frameIndex) noexcept;
    // 0 before any cut. A legacy single cut gives 1 from its frame on; a path
    // gives the ordinal of the latest cut reached (counting periodic repeats).
    [[nodiscard]] uint64_t evaluateBenchmarkViewHistoryResetRevision(
        const BenchmarkFixture& fixture, uint64_t frameIndex) noexcept;
    // Deterministic in the benchmark frame index only.
    [[nodiscard]] BenchmarkEntityPose evaluateBenchmarkCompositionEntity(
        const BenchmarkCompositionEntity& entity, uint64_t frameIndex) noexcept;
    [[nodiscard]] float evaluateBenchmarkInstanceYOffset(
        const BenchmarkSceneFactory& factory, uint64_t frameIndex,
        size_t instanceIndex) noexcept;
    [[nodiscard]] glm::vec3 evaluateBenchmarkInstanceOffset(
        const BenchmarkSceneFactory& factory, uint64_t frameIndex,
        size_t instanceIndex) noexcept;
    [[nodiscard]] uint64_t benchmarkInstanceCount(glm::uvec3 grid) noexcept;

} // namespace Iridium
