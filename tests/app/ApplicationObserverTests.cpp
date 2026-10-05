// M7R R2.6: the Application's IFrameObserver contract on a real device. A
// recording observer that owns an empty deterministic startup checks the exact
// startup/frame/shutdown hook order, the startup context hand-off, IAppControl
// scene resizes between frames, and the failure-path shutdown. Links iridium_app
// only: the Application works without the qualification library.

#include "app/Application.h"
#include "app/FrameObserver.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
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

    const char* name(StartupPhase phase) {
        switch (phase) {
        case StartupPhase::Configure: return "Configure";
        case StartupPhase::BackendReady: return "BackendReady";
        case StartupPhase::ContentLoad: return "ContentLoad";
        case StartupPhase::ScenePrerequisites: return "ScenePrerequisites";
        case StartupPhase::TopologyReady: return "TopologyReady";
        case StartupPhase::SceneConstruction: return "SceneConstruction";
        case StartupPhase::SceneComplete: return "SceneComplete";
        case StartupPhase::Ready: return "Ready";
        }
        return "?";
    }

    class RecordingObserver final : public IFrameObserver {
    public:
        std::vector<std::string> events;
        std::optional<uint64_t> throwAtMeasuredFrame;
        bool startupContextValid = true;
        bool resized = false;
        RenderExtent extentAfterResize{};
        RenderExtent extentNextFrame{};
        bool releaseCompleted = true;

        AppRunPolicy runPolicy() const override {
            return {
                .deterministicContent = true,
                .fullscreenScenePresentation = true,
                .colorValidationOverlay = false,
                .ownsStartupContent = true,
            };
        }

        void onStartup(StartupPhase phase, AppStartupContext& context) override {
            events.emplace_back(name(phase));
            const bool backendExpected = phase != StartupPhase::Configure;
            if ((context.backend != nullptr) != backendExpected ||
                (context.assets != nullptr) != backendExpected) {
                startupContextValid = false;
            }
            // The observer owns startup content and loads none, so the
            // Application creates no startup entities.
            if (phase == StartupPhase::SceneConstruction &&
                (context.mainModel || context.firstEntity != NULL_ENTITY)) {
                startupContextValid = false;
            }
        }

        void onFrameBegin(FrameBeginPhase phase, AppFrameContext& context) override {
            switch (phase) {
            case FrameBeginPhase::PreSceneUpdate:
                events.push_back("Pre");
                if (context.requests.viewHistoryResetRevision ||
                    context.requests.suppressGridOverlay) {
                    events.push_back("requests-not-reset");
                }
                context.requests.suppressGridOverlay = true;
                break;
            case FrameBeginPhase::PostSceneUpdate:
                events.push_back(context.measuredFrameIndex
                    ? "Post" + std::to_string(*context.measuredFrameIndex)
                    : std::string("Post-warmup"));
                if (context.measuredFrameIndex == 1u) {
                    extentNextFrame = context.control.renderExtent();
                }
                if (throwAtMeasuredFrame &&
                    context.measuredFrameIndex == throwAtMeasuredFrame) {
                    throw std::runtime_error("observer failure");
                }
                if (context.measuredFrameIndex == 0u) {
                    std::string diagnostic;
                    resized = context.control.resizeSceneExtent(
                        { 640u, 360u }, diagnostic);
                    extentAfterResize = context.control.renderExtent();
                }
                break;
            case FrameBeginPhase::BackendFrameOpened:
                events.push_back("Opened");
                break;
            }
        }

        void onFrameSubmit(FrameSubmitPoint point, AppFrameContext&) override {
            events.push_back(point == FrameSubmitPoint::SceneLinearReady
                ? "SceneLinear" : "Output");
        }

        void onFrameEnd(AppFrameContext&) override { events.push_back("End"); }

        void onShutdown(ShutdownPhase phase, AppShutdownContext& context) override {
            switch (phase) {
            case ShutdownPhase::RunComplete:
                events.push_back("RunComplete");
                if (context.backend == nullptr ||
                    context.run.measuredFrameCount != 3u ||
                    !context.run.measurementStarted) {
                    events.push_back("bad-run-snapshot");
                }
                break;
            case ShutdownPhase::ReleaseResources:
                events.push_back("ReleaseResources");
                releaseCompleted = context.completed;
                break;
            case ShutdownPhase::Finalize:
                events.push_back("Finalize");
                if (context.backend != nullptr) events.push_back("backend-alive");
                break;
            }
        }
    };

    ApplicationConfig smallRun() {
        ApplicationConfig config{};
        config.windowVisible = false;
        config.windowDecorated = false;
        config.windowWidth = 1280u;
        config.windowHeight = 720u;
        config.warmupFrameCount = 1u;
        config.frameLimit = 3u;
        return config;
    }

    bool testHookOrder() {
        RecordingObserver observer;
        {
            Application app(smallRun(), &observer);
            app.run();
        }
        CHECK(observer.startupContextValid);
        CHECK(observer.resized);
        CHECK(observer.extentAfterResize.width == 640u);
        CHECK(observer.extentAfterResize.height == 360u);
        CHECK(observer.extentNextFrame.width == 640u);
        CHECK(observer.releaseCompleted);

        std::vector<std::string> expected{
            "Configure", "BackendReady", "ContentLoad", "ScenePrerequisites",
            "TopologyReady", "SceneConstruction", "SceneComplete", "Ready",
        };
        const std::vector<std::string> frameTail{
            "Opened", "SceneLinear", "Output", "End" };
        for (const char* post : { "Post-warmup", "Post0", "Post1", "Post2" }) {
            expected.push_back("Pre");
            expected.push_back(post);
            expected.insert(expected.end(), frameTail.begin(), frameTail.end());
        }
        expected.push_back("RunComplete");
        expected.push_back("ReleaseResources");
        expected.push_back("Finalize");
        if (observer.events != expected) {
            std::cerr << "  recorded:";
            for (const std::string& event : observer.events)
                std::cerr << ' ' << event;
            std::cerr << '\n';
            return false;
        }
        return true;
    }

    bool testFailurePathReleasesWithoutFinalize() {
        RecordingObserver observer;
        observer.throwAtMeasuredFrame = 1u;
        bool threw = false;
        try {
            Application app(smallRun(), &observer);
            app.run();
        }
        catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "observer failure";
        }
        CHECK(threw);
        CHECK(!observer.releaseCompleted);
        CHECK(!observer.events.empty());
        CHECK(observer.events.back() == "ReleaseResources");
        for (const std::string& event : observer.events) {
            CHECK(event != "RunComplete");
            CHECK(event != "Finalize");
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Observer hook order", testHookOrder },
        { "Failure path releases without finalize",
            testFailurePathReleasesWithoutFinalize },
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
