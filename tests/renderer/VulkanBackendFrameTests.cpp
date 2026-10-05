// M7R R3c.10/R3c.11: the production Vulkan backend on a device (hidden GLFW
// window, validation with synchronization validation), recording empty frames
// through submitFrame:
//   - without an editor bridge (headless hosts): the UI pass still clears and
//     presents;
//   - with the ImGui editor bridge: viewport, editor-texture and retained-view
//     ids, the final-capture-hook consumer, a scene-extent resize and an
//     output-transport recreate (the bridge's target and presentation events).
// Every run must produce zero validation messages, and submitFrame reports
// its stage boundaries in order.

#include "renderer/color/AcesOutputLut.h"
#include "renderer/rhi/EditorRenderBridge.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderBackendFactory.h"
#include "renderer/vulkan_imgui/VulkanImGuiEditorBridge.h"

#include <GLFW/glfw3.h>
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Iridium;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (" << __FILE__ << ':' \
                << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

namespace {

    class HiddenWindow {
    public:
        HiddenWindow() {
            if (glfwInit() != GLFW_TRUE) throw std::runtime_error("glfwInit failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            window_ = glfwCreateWindow(640, 360, "VulkanBackendFrameTests", nullptr, nullptr);
            if (window_ == nullptr) {
                glfwTerminate();
                throw std::runtime_error("glfwCreateWindow failed");
            }
        }
        ~HiddenWindow() {
            glfwDestroyWindow(window_);
            glfwTerminate();
        }
        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;
        [[nodiscard]] GLFWwindow* get() const noexcept { return window_; }

    private:
        GLFWwindow* window_ = nullptr;
    };

    // VkContext reports validation messages on std::cerr ("[Validation]: ").
    class ValidationCapture {
    public:
        ValidationCapture() : previous_(std::cerr.rdbuf(text_.rdbuf())) {}
        ~ValidationCapture() { std::cerr.rdbuf(previous_); }
        [[nodiscard]] size_t messages() const {
            const std::string text = text_.str();
            size_t count = 0;
            for (size_t at = text.find("[Validation]"); at != std::string::npos;
                    at = text.find("[Validation]", at + 1))
                ++count;
            return count;
        }
        [[nodiscard]] std::string text() const { return text_.str(); }

    private:
        std::ostringstream text_;
        std::streambuf* previous_ = nullptr;
    };

    class StageLog final : public IRenderFrameStageObserver {
    public:
        void onRenderFrameStage(RenderFrameStage stage) override {
            if (count < stages.size()) stages[count] = stage;
            ++count;
        }
        std::array<RenderFrameStage, 8> stages{};
        size_t count = 0;
    };

    RenderBackendConfig deviceConfig() {
        RenderBackendConfig config{};
        config.enableValidation = true;
        config.enableSynchronizationValidation = true;
        return config;
    }

    // The output transform samples the pinned ACES 2 LUT, as in the engine.
    TextureHandle installOutputLut(IRenderBackend& backend) {
        const Color::AcesOutputLut lut = Color::loadAcesOutputLut(
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "color" /
            "aces2_rec709_100nit_srgb_128.irlt");
        const TextureHandle texture = backend.allocateTexture(TextureDesc{
            .width = lut.width(),
            .height = lut.height(),
            .format = TextureFormat::RGBA32_SFloat,
            .usageClass = TextureUsageClass::Sampled2D,
            .sampler = {
                .minFilter = FilterMode::Nearest,
                .magFilter = FilterMode::Nearest,
                .addressU = SamplerAddressMode::ClampToEdge,
                .addressV = SamplerAddressMode::ClampToEdge,
                .addressW = SamplerAddressMode::ClampToEdge,
            },
        }, std::as_bytes(std::span(lut.rgba32f)));
        backend.setOutputTransformLut(texture);
        return texture;
    }

    // One empty frame through the public frame pipeline. Returns false when
    // the swapchain asked to be recreated.
    bool renderFrame(IRenderBackend& backend, IEditorRenderBridge* bridge,
        GLFWwindow* window, bool retainedViews, uint32_t renderView,
        StageLog* stages = nullptr, RenderFrameOutputSettings output = {}) {
        const LightingFramePacket lights{};
        const ReflectionProbeGpuFramePacket probes{};
        backend.prepareLighting(lights.requiredCapacity);
        backend.prepareGpuScene({});
        backend.prepareReflectionProbes(probes.requiredCapacity, {});
        if (bridge != nullptr) bridge->prepareRetainedViews(retainedViews, renderView);
        if (backend.beginFrame() == FrameStatus::RecreateSwapchain) {
            backend.recreateSwapchain(window);
            return false;
        }
        if (bridge != nullptr) {
            bridge->beginUI();
            ImGui::Begin("Viewport");
            ImGui::Image(reinterpret_cast<ImTextureID>(bridge->sceneTextureId()),
                ImVec2(64.0f, 36.0f));
            ImGui::Text("Iridium");
            ImGui::End();
        }
        const RenderExtent extent = backend.getRenderExtent();
        const glm::vec3 eye{ 0.0f, 1.0f, 4.0f };
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 projection = glm::perspective(glm::radians(60.0f),
            static_cast<float>(extent.width) / static_cast<float>(extent.height),
            0.1f, 100.0f);
        projection[1][1] *= -1.0f;
        RenderFrame frame{
            .view = makeViewTransportRecord(view, projection, eye, 0.1f, 100.0f,
                { extent.width, extent.height }),
            .history = { .identity = 1u },
            .output = output,
        };
        frame.submitReflectionProbeCaptures = true;
        frame.lights = &lights;
        frame.reflectionProbes = &probes;
        frame.stageObserver = stages;
        backend.submitFrame(frame);
        if (backend.endFrame() == FrameStatus::RecreateSwapchain) {
            backend.recreateSwapchain(window);
            return false;
        }
        return true;
    }

    bool testHeadlessFramesWithoutEditorBridge() {
        HiddenWindow window;
        ValidationCapture validation;
        {
            std::unique_ptr<IRenderBackend> backend =
                createRenderBackend(RenderBackendCreateInfo{});
            backend->init(window.get(), deviceConfig());
            (void)installOutputLut(*backend);
            uint32_t presented = 0;
            for (uint32_t frame = 0; frame < 8; ++frame)
                if (renderFrame(*backend, nullptr, window.get(), false, 0)) ++presented;
            CHECK(presented >= 6u);

            StageLog stages;
            (void)renderFrame(*backend, nullptr, window.get(), false, 0, &stages);
            constexpr std::array<RenderFrameStage, 7> expected{
                RenderFrameStage::DirectionalShadows, RenderFrameStage::SpotShadows,
                RenderFrameStage::PointShadows,
                RenderFrameStage::ReflectionProbeCaptures, RenderFrameStage::Lighting,
                RenderFrameStage::SceneLinearComplete, RenderFrameStage::OutputComplete };
            CHECK(stages.count == expected.size());
            for (size_t index = 0; index < expected.size(); ++index)
                CHECK(stages.stages[index] == expected[index]);

            // A frame outside beginFrame/endFrame, or without its packets, is
            // rejected.
            bool rejected = false;
            try { backend->submitFrame(RenderFrame{}); }
            catch (const std::logic_error&) { rejected = true; }
            CHECK(rejected);
            backend->cleanup();
        }
        if (validation.messages() != 0u) std::cout << validation.text();
        CHECK(validation.messages() == 0u);
        return true;
    }

    bool testEditorBridgeFrames() {
        HiddenWindow window;
        ValidationCapture validation;
        {
            const std::unique_ptr<IEditorRenderBridge> bridge =
                createVulkanImGuiEditorBridge(window.get());
            IRenderBackendExtension* const extensions[]{ &bridge->backendExtension() };
            std::unique_ptr<IRenderBackend> backend = createRenderBackend(
                RenderBackendCreateInfo{ .extensions = extensions });
            backend->init(window.get(), deviceConfig());
            (void)installOutputLut(*backend);
            // The bridge created the ImGui context; keep imgui.ini out of the tree.
            ImGui::GetIO().IniFilename = nullptr;

            for (uint32_t frame = 0; frame < 4; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), false, 0);
            CHECK(bridge->sceneTextureId() != nullptr);
            CHECK(bridge->glassDepthTextureId() != nullptr);
            CHECK(bridge->retainedViewTextureId(0) == nullptr);

            // A backend texture as an editor preview, released with the texture.
            const std::array<std::byte, 16> pixels{};
            const TextureHandle texture = backend->allocateTexture(
                TextureDesc{ .width = 2, .height = 2 }, pixels);
            CHECK(bridge->editorTextureId(texture) != nullptr);
            CHECK(bridge->editorTextureId(texture) == bridge->editorTextureId(texture));

            // Dual retained views alternate through the final-capture hook.
            for (uint32_t frame = 0; frame < 4; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), true, frame % 2u);
            CHECK(bridge->retainedViewTextureId(0) != nullptr);
            CHECK(bridge->retainedViewTextureId(1) != nullptr);
            CHECK(bridge->retainedViewTextureId(0) != bridge->retainedViewTextureId(1));

            backend->freeTexture(texture);
            CHECK(bridge->editorTextureId(texture) == nullptr);

            // Scene targets rebuilt (onFrameTargetsReleased/Created) and the
            // swapchain recreated (onPresentationChanged).
            std::string diagnostic;
            CHECK(backend->resizeSceneRenderExtent({ 320, 180 }, diagnostic));
            void* const resizedScene = bridge->sceneTextureId();
            CHECK(resizedScene != nullptr);
            backend->setOutputTransport(window.get(), Color::OutputTransport::SdrSrgb);
            for (uint32_t frame = 0; frame < 4; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), frame >= 2u, 0);
            CHECK(bridge->sceneTextureId() != nullptr);
            // M9.2c: a live anti-aliasing switch rebuilds the graph and the
            // editor targets, both ways; the same mode is a no-op.
            CHECK(backend->setAntiAliasing(AntiAliasingMode::Taa, diagnostic));
            CHECK(diagnostic.empty());
            CHECK(bridge->sceneTextureId() != nullptr);
            for (uint32_t frame = 0; frame < 4; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), frame >= 2u, 0);
            CHECK(backend->setAntiAliasing(AntiAliasingMode::Taa, diagnostic));
            CHECK(backend->setAntiAliasing(AntiAliasingMode::None, diagnostic));
            for (uint32_t frame = 0; frame < 2; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), false, 0);
            CHECK(bridge->sceneTextureId() != nullptr);
            // M9.4: bloom on rebuilds the graph (post.bloom and its chain),
            // with and without TAA; an intensity or threshold change applies
            // without a rebuild; off restores the hook topology.
            BloomSettings bloom{};
            bloom.enabled = true;
            CHECK(backend->setBloom(bloom, diagnostic));
            CHECK(diagnostic.empty());
            CHECK(bridge->sceneTextureId() != nullptr);
            for (uint32_t frame = 0; frame < 3; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), frame >= 1u, 0);
            bloom.intensity = 0.2f;
            bloom.threshold = 1.0f;
            bloom.knee = 0.5f;
            CHECK(backend->setBloom(bloom, diagnostic));
            CHECK(backend->setAntiAliasing(AntiAliasingMode::Taa, diagnostic));
            for (uint32_t frame = 0; frame < 3; ++frame)
                (void)renderFrame(*backend, bridge.get(), window.get(), false, 0);
            CHECK(backend->setAntiAliasing(AntiAliasingMode::None, diagnostic));
            bloom.enabled = false;
            CHECK(backend->setBloom(bloom, diagnostic));
            (void)renderFrame(*backend, bridge.get(), window.get(), false, 0);
            CHECK(bridge->sceneTextureId() != nullptr);
            // Live output settings reach the bridge's display colour.
            (void)renderFrame(*backend, bridge.get(), window.get(), false, 0, nullptr,
                { .manualExposureEv = 0.5f, .paperWhiteNits = 240.0f, .peakNits = 1000.0f });
            backend->cleanup();
            CHECK(bridge->sceneTextureId() == nullptr);
        }
        if (validation.messages() != 0u) std::cout << validation.text();
        CHECK(validation.messages() == 0u);
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Headless frames without an editor bridge", testHeadlessFramesWithoutEditorBridge },
        { "Editor bridge frames", testEditorBridgeFrames },
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
    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
