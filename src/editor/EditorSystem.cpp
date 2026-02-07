#include "EditorSystem.h"
#include "scene/Components.h" // Need access to Transform/Mesh components
#include <vector>
#include <iostream>
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_glfw.h"

static void check_vk_result(VkResult err) {
    if (err == 0) return;
    fprintf(stderr, "[ImGui Vulkan Error] VkResult = %d\n", err);
    if (err < 0) abort();
}

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
}

void EditorSystem::cleanup(VkDevice device) {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(device, imguiPool, nullptr);
}

void EditorSystem::update(Registry& registry, AssetManager* assetManager) {
    // Start the frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // --- WINDOW 1: SCENE HIERARCHY ---
    ImGui::Begin("Scene Hierarchy");

    auto* meshPool = registry.getPool<MeshComponent>();
    // Loop through all entities that have a mesh
    for (size_t i = 0; i < meshPool->entities.size(); i++) {
        Entity e = meshPool->entities[i];
        std::string label = "Entity " + std::to_string(e);

        if (ImGui::Selectable(label.c_str(), selectedEntity == e)) {
            selectedEntity = e;
        }
    }

    // --- IMPORTER SECTION ---
    ImGui::Separator();
    ImGui::Text("Import New Model");
    ImGui::InputText("Path", importPathBuffer, sizeof(importPathBuffer));
    if (ImGui::Button("Import")) {
        try {
            // Build the full path using the project root
            std::string fullPath = std::string(PROJECT_ROOT_DIR) + importPathBuffer;

            auto newModel = assetManager->getModel(fullPath);

            Entity newEntity = registry.createEntity();
            registry.addComponent<MeshComponent>(newEntity, newModel);
            registry.addComponent<TransformComponent>(newEntity);

            selectedEntity = newEntity;
        }
        catch (const std::exception& e) {
            std::cerr << "IMPORT ERROR: " << e.what() << std::endl;
        }
    }
    ImGui::End();

    // --- WINDOW 2: INSPECTOR ---
    ImGui::Begin("Inspector");
    if (selectedEntity != NULL_ENTITY) {
        auto* transformPool = registry.getPool<TransformComponent>();

        // Check if selected entity has a transform
        if (transformPool->sparseMap.contains(selectedEntity)) {
            auto& transform = transformPool->get(selectedEntity);

            ImGui::Text("Transform Component");
            // Direct memory access allows real-time editing!
            ImGui::DragFloat3("Position", &transform.position.x, 0.1f);
            ImGui::DragFloat3("Rotation", &transform.rotation.x, 1.0f);
            ImGui::DragFloat3("Scale", &transform.scale.x, 0.1f);
        }
    }
    ImGui::End();

    // Render the data (internally)
    ImGui::Render();
}

void EditorSystem::render(VkCommandBuffer cmd) {
    // Record the draw data into the command buffer
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}