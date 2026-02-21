#include "EditorSystem.h"
#include "scene/Components.h"
#include "panels/core/SceneHierarchyPanel.h"
#include "panels/core/InspectorPanel.h"
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
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable windows outside of engine

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
    // panels.push_back(std::make_unique<ToolbarPanel>(&currentToolMode));
    // panels.push_back(std::make_unique<RenderModePanel>(&currentRenderMode));
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
    const glm::mat4& viewInput, const glm::mat4& projInput, VkDescriptorSet sceneTexture) {

    // Standard Frame Setup
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);

    // Draw editor panels
    for (auto& panel : panels) {
        panel->OnImGuiRender(registry, assetManager);
    }

    glm::mat4 view = viewInput;
    glm::mat4 proj = projInput;

    // --- 2. CUSTOM ECS SELECTED ENTITY SEARCH ---
    auto* transformPool = registry.getPool<TransformComponent>();
    TransformComponent* selectedTransform = nullptr;
    
    // Check if we have a valid selected entity and if the pool exists
    if (selectedEntity != NULL_ENTITY && transformPool) {
        if (transformPool->has(selectedEntity)) {
            selectedTransform = &transformPool->get(selectedEntity);
        }
    }

    // Define the gizmo operation locally so it remembers what tool is active
       static ImGuizmo::OPERATION currentGizmoOperation = ImGuizmo::TRANSLATE;

    // --- 3. RENDER THE VIEWPORT PANEL ---
    viewportPanel.render(sceneTexture,
        currentRenderMode,
        currentGizmoOperation,
        view,
        proj,
        selectedTransform);

    ImGui::Render();
}

void EditorSystem::render(VkCommandBuffer cmd) {
    // Record the draw data into the command buffer
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}