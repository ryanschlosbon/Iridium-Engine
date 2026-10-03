// M7R R3c.10: the editor render bridge contract without a device. The ImGui
// bridge (iridium_vulkan_imgui) is a Vulkan backend extension that serves the
// editor-UI service; before the backend's device is ready it hands out no
// texture ids and refuses retained-view preparation. VulkanBackendFrameTests
// covers the bridge on a device.

#include "renderer/rhi/EditorRenderBridge.h"
#include "renderer/rhi/RenderBackendFactory.h"
#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanEditorUi.h"
#include "renderer/vulkan/VulkanExtensionHooks.h"
#include "renderer/vulkan_imgui/VulkanImGuiEditorBridge.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

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

    // The bridge stores the window and uses it only once the device is ready.
    GLFWwindow* fakeWindow() {
        static int storage = 0;
        return reinterpret_cast<GLFWwindow*>(&storage);
    }

    class PlainExtension final : public IVulkanBackendExtension {};

    template <typename Exception, typename Function>
    bool throws(Function&& function) {
        try { function(); }
        catch (const Exception&) { return true; }
        catch (...) { return false; }
        return false;
    }

    bool testBridgeIsAVulkanEditorUiExtension() {
        CHECK(throws<std::invalid_argument>([] {
            (void)createVulkanImGuiEditorBridge(nullptr);
        }));
        const std::unique_ptr<IEditorRenderBridge> bridge =
            createVulkanImGuiEditorBridge(fakeWindow());
        IRenderBackendExtension& extension = bridge->backendExtension();
        CHECK(extension.api() == RenderBackendApi::Vulkan);
        auto* vulkan = dynamic_cast<IVulkanBackendExtension*>(&extension);
        CHECK(vulkan != nullptr);
        CHECK(vulkan->editorUi() != nullptr);
        // It declares no graph hooks and wants no hook, so attaching it
        // changes neither the topology nor any qualification hook.
        const VulkanGraphHooks hooks = vulkan->graphHooks();
        const VulkanGraphHooks none = VulkanGraphHooks::none();
        CHECK(hooks.depthPyramidValidation == none.depthPyramidValidation);
        CHECK(hooks.layeredValidation == none.layeredValidation);
        CHECK(hooks.virtualShadowDepthSnapshot == none.virtualShadowDepthSnapshot);
        CHECK(hooks.sceneColorCapture == none.sceneColorCapture);
        CHECK(!vulkan->wantsHook({ .point = VulkanHookPoint::FinalCaptureHook }));
        CHECK(vulkan->indirectOracle() == nullptr);
        CHECK(vulkan->indirectStreamObserver() == nullptr);
        return true;
    }

    bool testBridgeBeforeTheDevice() {
        const std::unique_ptr<IEditorRenderBridge> bridge =
            createVulkanImGuiEditorBridge(fakeWindow());
        CHECK(bridge->sceneTextureId() == nullptr);
        CHECK(bridge->glassDepthTextureId() == nullptr);
        CHECK(bridge->editorTextureId(TextureHandle{}) == nullptr);
        for (uint32_t view = 0; view < 3; ++view)
            CHECK(bridge->retainedViewTextureId(view) == nullptr);
        CHECK(throws<std::out_of_range>([&] { bridge->prepareRetainedViews(true, 2); }));
        CHECK(throws<std::logic_error>([&] { bridge->prepareRetainedViews(false, 0); }));

        IVulkanEditorUi* ui = dynamic_cast<IVulkanBackendExtension&>(
            bridge->backendExtension()).editorUi();
        CHECK(!ui->retainedViewsEnabled());
        // Events before the device are no-ops.
        ui->onFrameTargetsCreated();
        ui->onFrameTargetsReleased();
        ui->onDisplayColorChanged(Color::OutputTransport::ScRgb, 203.0f);
        ui->onSwapchainImageCountChanged(3);
        ui->recordUi(VK_NULL_HANDLE);
        ui->onUiShutdown();
        CHECK(bridge->sceneTextureId() == nullptr);
        return true;
    }

    bool testExtensionHooksServeTheFirstEditorUi() {
        PlainExtension plain;
        const std::unique_ptr<IEditorRenderBridge> first =
            createVulkanImGuiEditorBridge(fakeWindow());
        const std::unique_ptr<IEditorRenderBridge> second =
            createVulkanImGuiEditorBridge(fakeWindow());
        const auto editorUiOf = [](IEditorRenderBridge& bridge) {
            return dynamic_cast<IVulkanBackendExtension&>(
                bridge.backendExtension()).editorUi();
        };

        VulkanExtensionHooks headless;
        headless.attach(&plain, false);
        headless.configure({});
        CHECK(headless.editorUi() == nullptr);

        VulkanExtensionHooks hooks;
        hooks.attach(&plain, false);
        hooks.attach(&first->backendExtension(), false);
        hooks.attach(&second->backendExtension(), false);
        hooks.configure({});
        CHECK(hooks.editorUi() == editorUiOf(*first));
        CHECK(hooks.extensions().size() == 3u);
        // Attachment is a before-init operation.
        CHECK(throws<std::logic_error>([&] {
            hooks.attach(&first->backendExtension(), true);
        }));

        // The factory accepts the bridge next to other Vulkan extensions.
        IRenderBackendExtension* const list[]{ &plain, &first->backendExtension() };
        CHECK(createRenderBackend(RenderBackendCreateInfo{
            .api = RenderBackendApi::Vulkan, .extensions = list }) != nullptr);
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Bridge is a Vulkan editor-UI extension", testBridgeIsAVulkanEditorUiExtension },
        { "Bridge before the device", testBridgeBeforeTheDevice },
        { "Extension hooks serve the first editor UI",
            testExtensionHooksServeTheFirstEditorUi },
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
