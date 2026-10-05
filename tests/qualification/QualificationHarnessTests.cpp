// M7R R2.6: the qualification harness against a device-free backend. Covers the
// run policy, which hook issues which capture/readback request, requests lost to a
// frame that never opened, validator sequences, and probe-resource ownership.

#include "qualification/harness/QualificationHarness.h"
#include "FakeRenderBackend.h"

#include "profiling/CpuProfiler.h"
#include "scene/SceneWorld.h"
#include "scene/components/LightComponent.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace {

    using namespace Iridium;
    using namespace IridiumTest;
    using Kind = BackendCall::Kind;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    struct HarnessRig {
        ApplicationConfig config;
        QualificationOptions options;
        FakeRenderBackend backend;
        FakeQualificationBackend* qualification = nullptr;
        FakeAppControl control;
        CpuProfiler profiler{ false };
        SceneWorld scene;
        AppCamera camera;
        AppStartupTimings timings;
        AppFrameRequests requests;
        std::shared_ptr<ModelAsset> mainModel;
        std::unique_ptr<QualificationHarness> harness;

        void start() {
            auto fake = std::make_unique<FakeQualificationBackend>(backend);
            qualification = fake.get();
            harness = std::make_unique<QualificationHarness>(options,
                std::move(fake));
        }

        AppStartupContext startupContext() {
            AppStartupContext context{
                .config = config,
                .profiler = profiler,
                .control = control,
                .scene = scene,
                .camera = camera,
                .timings = timings,
            };
            context.backend = &backend;
            return context;
        }

        // One frame through every frame hook. `opened == false` models a
        // beginFrame that asked for a swapchain recreate: the frame returns before
        // BackendFrameOpened and nothing after it runs.
        void runFrame(uint64_t applicationFrame,
            std::optional<uint64_t> measuredFrame, bool opened = true,
            bool transportPending = false) {
            requests = {};
            AppFrameContext frame{
                .config = config,
                .backend = backend,
                .profiler = profiler,
                .scene = scene,
                .camera = camera,
                .control = control,
                .requests = requests,
                .mainModel = mainModel,
                .applicationFrameIndex = applicationFrame,
                .measuredFrameIndex = measuredFrame,
            };
            backend.marker = "pre";
            harness->onFrameBegin(FrameBeginPhase::PreSceneUpdate, frame);
            backend.marker = "post";
            harness->onFrameBegin(FrameBeginPhase::PostSceneUpdate, frame);
            if (!opened) return;
            backend.marker = "opened";
            harness->onFrameBegin(FrameBeginPhase::BackendFrameOpened, frame);
            backend.marker = "scene-linear";
            harness->onFrameSubmit(FrameSubmitPoint::SceneLinearReady, frame);
            backend.marker = "output";
            harness->onFrameSubmit(FrameSubmitPoint::OutputReady, frame);
            backend.marker = "end";
            frame.outputTransportPending = transportPending;
            harness->onFrameEnd(frame);
        }

        // Warmup frames are unmeasured; measured frame m is application frame
        // warmup + m.
        void runMeasured(uint64_t warmup, uint64_t measuredFrames) {
            for (uint64_t frame = 0; frame < warmup + measuredFrames; ++frame) {
                runFrame(frame, frame >= warmup
                    ? std::optional<uint64_t>(frame - warmup) : std::nullopt);
            }
        }
    };

    bool testRunPolicy() {
        struct DefaultObserver final : IFrameObserver {};
        const AppRunPolicy observerDefault = DefaultObserver{}.runPolicy();
        CHECK(!observerDefault.deterministicContent);
        CHECK(!observerDefault.fullscreenScenePresentation);
        CHECK(!observerDefault.colorValidationOverlay);
        CHECK(!observerDefault.ownsStartupContent);
        const AppFrameRequests requests{};
        CHECK(!requests.viewHistoryResetRevision);
        CHECK(!requests.suppressGridOverlay);

        const AppRenderRouting& production = observerDefault.routing;
        CHECK(!production.forceDirectGBufferReference);
        CHECK(!production.forceDirectShadowReference);
        CHECK(!production.forceDirectProbeCaptureReference);
        CHECK(production.weightedOitOrderSeed == 0u);
        CHECK(production.gpuLodMinimumResidentLevel == 0u);
        CHECK(DefaultObserver{}.backendExtensions().empty());

        QualificationOptions options{};
        const AppRunPolicy interactive = qualificationRunPolicy(options);
        CHECK(!interactive.deterministicContent);
        CHECK(!interactive.fullscreenScenePresentation);
        CHECK(!interactive.colorValidationOverlay);
        CHECK(!interactive.ownsStartupContent);
        CHECK(!interactive.routing.forceDirectGBufferReference);

        options.benchmarkId = "ordinary2_lit_closed_v1";
        FakeRenderBackend log;
        QualificationHarness harness(options,
            std::make_unique<FakeQualificationBackend>(log));
        const AppRunPolicy benchmark = harness.runPolicy();
        CHECK(benchmark.deterministicContent);
        CHECK(benchmark.fullscreenScenePresentation);
        CHECK(!benchmark.colorValidationOverlay);
        CHECK(benchmark.ownsStartupContent);
        // The harness supplies exactly its backend's extension.
        CHECK(harness.backendExtensions().size() == 1u);

        options.benchmarkId = "color_volume_transparency_v1";
        CHECK(qualificationRunPolicy(options).colorValidationOverlay);

        // Reference routes reach the Application only as routing.
        options.forceDirectGBufferReference = true;
        options.forceDirectShadowReference = true;
        options.forceDirectProbeCaptureReference = true;
        options.weightedOitOrderSeed = 63u;
        options.gpuLodMinimumResidentLevel = 2u;
        const AppRenderRouting routing = qualificationRunPolicy(options).routing;
        CHECK(routing.forceDirectGBufferReference);
        CHECK(routing.forceDirectShadowReference);
        CHECK(routing.forceDirectProbeCaptureReference);
        CHECK(routing.weightedOitOrderSeed == 63u);
        CHECK(routing.gpuLodMinimumResidentLevel == 2u);
        return true;
    }

    bool testBackendConfiguredAtConfigure() {
        HarnessRig rig;
        rig.options.shadowIndirectQualificationOracle = true;
        rig.options.virtualShadowDepthQualificationOracle = true;
        rig.options.validateReflectionProbes = true;
        rig.start();
        CHECK(!rig.qualification->configured);
        AppStartupContext startup = rig.startupContext();
        startup.backend = nullptr;   // Configure runs before the backend exists
        rig.harness->onStartup(StartupPhase::Configure, startup);
        CHECK(rig.qualification->configured.has_value());
        const QualificationBackendConfig& config = *rig.qualification->configured;
        CHECK(config.shadowIndirectOracle);
        CHECK(!config.gpuLodOracle);
        CHECK(!config.probeLodOracle);
        CHECK(!config.depthOcclusionOracle);
        CHECK(config.virtualShadowDepthOracle);
        CHECK(config.validateProbeCaptureTargets);
        CHECK(rig.backend.calls.empty());
        return true;
    }

    bool testCapturePoints() {
        for (const FrameCapturePoint point : { FrameCapturePoint::SceneLinear,
                FrameCapturePoint::FinalSdr, FrameCapturePoint::FinalOutput }) {
            HarnessRig rig;
            rig.options.captureFrameIndex = 2u;
            rig.options.capturePoint = point;
            rig.start();
            for (uint64_t frame = 0; frame < 6u; ++frame) {
                const std::optional<uint64_t> measured = frame >= 1u
                    ? std::optional<uint64_t>(frame - 1u) : std::nullopt;
                rig.runFrame(frame, measured);
                // Only the capture frame hides the editor grid (no benchmark).
                CHECK(rig.requests.suppressGridOverlay == (measured == 2u));
            }
            CHECK(rig.backend.calls.size() == 1u);
            const BackendCall& capture = rig.backend.calls.front();
            CHECK(capture.kind == Kind::ArmFrameCapture);
            CHECK(capture.value == 2u);
            CHECK(capture.detail == static_cast<uint32_t>(point));
            // Armed inside the open frame; the extension consumes it at the
            // scene-color or final-capture hook of the same recording.
            CHECK(capture.marker == "opened");
        }
        return true;
    }

    bool testReadbackValidationsArmInsideTheOpenFrame() {
        HarnessRig rig;
        rig.options.validateDepthPyramidCapture = true;
        rig.options.validateDeepLayeredCapture = true;
        rig.options.deepLayeredCaptureQuality = TransparencyQuality::Cinematic8;
        rig.start();
        rig.runMeasured(2u, 4u);
        CHECK(rig.backend.calls.size() == 2u);
        CHECK(rig.backend.calls[0].kind == Kind::ArmDeepLayeredValidation);
        CHECK(rig.backend.calls[0].value == 0u);
        CHECK(rig.backend.calls[0].detail ==
            static_cast<uint32_t>(TransparencyQuality::Cinematic8));
        CHECK(rig.backend.calls[0].marker == "opened");
        CHECK(rig.backend.calls[1].kind == Kind::ArmDepthPyramidValidation);
        CHECK(rig.backend.calls[1].marker == "opened");
        return true;
    }

    bool testRequestsLostWithAnUnopenedFrame() {
        // A beginFrame swapchain recreate drops that frame's capture and
        // readback requests; nothing re-arms them later (pre-R2 behavior, which
        // the end-of-run report then rejects).
        HarnessRig rig;
        rig.options.captureFrameIndex = 0u;
        rig.options.validateOrdinary2Capture = true;
        rig.start();
        rig.runFrame(0u, 0u, false);
        rig.runFrame(1u, 1u);
        rig.runFrame(2u, 2u);
        CHECK(rig.backend.calls.empty());
        return true;
    }

    bool testResizeSequenceRunsBetweenFrames() {
        HarnessRig rig;
        rig.options.validateOrdinary2Resize = true;
        rig.start();
        rig.runMeasured(1u, 7u);
        CHECK(rig.control.resizes.size() == 3u);
        CHECK(rig.control.resizes[0].width == 960u &&
            rig.control.resizes[0].height == 540u);
        CHECK(rig.control.resizes[1].width == 1600u &&
            rig.control.resizes[1].height == 900u);
        // The original extent is recorded by the ScenePrerequisites checks,
        // which this rig does not run.
        CHECK(rig.control.resizes[2].width == 0u);
        CHECK(rig.backend.calls.size() == 1u);
        CHECK(rig.backend.calls[0].kind == Kind::ArmOrdinary2Validation);
        CHECK(rig.backend.calls[0].marker == "opened");
        return true;
    }

    bool testOutputTransportSwitchSequence() {
        HarnessRig rig;
        rig.options.validateOutputTransportSwitch = true;
        rig.start();
        rig.runFrame(0u, 0u, true, true);   // an editor switch is pending: wait
        CHECK(rig.control.transports.empty());
        for (uint64_t frame = 1u; frame < 6u; ++frame) rig.runFrame(frame, frame);
        CHECK(rig.control.transports.size() == 3u);
        CHECK(rig.control.transports[0] == Color::OutputTransport::ScRgb);
        CHECK(rig.control.transports[1] == Color::OutputTransport::Hdr10Pq);
        CHECK(rig.control.transports[2] == Color::OutputTransport::SdrSrgb);
        return true;
    }

    bool testTableScaleProbesAreHarnessOwned() {
        HarnessRig rig;
        rig.options.validateTextureTableScale = 3u;
        rig.options.validateMaterialTableScale = 2u;
        rig.start();
        AppStartupContext startup = rig.startupContext();
        rig.backend.marker = "backend-ready";
        rig.harness->onStartup(StartupPhase::BackendReady, startup);
        CHECK(rig.backend.count(Kind::AllocateTexture) == 4u);
        CHECK(rig.backend.count(Kind::AllocateMaterial) == 2u);

        const AppRunSnapshot run{};
        AppShutdownContext shutdown{
            .config = rig.config,
            .profiler = rig.profiler,
            .backend = &rig.backend,
            .completed = false,
            .run = run,
        };
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        CHECK(rig.backend.count(Kind::FreeTexture) == 4u);
        CHECK(rig.backend.count(Kind::FreeMaterial) == 2u);
        // Released once; a second release (or one without a backend) is inert.
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        shutdown.backend = nullptr;
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        CHECK(rig.backend.count(Kind::FreeTexture) == 4u);
        return true;
    }

    // M7R R4c.0: scripted changes apply at PreSceneUpdate of their measured
    // frame (never during warmup or later hooks), and their resources are
    // harness-owned.
    bool testScriptedChangesApplyAtTheirMeasuredFrame() {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            "iridium-scripted-changes-test.json";
        {
            std::ofstream file(path, std::ios::binary);
            file << R"({"schema":"iridium.qualification.scripted_changes.v1",)"
                R"("id":"t","events":[)"
                R"({"frame":1,"action":"add_materials","count":3},)"
                R"({"frame":3,"action":"add_lights","count":5},)"
                R"({"frame":4,"action":"remove_lights","count":2}]})";
        }
        HarnessRig rig;
        rig.options.scriptedChanges = path;
        rig.options.cpuProfileOutput = "unused.jsonl";
        rig.config.frameLimit = 6u;
        rig.profiler.setEnabled(true);
        rig.start();
        AppStartupContext startup = rig.startupContext();
        rig.harness->onStartup(StartupPhase::Configure, startup);
        rig.harness->onStartup(StartupPhase::BackendReady, startup);
        rig.harness->onStartup(StartupPhase::Ready, startup);
        std::filesystem::remove(path);
        const auto lights = [&rig] {
            const auto* pool = rig.scene.registry().findPool<LightComponent>();
            return pool == nullptr ? size_t{ 0 } : pool->entities.size();
        };
        rig.runMeasured(2u, 1u);   // warmup 0-1, measured 0
        CHECK(rig.backend.calls.empty());
        rig.runFrame(3u, 1u);
        // One texture, then three materials, all at PreSceneUpdate.
        CHECK(rig.backend.count(Kind::AllocateTexture) == 1u);
        CHECK(rig.backend.count(Kind::AllocateMaterial) == 3u);
        for (const BackendCall& call : rig.backend.calls) CHECK(call.marker == "pre");
        rig.runFrame(4u, 2u);
        CHECK(lights() == 0u);
        rig.runFrame(5u, 3u);
        CHECK(lights() == 5u);
        rig.runFrame(6u, 4u);
        CHECK(lights() == 3u);
        rig.runFrame(7u, 5u);
        CHECK(rig.backend.count(Kind::AllocateMaterial) == 3u);

        const AppRunSnapshot run{};
        AppShutdownContext shutdown{
            .config = rig.config,
            .profiler = rig.profiler,
            .backend = &rig.backend,
            .completed = true,
            .run = run,
        };
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        CHECK(rig.backend.count(Kind::FreeMaterial) == 3u);
        CHECK(rig.backend.count(Kind::FreeTexture) == 1u);
        return true;
    }

    bool testScriptedChangesRejectUnmetPrerequisites() {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            "iridium-scripted-changes-prerequisites.json";
        {
            std::ofstream file(path, std::ios::binary);
            file << R"({"schema":"iridium.qualification.scripted_changes.v1",)"
                R"("id":"t","events":[{"frame":2,"action":"remove_capture_probe"}]})";
        }
        const auto readyFails = [&path](uint64_t frameLimit) {
            HarnessRig rig;
            rig.options.scriptedChanges = path;
            rig.options.cpuProfileOutput = "unused.jsonl";
            rig.config.frameLimit = frameLimit;
            rig.profiler.setEnabled(true);
            rig.start();
            AppStartupContext startup = rig.startupContext();
            try {
                rig.harness->onStartup(StartupPhase::Configure, startup);
                rig.harness->onStartup(StartupPhase::Ready, startup);
            }
            catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        // Beyond the frame limit, and (within it) without probe entities.
        const bool beyondLimit = readyFails(2u);
        const bool withoutProbes = readyFails(10u);
        std::filesystem::remove(path);
        CHECK(beyondLimit);
        CHECK(withoutProbes);
        return true;
    }

    bool testInertWithoutQualificationFlags() {
        HarnessRig rig;
        rig.start();
        AppStartupContext startup = rig.startupContext();
        rig.harness->onStartup(StartupPhase::BackendReady, startup);
        rig.harness->onStartup(StartupPhase::Ready, startup);
        rig.runMeasured(1u, 4u);
        CHECK(rig.backend.calls.empty());
        CHECK(rig.control.resizes.empty());
        CHECK(rig.control.transports.empty());
        CHECK(!rig.requests.suppressGridOverlay);
        CHECK(!rig.requests.viewHistoryResetRevision);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Run policy", testRunPolicy },
        { "Backend configured at Configure", testBackendConfiguredAtConfigure },
        { "Capture points", testCapturePoints },
        { "Readback validations arm inside the open frame",
            testReadbackValidationsArmInsideTheOpenFrame },
        { "Requests lost with an unopened frame",
            testRequestsLostWithAnUnopenedFrame },
        { "Resize sequence runs between frames",
            testResizeSequenceRunsBetweenFrames },
        { "Output transport switch sequence", testOutputTransportSwitchSequence },
        { "Table-scale probes are harness-owned",
            testTableScaleProbesAreHarnessOwned },
        { "Scripted changes apply at their measured frame",
            testScriptedChangesApplyAtTheirMeasuredFrame },
        { "Scripted changes reject unmet prerequisites",
            testScriptedChangesRejectUnmetPrerequisites },
        { "Inert without qualification flags", testInertWithoutQualificationFlags },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
