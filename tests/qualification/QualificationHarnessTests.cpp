// M7R R2.6: the qualification harness against a device-free backend. Covers the
// run policy, which hook issues which capture/readback request, requests lost to a
// frame that never opened, validator sequences, and probe-resource ownership.

#include "qualification/harness/QualificationHarness.h"
#include "FakeRenderBackend.h"

#include "profiling/CpuProfiler.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderFrame.h"
#include "scene/SceneWorld.h"
#include "scene/components/LightComponent.h"

#include <nlohmann/json.hpp>

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
        // The frame handed to onFrameSubmit (its view carries the jitter).
        RenderFrame renderFrame{};
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
            frame.renderFrame = &renderFrame;
            backend.marker = "scene-linear";
            harness->onFrameSubmit(FrameSubmitPoint::SceneLinearReady, frame);
            backend.marker = "output";
            harness->onFrameSubmit(FrameSubmitPoint::OutputReady, frame);
            frame.renderFrame = nullptr;
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

    // M9 G6c --capture-frames parsing and the selection/hold helpers.
    bool testCaptureFrameRangeAndHoldHelpers() {
        const CaptureFrameRange range = parseCaptureFrameRange("3:11:4");
        CHECK(range.first == 3u && range.last == 11u && range.step == 4u);
        CHECK(range.count() == 3u);
        CHECK(range.frame(0) == 3u && range.frame(1) == 7u && range.frame(2) == 11u);
        CHECK(range.contains(7u) && !range.contains(8u) && !range.contains(2u) &&
            !range.contains(15u));
        const CaptureFrameRange single = parseCaptureFrameRange("5:5");
        CHECK(single.step == 1u && single.count() == 1u && single.contains(5u));
        // The last frame need not be on the step.
        CHECK(parseCaptureFrameRange("0:10:3").count() == 4u);
        for (const char* bad : { "", "5", ":5", "5:", "5:4", "1:2:0", "1:2:3:4",
                 "+1:2", "1 :2", "x:y", "1:2:", "99999999999999999999:1" }) {
            bool rejected = false;
            try {
                (void)parseCaptureFrameRange(bad);
            }
            catch (const std::invalid_argument&) {
                rejected = true;
            }
            CHECK(rejected);
        }

        QualificationOptions options{};
        CHECK(!capturesFrames(options) && !captureSelectsFrame(options, 0u));
        options.captureFrameRange = CaptureFrameRange{ 2, 6, 2 };
        CHECK(capturesFrames(options));
        CHECK(captureSelectsFrame(options, 4u) && !captureSelectsFrame(options, 5u) &&
            !captureSelectsFrame(options, 8u));
        options.captureFrameRange.reset();
        options.captureFrameIndex = 3u;
        CHECK(captureSelectsFrame(options, 3u) && !captureSelectsFrame(options, 4u));

        CHECK(benchmarkStateFrameIndex(options, 500u) == 500u);
        options.benchmarkHoldFrame = 10u;
        CHECK(benchmarkStateFrameIndex(options, 3u) == 3u);
        CHECK(benchmarkStateFrameIndex(options, 10u) == 10u);
        CHECK(benchmarkStateFrameIndex(options, 500u) == 10u);
        return true;
    }

    size_t countFiles(const std::filesystem::path& directory, std::string_view suffix) {
        size_t total = 0;
        if (!std::filesystem::exists(directory)) return total;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().ends_with(suffix)) ++total;
        }
        return total;
    }

    // M9 G6c: a sequence arms one capture per selected measured frame,
    // writes each image as soon as its readback completes (never holding
    // more than the frames in flight), and commits one sidecar per frame,
    // in order, with that frame's own jitter.
    bool testCaptureSequenceStreamsArtifacts() {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "iridium-capture-sequence-test";
        std::filesystem::remove_all(directory);
        HarnessRig rig;
        rig.options.captureFrameRange = CaptureFrameRange{ 1, 5, 2 };
        rig.options.captureDirectory = directory;
        rig.start();
        rig.qualification->produceCaptures = true;
        rig.qualification->captureLatency = 3u;
        // Warmup 1: measured m is application frame m + 1; captures at
        // application frames 2, 4 and 6. Exactly representable jitter:
        // NDC k/2048 over a 1024-wide view is k/4 pixels.
        for (uint64_t frame = 0; frame < 7u; ++frame) {
            ViewTransportRecord& view = rig.renderFrame.view;
            view.renderInfo = glm::uvec4(1024u, 512u, 0u, 0u);
            view.jitter = glm::vec4(static_cast<float>(frame) / 2048.0f,
                -1.0f / 1024.0f, 0.0f, 0.0f);
            view.temporalInfo = glm::uvec4(static_cast<uint32_t>(frame % 8u),
                static_cast<uint32_t>(frame),
                ViewTemporalJitterActive |
                    (frame == 4u ? ViewTemporalHistoryReset : 0u), 0u);
            rig.runFrame(frame, frame >= 1u
                ? std::optional<uint64_t>(frame - 1u) : std::nullopt);
            // Measured frame 1 (armed at frame 2) completes at the third
            // collection, frame 5's PreSceneUpdate: written, uncommitted.
            if (frame == 4u) CHECK(countFiles(directory, ".pfm.tmp") == 0u);
            if (frame == 5u) {
                CHECK(countFiles(directory, ".pfm.tmp") == 1u);
                CHECK(countFiles(directory, ".json") == 0u);
            }
        }
        CHECK(rig.backend.count(Kind::ArmFrameCapture) == 3u);
        for (const BackendCall& call : rig.backend.calls) {
            CHECK(call.kind == Kind::ArmFrameCapture && call.marker == "opened");
        }
        CHECK(rig.qualification->maximumHeldCaptures == 2u);

        const AppRunSnapshot run{};
        AppShutdownContext shutdown{
            .config = rig.config,
            .profiler = rig.profiler,
            .backend = &rig.backend,
            .completed = true,
            .run = run,
        };
        // The waiting drain returns the last two in slot (reverse) order.
        rig.harness->onShutdown(ShutdownPhase::RunComplete, shutdown);
        CHECK(countFiles(directory, ".pfm.tmp") == 3u);
        CHECK(countFiles(directory, ".json") == 0u);
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        shutdown.backend = nullptr;
        rig.harness->onShutdown(ShutdownPhase::Finalize, shutdown);
        CHECK(countFiles(directory, ".tmp") == 0u);
        CHECK(countFiles(directory, ".pfm") == 3u);
        CHECK(countFiles(directory, ".json") == 3u);

        for (const uint64_t measured : { 1u, 3u, 5u }) {
            const uint64_t application = measured + 1u;
            const std::filesystem::path sidecar = directory /
                ("interactive__r0__interactive_camera__final__2x2__mf" +
                    std::to_string(measured) + ".json");
            std::ifstream file(sidecar, std::ios::binary);
            CHECK(file.good());
            const nlohmann::json document = nlohmann::json::parse(file);
            CHECK(document.at("capture").at("capture_id") == measured);
            const nlohmann::json& runInfo = document.at("run");
            CHECK(runInfo.at("measured_frame_index") == measured);
            CHECK(runInfo.at("application_frame_index") == application);
            CHECK(runInfo.at("benchmark_state_frame_index") == application);
            const nlohmann::json& jitter =
                document.at("render_configuration").at("temporal_jitter");
            CHECK(jitter.at("enabled") == true);
            CHECK(jitter.at("sequence_length") == 8u);
            CHECK(jitter.at("sequence_index") == application % 8u);
            CHECK(jitter.at("turns_since_cut") == application);
            CHECK(jitter.at("history_reset") == (application == 4u));
            CHECK(jitter.at("offset_ndc")[0].get<double>() ==
                static_cast<double>(application) / 2048.0);
            CHECK(jitter.at("offset_ndc")[1].get<double>() == -1.0 / 1024.0);
            CHECK(jitter.at("offset_pixels")[0].get<double>() ==
                static_cast<double>(application) / 4.0);
            CHECK(jitter.at("offset_pixels")[1].get<double>() == -0.25);
        }
        std::filesystem::remove_all(directory);
        return true;
    }

    // A failed run commits no sidecar and leaves no streamed image behind.
    bool testCaptureSequenceDiscardedOnFailure() {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "iridium-capture-sequence-failure";
        std::filesystem::remove_all(directory);
        HarnessRig rig;
        rig.options.captureFrameRange = CaptureFrameRange{ 0, 9 };
        rig.options.captureDirectory = directory;
        rig.start();
        rig.qualification->produceCaptures = true;
        rig.qualification->captureLatency = 1u;
        rig.runMeasured(0u, 4u);
        CHECK(countFiles(directory, ".pfm.tmp") == 3u);
        const AppRunSnapshot run{};
        AppShutdownContext shutdown{
            .config = rig.config,
            .profiler = rig.profiler,
            .backend = &rig.backend,
            .completed = false,
            .run = run,
        };
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        CHECK(std::filesystem::is_empty(directory));
        std::filesystem::remove_all(directory);
        return true;
    }

    // An incomplete sequence fails the end-of-run report.
    bool testCaptureSequenceRequiresEveryFrame() {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "iridium-capture-sequence-short";
        std::filesystem::remove_all(directory);
        HarnessRig rig;
        rig.options.captureFrameRange = CaptureFrameRange{ 0, 5 };
        rig.options.captureDirectory = directory;
        rig.start();
        rig.qualification->produceCaptures = true;
        rig.runMeasured(0u, 3u);   // measured frames 3..5 never ran
        const AppRunSnapshot run{};
        AppShutdownContext shutdown{
            .config = rig.config,
            .profiler = rig.profiler,
            .backend = &rig.backend,
            .completed = true,
            .run = run,
        };
        bool rejected = false;
        try {
            rig.harness->onShutdown(ShutdownPhase::RunComplete, shutdown);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        shutdown.completed = false;
        rig.harness->onShutdown(ShutdownPhase::ReleaseResources, shutdown);
        CHECK(std::filesystem::is_empty(directory));
        std::filesystem::remove_all(directory);
        return true;
    }

    // M9 G6c --benchmark-hold-frame on a real fixture (TF-pan: a camera
    // path with cuts at 90 and 180): camera pose and history-reset revision
    // are frame F's from F on; without the hold they keep changing.
    bool testBenchmarkHoldFrame() {
        struct Sample {
            glm::vec3 position{ 0.0f };
            glm::vec3 front{ 0.0f };
            uint64_t revision = 0;
        };
        const auto run = [](std::optional<uint64_t> hold, std::vector<Sample>& samples) {
            HarnessRig rig;
            rig.options.benchmarkId = "m9_tf_pan_v1";
            rig.options.benchmarkManifest = std::filesystem::path(PROJECT_ROOT_DIR) /
                "assets" / "benchmarks" / "m9" / "temporal-manifest.v1.json";
            rig.options.benchmarkHoldFrame = hold;
            // Startup content goes through the fake control.
            rig.config.cookedModelArtifact = "unused.irartifact";
            rig.config.cookedEnvironmentArtifact = "unused.irartifact";
            rig.start();
            AppStartupContext startup = rig.startupContext();
            rig.harness->onStartup(StartupPhase::ContentLoad, startup);
            for (uint64_t frame = 0; frame < 200u; ++frame) {
                rig.runFrame(frame, std::nullopt);
                samples.push_back({ rig.camera.position, rig.camera.front,
                    rig.requests.viewHistoryResetRevision.value_or(0u) });
            }
        };
        std::vector<Sample> free;
        std::vector<Sample> held;
        run(std::nullopt, free);
        run(95u, held);
        CHECK(free.size() == 200u && held.size() == 200u);
        const auto same = [](const Sample& a, const Sample& b) {
            return a.position == b.position && a.front == b.front &&
                a.revision == b.revision;
        };
        for (uint64_t frame = 0; frame <= 95u; ++frame) {
            CHECK(same(free[frame], held[frame]));
        }
        for (uint64_t frame = 96u; frame < 200u; ++frame) {
            CHECK(same(held[frame], held[95]));
        }
        // Unheld, the camera moves after 95 and the cut at 180 advances the
        // revision.
        CHECK(!same(free[120], free[95]));
        CHECK(free[185].revision != free[95].revision);
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
        { "Capture frame range and hold helpers", testCaptureFrameRangeAndHoldHelpers },
        { "Capture sequence streams artifacts", testCaptureSequenceStreamsArtifacts },
        { "Capture sequence discarded on failure", testCaptureSequenceDiscardedOnFailure },
        { "Capture sequence requires every frame", testCaptureSequenceRequiresEveryFrame },
        { "Benchmark hold frame", testBenchmarkHoldFrame },
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
