// M7R R5a.4: code moved from Application.cpp (design section 3.4); see
// FrameOrchestrator.h.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "app/FrameOrchestrator.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/AssetIntegration.h"
#include "core/EngineLog.h"
#include "editor/EditorHost.h"
#include "extraction/RenderExtractor.h"
#include "profiling/CpuAllocationProfile.h"
#include "profiling/CpuProfiler.h"
#include "renderer/color/AcesOutputLut.h"

namespace Iridium {

    FrameOrchestrator::FrameOrchestrator(const FrameOrchestratorContext& context)
        : config_(context.config),
          cpuProfiler_(context.profiler),
          engineLog_(context.log),
          observer_(context.observer),
          policy_(context.policy),
          sceneWorld_(context.scene),
          registry(context.scene.registry()),
          camera_(context.camera),
          control_(context.control),
          extractor_(context.extractor),
          assets_(context.assets),
          editorHost_(context.editor) {}

    void FrameOrchestrator::installWindowCallbacks(GLFWwindow* window) {
        this->window = window;
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    void FrameOrchestrator::adoptBackend(IRenderBackend& backend) {
        renderBackend = &backend;
        renderExtent_ = renderBackend->getRenderExtent();
    }

    void FrameOrchestrator::refreshOutputTransportStatus() {
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        publishOutputTransportStatus();
    }

    void FrameOrchestrator::initializeOutputTransformLut() {
        replaceOutputTransformLut(
            renderRuntimeInfo_.effectiveOutputTransportMode);
    }

    void FrameOrchestrator::releaseOutputTransformLut() {
        if (renderBackend && outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
            outputTransformLut = {};
        }
    }


    void FrameOrchestrator::run() {
        float lastFrameTime = 0.0f;
        uint64_t applicationFrameCount = 0;
        std::chrono::steady_clock::time_point measurementStart{};
        bool measurementStarted = false;

        // --- Added for FPS Tracking ---
        int frameCount = 0;
        float timeAccumulator = 0.0f;

        AppFrameContext frame{
            .config = config_,
            .backend = *renderBackend,
            .profiler = cpuProfiler_,
            .scene = sceneWorld_,
            .camera = camera_,
            .control = control_,
            .requests = frameRequests_,
            .mainModel = assets_.mainModel(),
        };

        while (!glfwWindowShouldClose(window)) {
            const bool isMeasuredFrame = applicationFrameCount >= config_.warmupFrameCount;
            if (isMeasuredFrame && !measurementStarted) {
                measurementStart = std::chrono::steady_clock::now();
                measurementStarted = true;
                measurementStarted_ = true;
            }
            const bool profileFrame = isMeasuredFrame &&
                cpuProfiler_.beginFrame(applicationFrameCount + 1);
            if (profileFrame) {
                beginCpuAllocationFrame();
            }
            {
                CpuScope frameScope(cpuProfiler_, "cpu.frame.total");
                {
                    CpuScope eventScope(cpuProfiler_, "cpu.platform.events");
                    glfwPollEvents();
                }

                // 1. Time & Input
                float currentFrameTime = static_cast<float>(glfwGetTime());
                deltaTime = currentFrameTime - lastFrameTime;
                lastFrameTime = currentFrameTime;

                // --- FPS CALCULATION ---
                frameCount++;
                timeAccumulator += deltaTime;

                // Update the window title once every second
                if (timeAccumulator >= 1.0f) {
                    char title[96]{};
                    const float millisecondsPerFrame = frameCount != 0
                        ? 1000.0f / static_cast<float>(frameCount)
                        : 0.0f;
                    std::snprintf(title, sizeof(title),
                        "Iridium Engine - FPS: %u (%.2f ms/frame)",
                        frameCount, millisecondsPerFrame);
                    glfwSetWindowTitle(window, title);

                    frameCount = 0;
                    timeAccumulator -= 1.0f;
                }

                if (!policy_.deterministicContent) {
                    processInput(window);
                }

                frameRequests_ = {};
                frame.applicationFrameIndex = applicationFrameCount;
                frame.measuredFrameIndex = isMeasuredFrame
                    ? std::optional<uint64_t>(measuredFrameCount_)
                    : std::nullopt;
                frame.outputTransportPending = false;
                if (observer_) {
                    observer_->onFrameBegin(
                        FrameBeginPhase::PreSceneUpdate, frame);
                }

                // 2. Process delayed ECS events (like swapping meshes on the main thread)
                assets_.processMeshSwaps();

                assets_.tickRuntime();

                // 3. Update ECS Systems (Physics, Transforms, Animations)
                // This recalculates all local/world matrices before we extract them.
                {
                    CpuScope transformScope(cpuProfiler_, "cpu.scene.transforms");
                    changedTransformsThisFrame_ = transformSystem.update(
                        registry, &changedTransformEntities_);
                }

                // Observer scene-extent resizes happen here, between frames.
                if (observer_) {
                    observer_->onFrameBegin(
                        FrameBeginPhase::PostSceneUpdate, frame);
                }

                // 4. The frame acquisition must precede UI construction so the
                // viewport texture IDs correspond to the image acquired this frame.
                drawFrame(frame);
            }
            if (profileFrame) {
                const CpuAllocationFrameSample allocationSample =
                    endCpuAllocationFrame();
                cpuProfiler_.recordCounter("allocation.cpp.calls",
                    allocationSample.allocationCount);
                cpuProfiler_.recordCounter("allocation.cpp.bytes",
                    allocationSample.requestedBytes);
                (void)cpuProfiler_.endFrame();
            }

            ++applicationFrameCount;
            if (isMeasuredFrame) {
                ++measuredFrameCount_;
            }
            if (config_.frameLimit != 0 && measuredFrameCount_ >= config_.frameLimit) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }

        if (measurementStarted) {
            measurementWallNanoseconds_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - measurementStart).count());
        }
    }

    void FrameOrchestrator::onRenderFrameStage(RenderFrameStage stage) {
        switch (stage) {
        case RenderFrameStage::DirectionalShadows:
        case RenderFrameStage::SpotShadows:
        case RenderFrameStage::PointShadows:
        case RenderFrameStage::ReflectionProbeCaptures:
        case RenderFrameStage::Lighting:
            extractor_.onRenderFrameStage(stage);
            break;
        case RenderFrameStage::SceneLinearComplete:
            // Scene-linear captures read the lit scene here.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::SceneLinearReady,
                    *stageFrame_);
            break;
        case RenderFrameStage::OutputComplete:
            // Final-output captures read the output target before the UI pass.
            if (observer_)
                observer_->onFrameSubmit(FrameSubmitPoint::OutputReady,
                    *stageFrame_);
            break;
        }
    }

    void FrameOrchestrator::applyEditorFrameRequests(
        const EditorFrameRequests& requests) {
        if (requests.output) {
            const EditorOutputSettings& outputSettings = *requests.output;
            if (outputSettings.transport != config_.outputTransport) {
                config_.outputTransport = outputSettings.transport;
                pendingOutputTransport_ = outputSettings.transport;
            }
            config_.manualExposureEv = outputSettings.manualExposureEv;
            config_.paperWhiteNits = outputSettings.paperWhiteNits;
            config_.peakNits = outputSettings.peakNits;
        }
        if (requests.shadows) {
            ProjectShadowSettings shadowSettings = *requests.shadows;
            // Resolution is immutable for the active backend allocation;
            // every remaining project policy applies on the next frame.
            shadowSettings.directionalResolution =
                config_.shadowSettings.directionalResolution;
            shadowSettings.spotAtlasResolution =
                config_.shadowSettings.spotAtlasResolution;
            shadowSettings.pointPool256Capacity =
                config_.shadowSettings.pointPool256Capacity;
            shadowSettings.pointPool512Capacity =
                config_.shadowSettings.pointPool512Capacity;
            shadowSettings.pointPool1024Capacity =
                config_.shadowSettings.pointPool1024Capacity;
            config_.shadowSettings = shadowSettings;
        }
        if (requests.probes) {
            const ProjectReflectionProbeSettings& probeSettings =
                *requests.probes;
            config_.reflectionProbeSettings = probeSettings;
            extractor_.configureProbeCaptures(probeSettings);
            renderBackend->configureReflectionProbeCaptures(
                probeSettings);
        }
    }

    void FrameOrchestrator::drawFrame(AppFrameContext& frame) {
        const uint64_t applicationFrameIndex = frame.applicationFrameIndex;
        const EditorViewSelection viewSelection =
            editorHost_.chooseView(policy_.fullscreenScenePresentation);
        const bool dualViews = viewSelection.dualViews;
        const uint32_t renderView = viewSelection.renderView;
        {
        CpuScope finalizeScope(cpuProfiler_, "cpu.probe.capture.finalize");
        for (const ReflectionProbeCaptureCompletion& completion :
                renderBackend->finalizeReflectionProbeCaptures()) {
            extractor_.markCapturePublished(completion);
            assets_.publishCaptureCompletion(completion);
        }
        }
        const std::map<AssetGuid, LoadedEnvironmentAsset>& loadedEnvironments =
            assets_.loadedEnvironments();
        // Light and probe extraction run before beginFrame; their packets stay
        // valid until the frame is released after submitFrame.
        SceneWorld* const previewLightingWorld =
            editorHost_.previewLightingWorld();
        extractor_.prepareLightsAndProbes(
            previewLightingWorld ? *previewLightingWorld : sceneWorld_,
            loadedEnvironments);
        assets_.processMaterialPreviews(editorHost_.assetDocuments());
        extractor_.prepareGpuScenePublication(editorHost_.selectedEntity());
        const GpuScenePackedTables* const gpuSceneFrame =
            extractor_.gpuSceneFrame();
        // Descriptor publication waits for old users and must precede acquisition.
        // Keep the authored scene environment identity separate from this binding.
        const EnvironmentLightingHandles desiredEnvironment =
            editorHost_.selectViewEnvironment(assets_.environmentLighting(),
                applicationFrameIndex);
        if (desiredEnvironment.isValid() && desiredEnvironment != renderBackend->getEnvironmentLighting())
            renderBackend->setEnvironmentLighting(desiredEnvironment);
        editorHost_.prepareRetainedViews(dualViews, renderView);
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            extractor_.releaseFrame();
            recreateSwapchain();
            return;
        }
        if (gpuSceneFrame) renderBackend->publishGpuScene(*gpuSceneFrame);
        const GpuSceneUploadTelemetry gpuSceneUpload =
            renderBackend->frameTelemetry().gpuSceneUpload;
        cpuProfiler_.recordCounter("gpu_scene.upload.bytes",
            gpuSceneUpload.bytes);
        cpuProfiler_.recordCounter("gpu_scene.upload.ranges",
            gpuSceneUpload.ranges);
        if (observer_) {
            observer_->onFrameBegin(FrameBeginPhase::BackendFrameOpened, frame);
        }

        // --- 1. CLEAR THE QUEUES ---
        extractor_.clearQueues();

        const float aspect = renderExtent_.height != 0
            ? static_cast<float>(renderExtent_.width) /
                static_cast<float>(renderExtent_.height)
            : 16.0f / 9.0f;
        // The previous-frame editor state that extraction reads before the
        // editor is built (design section 3.2, step 4).
        const EditorViewState preBuildView =
            editorHost_.viewState(aspect, renderExtent_, measuredFrameCount_);
        // --- 2. GET CAMERA DATA ---
        extractor_.beginView(preBuildView, {
            .position = camera_.position,
            .front = camera_.front,
            .up = camera_.up,
            .verticalFovDegrees = camera_.verticalFovDegrees,
            .nearPlane = camera_.nearPlane,
            .farPlane = camera_.farPlane,
        }, aspect);

        EditorFrameRequests editorRequests = editorHost_.build({
            .registry = &registry,
            .cameraPosition = camera_.position,
            .cameraFront = camera_.front,
            .cameraUp = camera_.up,
            .verticalFovDegrees = camera_.verticalFovDegrees,
            .nearPlane = camera_.nearPlane,
            .farPlane = camera_.farPlane,
            .aspect = aspect,
            .renderExtent = renderExtent_,
            .measuredFrameCount = measuredFrameCount_,
            .fullscreenScenePresentation = policy_.fullscreenScenePresentation,
            .colorValidationOverlay = policy_.colorValidationOverlay,
        });
        applyEditorFrameRequests(editorRequests);
        // The editor state after this frame's build: what extraction reads.
        const EditorViewState& view = editorRequests.view;
        extractor_.finalizeView(view, {
            .renderExtent = renderExtent_,
            .sceneEnvironmentSettings = assets_.sceneEnvironmentSettings(),
            .manualExposureEv = config_.manualExposureEv,
            .paperWhiteNits = config_.paperWhiteNits,
            .peakNits = config_.peakNits,
            .viewHistoryResetRevision = frameRequests_.viewHistoryResetRevision,
        });
        if (!frameRequests_.suppressGridOverlay && !view.assetPreviewActive) {
            extractor_.setGridOverlay(editorHost_.viewportGridOverlay(
                extractor_.viewMatrix(), extractor_.projectionMatrix()));
        }

        // --- 3-5. EXTRACTION, SORTING AND SCHEDULES ---
        const RenderFrame& renderFrame = extractor_.extract({
            .forceWireframe = config_.forceWireframe,
            .changedTransforms = changedTransformsThisFrame_,
            .environmentCookKey = &assets_.activeEnvironmentCookKey(),
            .applicationFrameIndex = applicationFrameIndex,
            .stageObserver = this,
        });
        stageFrame_ = &frame;


        // Shadows, probe captures, G-buffer, lighting, forward and
        // transparency, output and UI. The observer's scene-linear and
        // output submit points are reported from the stage boundaries.
        renderBackend->submitFrame(renderFrame);
        stageFrame_ = nullptr;
        extractor_.releaseFrame();

        if (renderBackend->endFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
            return;
        }
        if (dualViews) editorHost_.viewRendered(renderView);
        if (observer_) {
            const uint64_t switchesBefore = outputTransportSwitchCount_;
            frame.outputTransportPending = pendingOutputTransport_.has_value();
            observer_->onFrameEnd(frame);
            if (outputTransportSwitchCount_ != switchesBefore) return;
        }
        if (pendingOutputTransport_) {
            const Color::OutputTransport requested = *pendingOutputTransport_;
            pendingOutputTransport_.reset();
            (void)switchOutputTransport(requested);
            return;
        }
        if (!policy_.fullscreenScenePresentation && config_.windowVisible) {
            const RenderExtent requested =
                editorRequests.requestedSceneExtent;
            if (requested.width == 0 || requested.height == 0 ||
                (requested.width == renderExtent_.width &&
                    requested.height == renderExtent_.height)) {
                // The steady/minimized path performs no clock query, target
                // allocation, descriptor update, or graph rebuild.
                viewportExtentPolicy_.reset();
                return;
            }
            const uint64_t nowMilliseconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (const auto replacement = viewportExtentPolicy_.observe(
                    requested, renderExtent_, nowMilliseconds)) {
                cpuProfiler_.recordCounter("viewport.resize.requests", 1);
                std::string diagnostic;
                const auto resizeStart = std::chrono::steady_clock::now();
                const bool resized = renderBackend->resizeSceneRenderExtent(
                    *replacement, diagnostic);
                const uint64_t resizeNanoseconds = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - resizeStart).count());
                cpuProfiler_.recordCounter(
                    "viewport.resize.cpu_ns", resizeNanoseconds);
                if (resized) {
                    renderExtent_ = renderBackend->getRenderExtent();
                    viewportExtentDiagnostic_.clear();
                    cpuProfiler_.recordCounter("viewport.resize.successes", 1);
                    cpuProfiler_.recordCounter("viewport.target.pixels",
                        static_cast<uint64_t>(renderExtent_.width) *
                            renderExtent_.height);
                    engineLog_.info("renderer.viewport",
                        "Scene target resized to " +
                            std::to_string(renderExtent_.width) + "x" +
                            std::to_string(renderExtent_.height));
                }
                else {
                    viewportExtentPolicy_.reportFailure(nowMilliseconds);
                    viewportExtentDiagnostic_ = diagnostic.empty()
                        ? "Scene target resize failed; retaining the previous target"
                        : std::move(diagnostic);
                    cpuProfiler_.recordCounter("viewport.resize.failures", 1);
                    engineLog_.warning(
                        "renderer.viewport", viewportExtentDiagnostic_);
                }
            }
        }
    }

    // --- GLFW CALLBACK STUBS ---
    void FrameOrchestrator::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<FrameOrchestrator*>(glfwGetWindowUserPointer(window));
        if (app) app->framebufferResized = true;
    }

    void FrameOrchestrator::mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
        auto app = reinterpret_cast<FrameOrchestrator*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        float xpos = static_cast<float>(xposIn);
        float ypos = static_cast<float>(yposIn);

        if (app->firstMouse) {
            app->lastX = xpos;
            app->lastY = ypos;
            app->firstMouse = false;
        }

        float xoffset = xpos - app->lastX;
        float yoffset = app->lastY - ypos;
        app->lastX = xpos;
        app->lastY = ypos;

        // Asset documents own their orbit controls through ImGui and never move
        // the active scene camera while being inspected.
        if (!app->editorHost_.assetViewerFocused() &&
            (app->editorHost_.sceneViewportHovered() || glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            xoffset *= app->mouseSensitivity;
            yoffset *= app->mouseSensitivity;

            // INVERSION FIX: Swap += to -= if your X or Y still feels backward!
            app->yaw += xoffset;
            app->pitch += yoffset;

            // Clamp pitch to prevent flipping upside down
            if (app->pitch > 89.0f)  app->pitch = 89.0f;
            if (app->pitch < -89.0f) app->pitch = -89.0f;

            glm::vec3 front;
            front.x = cos(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            front.y = sin(glm::radians(app->pitch));
            front.z = sin(glm::radians(app->yaw)) * cos(glm::radians(app->pitch));
            app->camera_.front = glm::normalize(front);
        }
    }

    void FrameOrchestrator::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto app = reinterpret_cast<FrameOrchestrator*>(glfwGetWindowUserPointer(window));
        if (!app) return;
        if (app->editorHost_.assetViewerFocused() || !app->editorHost_.sceneViewportHovered()) return;

        // Use scroll wheel to change camera fly speed
        app->cameraSpeed += static_cast<float>(yoffset) * 0.5f;
        if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
        if (app->cameraSpeed > 20.0f) app->cameraSpeed = 20.0f;
    }

    void FrameOrchestrator::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto app = reinterpret_cast<FrameOrchestrator*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        // Only activate camera look on Right Click
        if (button == GLFW_MOUSE_BUTTON_RIGHT &&
            ((!app->editorHost_.assetViewerFocused() && app->editorHost_.sceneViewportHovered()) || action == GLFW_RELEASE)) {
            if (action == GLFW_PRESS) {
                app->firstMouse = true; // Prevent violent camera snapping
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); // Hide cursor
            }
            else if (action == GLFW_RELEASE) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // Show cursor
            }
        }
    }
    void FrameOrchestrator::processInput(GLFWwindow* window) {
        CpuScope inputScope(cpuProfiler_, "cpu.input");
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Only move camera if Right Mouse Button is held down (standard editor behavior)
        if (editorHost_.assetViewerFocused() && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (!editorHost_.assetViewerFocused() && glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            float velocity = cameraSpeed * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                camera_.position += camera_.front * velocity;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                camera_.position -= camera_.front * velocity;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                camera_.position -= glm::normalize(glm::cross(camera_.front, camera_.up)) * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                camera_.position += glm::normalize(glm::cross(camera_.front, camera_.up)) * velocity;
        }
    }

    void FrameOrchestrator::recreateSwapchain() {
        if (renderBackend) {
            renderBackend->recreateSwapchain(window);
            renderExtent_ = renderBackend->getRenderExtent();
            renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
            if (renderRuntimeInfo_.effectiveOutputTransportMode !=
                    outputTransformLutTransport_) {
                replaceOutputTransformLut(
                    renderRuntimeInfo_.effectiveOutputTransportMode);
            }
            publishOutputTransportStatus();
        }
    }

    void FrameOrchestrator::replaceOutputTransformLut(
        Color::OutputTransport effectiveTransport) {
        if (!renderBackend) return;
        if (effectiveTransport == Color::OutputTransport::Automatic) {
            throw std::logic_error(
                "The backend returned an unresolved automatic output transport.");
        }
        const bool hdrTransport = effectiveTransport !=
            Color::OutputTransport::SdrSrgb;
        const bool currentHdrTransport = outputTransformLutTransport_ !=
            Color::OutputTransport::SdrSrgb;
        // scRGB and HDR10 intentionally share the same P3-D65 1000-nit ACES
        // transform; only their post-LUT encoding/composition differs.
        if (outputTransformLut.isValid() &&
            currentHdrTransport == hdrTransport) {
            outputTransformLutTransport_ = effectiveTransport;
            return;
        }
        const Color::AcesOutputLut outputLut = Color::loadAcesOutputLut(
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "color" /
            (hdrTransport ? "aces2_p3d65_1000nit_rec2100_pq_128.irlt" :
                "aces2_rec709_100nit_srgb_128.irlt"));
        const TextureDesc outputLutDesc{
            .width = outputLut.width(),
            .height = outputLut.height(),
            .format = TextureFormat::RGBA32_SFloat,
            .usageClass = TextureUsageClass::Sampled2D,
            .sampler = {
                .minFilter = FilterMode::Nearest,
                .magFilter = FilterMode::Nearest,
                .addressU = SamplerAddressMode::ClampToEdge,
                .addressV = SamplerAddressMode::ClampToEdge,
                .addressW = SamplerAddressMode::ClampToEdge,
            },
        };
        const TextureHandle replacement = renderBackend->allocateTexture(
            outputLutDesc, std::as_bytes(std::span(outputLut.rgba32f)));
        renderBackend->setOutputTransformLut(replacement);
        if (outputTransformLut.isValid()) {
            renderBackend->freeTexture(outputTransformLut);
        }
        outputTransformLut = replacement;
        outputTransformLutTransport_ = effectiveTransport;
    }

    void FrameOrchestrator::publishOutputTransportStatus() {
        editorHost_.setOutputTransportStatus(
            renderRuntimeInfo_.requestedOutputTransportMode,
            renderRuntimeInfo_.effectiveOutputTransportMode,
            renderRuntimeInfo_.supportedOutputTransportModes,
            renderRuntimeInfo_.outputTransportDiagnostic);
    }

    OutputTransportSwitchResult FrameOrchestrator::switchOutputTransport(
        Color::OutputTransport requested) {
        const auto switchStart = std::chrono::steady_clock::now();
        renderBackend->setOutputTransport(window, requested);
        renderRuntimeInfo_ = renderBackend->getRuntimeInfo();
        replaceOutputTransformLut(
            renderRuntimeInfo_.effectiveOutputTransportMode);
        publishOutputTransportStatus();
        const uint64_t switchNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - switchStart).count());
        ++outputTransportSwitchCount_;
        std::cout << "IRIDIUM_OUTPUT_TRANSPORT_SWITCH {\"requested\":\""
            << renderRuntimeInfo_.requestedOutputTransport
            << "\",\"effective\":\""
            << renderRuntimeInfo_.effectiveOutputTransport
            << "\",\"diagnostic\":\""
            << renderRuntimeInfo_.outputTransportDiagnostic
            << "\",\"duration_ns\":" << switchNanoseconds
            << "}\n" << std::flush;
        return OutputTransportSwitchResult{
            .requested = renderRuntimeInfo_.requestedOutputTransport,
            .effective = renderRuntimeInfo_.effectiveOutputTransport,
            .diagnostic = renderRuntimeInfo_.outputTransportDiagnostic,
            .durationNanoseconds = switchNanoseconds,
        };
    }

    bool FrameOrchestrator::resizeSceneExtent(RenderExtent requested,
        std::string& diagnostic) {
        const bool resized = renderBackend->resizeSceneRenderExtent(
            requested, diagnostic);
        if (resized) renderExtent_ = renderBackend->getRenderExtent();
        return resized;
    }

} // namespace Iridium
