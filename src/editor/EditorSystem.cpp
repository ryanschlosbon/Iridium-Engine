#include "EditorSystem.h"
#include "scene/Components.h"
#include "panels/core/SceneHierarchyPanel.h"
#include "panels/core/InspectorPanel.h"
#include "panels/core/ToolbarPanel.h"
#include "panels/core/RenderModePanel.h"
#include "panels/menus/MenuBarPanel.h"
#include "panels/windows/ProjectSettingsPanel.h"

#include <vector>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"
#include "vendor/imguizmo/ImGuizmo.h"

static void check_vk_result(VkResult err) {
    if (err == 0) return;
    fprintf(stderr, "[ImGui Vulkan Error] VkResult = %d\n", err);
    if (err < 0) abort();
}

EditorSystem::~EditorSystem() = default;

void EditorSystem::init(VkInstance instance, VkDevice device, VkPhysicalDevice physicalDevice,
    VkQueue graphicsQueue, VkRenderPass renderPass, GLFWwindow* window, VkCommandPool cmdPool) {

    // 1. Create a Descriptor Pool specifically for ImGui
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
    pool_info.pPoolSizes = pool_sizes;

    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &imguiPool) != VK_SUCCESS) {
        throw std::runtime_error("failed to create imgui descriptor pool!");
    }

    // 2. Initialize ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = physicalDevice;
    init_info.Device = device;
    init_info.QueueFamily = 0; // Ensure this matches your actual Graphics Queue Index
    init_info.Queue = graphicsQueue;
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    init_info.CheckVkResultFn = check_vk_result; // Use the robust static function

    // Newer ImGui versions require this sub-struct for the pipeline
    init_info.PipelineInfoMain.RenderPass = renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed!");
    }

    panels.push_back(std::make_unique<SceneHierarchyPanel>(&selectedEntity));
    panels.push_back(std::make_unique<InspectorPanel>(&selectedEntity));
    panels.push_back(std::make_unique<ToolbarPanel>(&currentToolMode));
    panels.push_back(std::make_unique<RenderModePanel>(&currentRenderMode));
    panels.push_back(std::make_unique<MenuBarPanel>(&selectedEntity, &uiState));
    panels.push_back(std::make_unique<ProjectSettingsPanel>(&uiState.showProjectSettings));
}

void EditorSystem::cleanup(VkDevice device) {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, imguiPool, nullptr);
}

void EditorSystem::update(Registry& registry, AssetManager* assetManager,
    const glm::mat4& view, const glm::mat4& proj) {

    // Standard Frame Setup
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    // Draw editor panels
    for (auto& panel : panels) {
        panel->OnImGuiRender(registry, assetManager);
    }

    // GIZMO DRAWING
    // Only draw if an entity is selected AND we are in Move/Rotate/Scale mode
    if (selectedEntity != NULL_ENTITY && currentToolMode > 0) {
        auto* transformPool = registry.getPool<TransformComponent>();

        if (transformPool && transformPool->has(selectedEntity)) {
            auto& transform = transformPool->get(selectedEntity);

            // FIX 1: Bind strictly to the raw Display Size, bypassing ImGui window offsets.
            // This ensures ImGuizmo's 2D space perfectly matches the Vulkan Swapchain 1:1.
            ImGuiIO& io = ImGui::GetIO();
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
            ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

            // Map Iridium Engine's Toolbar Modes to ImGuizmo Operations
            ImGuizmo::OPERATION snapOp = ImGuizmo::TRANSLATE;
            if (currentToolMode == 1) snapOp = ImGuizmo::TRANSLATE;
            if (currentToolMode == 2) snapOp = ImGuizmo::ROTATE;
            if (currentToolMode == 3) snapOp = ImGuizmo::SCALE;

            // FIX 2: Sanitize the projection matrix for ImGuizmo's internal OpenGL hit-testing.
            // If the camera does a Vulkan Y-flip, we temporarily un-flip it just for this calculation.
            glm::mat4 gizmoProj = proj;
            if (gizmoProj[1][1] < 0.0f) {
                gizmoProj[1][1] *= -1.0f;
            }

            // Draw the Gizmo and handle user interaction
            bool isManipulated = ImGuizmo::Manipulate(
                glm::value_ptr(view),
                glm::value_ptr(gizmoProj),
                snapOp,
                ImGuizmo::LOCAL,
                glm::value_ptr(transform.worldMatrix)
            );

            if (isManipulated) {
                glm::vec3 newPos, newRot, newScale;

                // Extract the modified data from the matrix
                ImGuizmo::DecomposeMatrixToComponents(
                    glm::value_ptr(transform.worldMatrix),
                    glm::value_ptr(newPos),
                    glm::value_ptr(newRot),
                    glm::value_ptr(newScale)
                );

                transform.position = newPos;
                transform.rotation = newRot;
                transform.scale = newScale;

                // Flag the TransformSystem to recalculate matrices next frame
                transform.isDirty = true;
            }
        }
    }

    ImGui::Render();
}

void EditorSystem::render(VkCommandBuffer cmd) {
    // Record the draw data into the command buffer
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}