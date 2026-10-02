#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Iridium {

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

    struct BenchmarkSceneFactory {
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

    [[nodiscard]] BenchmarkManifest loadBenchmarkManifest(
        const std::filesystem::path& path, bool verifyContentHashes = true);
    [[nodiscard]] const BenchmarkFixture& findBenchmarkFixture(
        const BenchmarkManifest& manifest, const std::string& id);
    [[nodiscard]] BenchmarkCameraPose evaluateBenchmarkCamera(
        const BenchmarkFixture& fixture, uint64_t frameIndex) noexcept;
    [[nodiscard]] float evaluateBenchmarkInstanceYOffset(
        const BenchmarkSceneFactory& factory, uint64_t frameIndex,
        size_t instanceIndex) noexcept;
    [[nodiscard]] glm::vec3 evaluateBenchmarkInstanceOffset(
        const BenchmarkSceneFactory& factory, uint64_t frameIndex,
        size_t instanceIndex) noexcept;
    [[nodiscard]] uint64_t benchmarkInstanceCount(glm::uvec3 grid) noexcept;

} // namespace Iridium
