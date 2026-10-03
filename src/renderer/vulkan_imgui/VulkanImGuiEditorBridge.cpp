#include "renderer/vulkan_imgui/VulkanImGuiEditorBridge.h"

#include "renderer/vulkan/VulkanBackendExtension.h"
#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanEditorUi.h"
#include "renderer/vulkan/VulkanFrameScheduler.h"
#include "renderer/vulkan/VulkanFrameTargets.h"
#include "renderer/vulkan/VulkanFrameTelemetry.h"
#include "renderer/vulkan/VulkanResourceAllocator.h"
#include "renderer/vulkan/VulkanResourceRegistry.h"
#include "utils/File.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "vendor/imguizmo/ImGuizmo.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Iridium {

    namespace {

        // ImGui's linear-HDR display configuration for an output transport.
        float displayColorScale(Color::OutputTransport transport,
            float paperWhiteNits) noexcept {
            return transport == Color::OutputTransport::ScRgb
                ? paperWhiteNits / 80.0f : 1.0f;
        }

        uint32_t outputColorSpace(Color::OutputTransport transport) noexcept {
            return transport == Color::OutputTransport::Hdr10Pq ? 1u : 0u;
        }

        class VulkanImGuiEditorBridge final : public IEditorRenderBridge,
            public IVulkanBackendExtension, public IVulkanEditorUi {
        public:
            explicit VulkanImGuiEditorBridge(GLFWwindow* window) : window_(window) {
                if (window_ == nullptr)
                    throw std::invalid_argument("The ImGui editor bridge needs a window");
            }
            ~VulkanImGuiEditorBridge() override = default;

            // IEditorRenderBridge
            IRenderBackendExtension& backendExtension() noexcept override { return *this; }
            void beginUI() override;
            void* sceneTextureId() override;
            void* glassDepthTextureId() override;
            void* editorTextureId(TextureHandle texture) override;
            void prepareRetainedViews(bool enabled, uint32_t renderView) override;
            void* retainedViewTextureId(uint32_t view) override {
                return view < 2 ? reinterpret_cast<void*>(retainedViewDescriptors_[view]) : nullptr;
            }

            // IVulkanBackendExtension
            IVulkanEditorUi* editorUi() noexcept override { return this; }

            // IVulkanEditorUi
            void onUiDeviceReady(const VulkanEditorUiDevice& device,
                const VulkanEditorUiPresentation& presentation) override;
            void onFrameTargetsCreated() override;
            void onFrameTargetsReleased() override;
            void onPresentationChanged(
                const VulkanEditorUiPresentation& presentation) override;
            void onSwapchainImageCountChanged(uint32_t imageCount) override;
            void onDisplayColorChanged(Color::OutputTransport transport,
                float paperWhiteNits) override;
            bool retainedViewsEnabled() const noexcept override {
                return retainedViewsEnabled_;
            }
            void prepareRetainedViewImages(VkCommandBuffer commandBuffer) override;
            void copyRetainedView(VkCommandBuffer commandBuffer) override;
            void recordUi(VkCommandBuffer commandBuffer) override;
            void onUiShutdown() noexcept override;

        private:
            void destroyRetainedViews();
            // M7R R4a: ImGui's main pipeline targets the "ui" pass's colour
            // format (dynamic rendering); ImGui deep-copies the format array.
            VkPipelineRenderingCreateInfoKHR pipelineRendering(
                const VulkanEditorUiPresentation& presentation) {
                colorFormat_ = presentation.colorFormat;
                VkPipelineRenderingCreateInfoKHR info{
                    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
                info.colorAttachmentCount = 1;
                info.pColorAttachmentFormats = &colorFormat_;
                return info;
            }

            VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;

            GLFWwindow* window_ = nullptr;
            VulkanEditorUiDevice device_{};
            bool initialized_ = false;
            VkDescriptorPool pool_ = VK_NULL_HANDLE;
            std::vector<uint32_t> fragmentShaderCode_;
            // Per frame slot: the display-output target and depth.
            std::vector<VkDescriptorSet> sceneTextures_;
            std::vector<VkDescriptorSet> depthTextures_;
            std::array<VulkanImageResource, 2> retainedViewImages_{};
            std::array<VkDescriptorSet, 2> retainedViewDescriptors_{};
            VkSampler retainedViewSampler_ = VK_NULL_HANDLE;
            uint32_t retainedRenderView_ = 0;
            bool retainedViewsEnabled_ = false;
        };

        void VulkanImGuiEditorBridge::onUiDeviceReady(const VulkanEditorUiDevice& device,
            const VulkanEditorUiPresentation& presentation) {
            if (initialized_)
                throw std::logic_error("The ImGui editor bridge is attached to one backend");
            device_ = device;
            // A retired texture releases its editor descriptor when the
            // registry finally destroys it.
            device_.resources->setEditorDescriptorRelease(this,
                [](void* owner, VkDescriptorSet descriptor) {
                    if (static_cast<VulkanImGuiEditorBridge*>(owner)->initialized_)
                        ImGui_ImplVulkan_RemoveTexture(descriptor);
                });

            // A small pool for ImGui's internal fonts and editor textures.
            VkDescriptorPoolSize poolSizes[] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 } };
            VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 4096;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = poolSizes;
            vkCreateDescriptorPool(device_.device, &poolInfo, nullptr, &pool_);

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            io.ConfigWindowsMoveFromTitleBarOnly = true;
            ImGui_ImplGlfw_InitForVulkan(window_, true);

            ImGui_ImplVulkan_InitInfo initInfo = {};
            initInfo.Instance = device_.instance;
            initInfo.PhysicalDevice = device_.physicalDevice;
            initInfo.Device = device_.device;
            initInfo.QueueFamily = device_.queueFamily;
            initInfo.Queue = device_.queue;
            initInfo.PipelineCache = device_.pipelineCache;
            initInfo.DescriptorPool = pool_;
            initInfo.MinImageCount = presentation.imageCount;
            initInfo.ImageCount = presentation.imageCount;
            // M7R R4a: the UI pass records with dynamic rendering.
            initInfo.ApiVersion = VK_API_VERSION_1_3;
            initInfo.UseDynamicRendering = true;
            initInfo.PipelineInfoMain.PipelineRenderingCreateInfo =
                pipelineRendering(presentation);
            const std::vector<char> fragmentBytes = readFile(
                std::string(PROJECT_ROOT_DIR) +
                "assets/shaders/imgui_color_managed_frag.spv");
            if (fragmentBytes.empty() || fragmentBytes.size() % sizeof(uint32_t) != 0) {
                throw std::runtime_error("Color-managed ImGui shader is invalid.");
            }
            fragmentShaderCode_.resize(fragmentBytes.size() / sizeof(uint32_t));
            std::memcpy(fragmentShaderCode_.data(), fragmentBytes.data(),
                fragmentBytes.size());
            initInfo.CustomShaderFragCreateInfo = {
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            initInfo.CustomShaderFragCreateInfo.codeSize =
                fragmentShaderCode_.size() * sizeof(uint32_t);
            initInfo.CustomShaderFragCreateInfo.pCode = fragmentShaderCode_.data();
            initInfo.DisplayColorScale = displayColorScale(presentation.transport,
                presentation.paperWhiteNits);
            initInfo.OutputColorSpace = outputColorSpace(presentation.transport);
            ImGui_ImplVulkan_Init(&initInfo);
            initialized_ = true;
        }

        void VulkanImGuiEditorBridge::onFrameTargetsCreated() {
            if (!initialized_) return;
            VulkanFrameTargets& frameTargets = *device_.frameTargets;
            sceneTextures_.resize(frameTargets.size());
            depthTextures_.resize(frameTargets.size());
            for (size_t index = 0; index < frameTargets.size(); ++index) {
                const VulkanFrameContextTargets& targets = frameTargets.get(index);
                sceneTextures_[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.output.view,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                depthTextures_[index] = ImGui_ImplVulkan_AddTexture(
                    frameTargets.sampler(), targets.depth.view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
        }

        void VulkanImGuiEditorBridge::onFrameTargetsReleased() {
            for (VkDescriptorSet texture : sceneTextures_)
                if (texture != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(texture);
            for (VkDescriptorSet texture : depthTextures_)
                if (texture != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(texture);
            sceneTextures_.clear();
            depthTextures_.clear();
        }

        void VulkanImGuiEditorBridge::onPresentationChanged(
            const VulkanEditorUiPresentation& presentation) {
            if (!initialized_) return;
            ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
            pipelineInfo.PipelineRenderingCreateInfo = pipelineRendering(presentation);
            pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            ImGui_ImplVulkan_CreateMainPipeline(&pipelineInfo);
            onDisplayColorChanged(presentation.transport, presentation.paperWhiteNits);
        }

        void VulkanImGuiEditorBridge::onSwapchainImageCountChanged(uint32_t imageCount) {
            if (initialized_) ImGui_ImplVulkan_SetMinImageCount(imageCount);
        }

        void VulkanImGuiEditorBridge::onDisplayColorChanged(
            Color::OutputTransport transport, float paperWhiteNits) {
            if (!initialized_) return;
            ImGui_ImplVulkan_SetDisplayColorConfiguration(
                displayColorScale(transport, paperWhiteNits),
                outputColorSpace(transport));
        }

        void VulkanImGuiEditorBridge::beginUI() {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGuizmo::BeginFrame();
        }

        void* VulkanImGuiEditorBridge::sceneTextureId() {
            if (!initialized_) return nullptr;
            const uint32_t frame = device_.scheduler->currentFrameIndex();
            return frame < sceneTextures_.size()
                ? reinterpret_cast<void*>(sceneTextures_[frame]) : nullptr;
        }

        void* VulkanImGuiEditorBridge::glassDepthTextureId() {
            if (!initialized_) return nullptr;
            const uint32_t frame = device_.scheduler->currentFrameIndex();
            return frame < depthTextures_.size()
                ? reinterpret_cast<void*>(depthTextures_[frame]) : nullptr;
        }

        void* VulkanImGuiEditorBridge::editorTextureId(TextureHandle texture) {
            if (!initialized_) return nullptr;
            VulkanTexturePayload* payload = device_.resources->textures().get(texture);
            if (payload == nullptr || payload->retired) return nullptr;
            if (payload->editorDescriptor == VK_NULL_HANDLE) {
                payload->editorDescriptor = ImGui_ImplVulkan_AddTexture(payload->sampler,
                    payload->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            return reinterpret_cast<void*>(payload->editorDescriptor);
        }

        void VulkanImGuiEditorBridge::destroyRetainedViews() {
            for (size_t i = 0; i < retainedViewImages_.size(); ++i) {
                if (retainedViewDescriptors_[i])
                    ImGui_ImplVulkan_RemoveTexture(retainedViewDescriptors_[i]);
                retainedViewDescriptors_[i] = VK_NULL_HANDLE;
                device_.allocator->destroy(retainedViewImages_[i]);
            }
            if (retainedViewSampler_)
                vkDestroySampler(device_.device, retainedViewSampler_, nullptr);
            retainedViewSampler_ = VK_NULL_HANDLE;
        }

        void VulkanImGuiEditorBridge::prepareRetainedViews(bool enabled, uint32_t renderView) {
            if (renderView >= 2) throw std::out_of_range("Retained view index");
            if (!initialized_)
                throw std::logic_error("Retained views need an initialized backend");
            retainedRenderView_ = renderView;
            device_.selectRetainedView(device_.backend, renderView);
            retainedViewsEnabled_ = enabled;
            const auto& output = device_.frameTargets->get(0).output;
            const auto& existing = retainedViewImages_[0];
            if (enabled && existing.isValid() &&
                existing.extent.width == output.extent.width &&
                existing.extent.height == output.extent.height &&
                existing.format == output.format) return;
            if (!enabled && !existing.isValid()) return;
            device_.scheduler->waitForAllFrames();
            destroyRetainedViews();
            if (!enabled) return;
            VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (vkCreateSampler(device_.device, &sampler, nullptr,
                    &retainedViewSampler_) != VK_SUCCESS)
                throw std::runtime_error("Retained view sampler allocation failed");
            try {
                for (size_t i = 0; i < retainedViewImages_.size(); ++i) {
                    retainedViewImages_[i] = device_.allocator->createImage2D(
                        output.extent, output.format,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, ProfileMemoryCategory::Texture);
                    retainedViewDescriptors_[i] = ImGui_ImplVulkan_AddTexture(
                        retainedViewSampler_, retainedViewImages_[i].view,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            catch (...) { destroyRetainedViews(); throw; }
        }

        void VulkanImGuiEditorBridge::prepareRetainedViewImages(VkCommandBuffer commandBuffer) {
            VulkanCommandList commands(commandBuffer);
            for (auto& image : retainedViewImages_) if (image.state == ResourceState::Undefined) {
                commands.transition(image, ResourceState::CopyDestination);
                const VkClearColorValue black{};
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(commandBuffer, image.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                commands.transition(image, ResourceState::ShaderResource);
            }
        }

        void VulkanImGuiEditorBridge::copyRetainedView(VkCommandBuffer commandBuffer) {
            VulkanCommandList commands(commandBuffer);
            auto& target = retainedViewImages_[retainedRenderView_];
            commands.transition(target, ResourceState::CopyDestination);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = { target.extent.width, target.extent.height, 1 };
            commands.copyImage(device_.frameTargets->get(
                device_.scheduler->currentFrameIndex()).output, target, copy);
            commands.transition(target, ResourceState::ShaderResource);
        }

        void VulkanImGuiEditorBridge::recordUi(VkCommandBuffer commandBuffer) {
            if (!initialized_) return;
            ImGui::Render();
            // The backend asks ImGui to record its vertex buffers into the
            // frame's command buffer, inside the cleared UI render pass.
            ImDrawData* drawData = ImGui::GetDrawData();
            if (drawData == nullptr) return;
            VulkanFrameTelemetry& telemetry = *device_.telemetry;
            const int framebufferWidth = static_cast<int>(
                drawData->DisplaySize.x * drawData->FramebufferScale.x);
            const int framebufferHeight = static_cast<int>(
                drawData->DisplaySize.y * drawData->FramebufferScale.y);
            if (telemetry.collecting() && framebufferWidth > 0 && framebufferHeight > 0) {
                telemetry.recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::ImGui));
                const ImVec2 clipOffset = drawData->DisplayPos;
                const ImVec2 clipScale = drawData->FramebufferScale;
                for (const ImDrawList* drawList : drawData->CmdLists) {
                    for (const ImDrawCmd& command : drawList->CmdBuffer) {
                        if (command.UserCallback != nullptr) {
                            if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                                telemetry.recordPipelineBind(
                                    pipelineIdentity(FixedPipelineIdentity::ImGui));
                            }
                            else {
                                ++telemetry.counters().uiUntrackedCallbacks;
                            }
                            continue;
                        }

                        ImVec2 clipMinimum{
                            (command.ClipRect.x - clipOffset.x) * clipScale.x,
                            (command.ClipRect.y - clipOffset.y) * clipScale.y };
                        ImVec2 clipMaximum{
                            (command.ClipRect.z - clipOffset.x) * clipScale.x,
                            (command.ClipRect.w - clipOffset.y) * clipScale.y };
                        clipMinimum.x = std::max(clipMinimum.x, 0.0f);
                        clipMinimum.y = std::max(clipMinimum.y, 0.0f);
                        clipMaximum.x = std::min(clipMaximum.x,
                            static_cast<float>(framebufferWidth));
                        clipMaximum.y = std::min(clipMaximum.y,
                            static_cast<float>(framebufferHeight));
                        if (clipMaximum.x <= clipMinimum.x ||
                            clipMaximum.y <= clipMinimum.y) {
                            continue;
                        }

                        telemetry.recordDraw(telemetry.counters().drawUi,
                            command.ElemCount / 3);
                    }
                }
            }
            ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        }

        void VulkanImGuiEditorBridge::onUiShutdown() noexcept {
            if (device_.device == VK_NULL_HANDLE) return;
            onFrameTargetsReleased();
            destroyRetainedViews();
            if (initialized_) {
                ImGui_ImplVulkan_Shutdown();
                ImGui_ImplGlfw_Shutdown();
                ImGui::DestroyContext();
                initialized_ = false;
            }
            fragmentShaderCode_.clear();
            if (pool_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_.device, pool_, nullptr);
                pool_ = VK_NULL_HANDLE;
            }
        }

    } // namespace

    std::unique_ptr<IEditorRenderBridge> createVulkanImGuiEditorBridge(GLFWwindow* window) {
        return std::make_unique<VulkanImGuiEditorBridge>(window);
    }

} // namespace Iridium
