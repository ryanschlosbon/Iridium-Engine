#include "EditorSystem.h"
#include "scene/Components.h" // Need access to Transform/Mesh components
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

void EditorSystem::update(Registry& registry, AssetManager* assetManager,
    const glm::mat4& view, const glm::mat4& proj) {

    // Standard Frame Setup
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    // Main Menu Bar
    if (ImGui::BeginMainMenuBar()) {

        // --- FILE MENU ---
        if (ImGui::BeginMenu("File")) {
            // Save Scene
            // Shortcut display "Ctrl+S" is just text; we have to handle the key press separately if we want hotkeys.
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                SceneSerializer serializer(registry, assetManager);
                // For now, we hardcode the path. Later, we can add a File Dialog.
                serializer.Serialize("assets/scenes/test_scene.json");
            }

            // Load Scene
            if (ImGui::MenuItem("Load Scene", "Ctrl+O")) {
                // 1. Clear the current world
                registry.clear();

                // 2. Create serializer (PASSING THE ASSET MANAGER)
                SceneSerializer serializer(registry, assetManager);

                // 3. Load
                // Use the relative path from your project root
                if (serializer.Deserialize("assets/scenes/test_scene.json")) {
                    // Reset selected entity so we don't crash trying to display a deleted one
                    selectedEntity = NULL_ENTITY;
                }
            }

            ImGui::Separator(); // Adds a nice line

            // Exit
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                // You'd need a pointer to the window to close it, 
                // or set a global "shouldClose" flag.
                // glfwSetWindowShouldClose(window, true); 
            }

            ImGui::EndMenu();
        }

        // --- EDIT MENU (Placeholder for future) ---
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
            ImGui::EndMenu();
        }

        // --- VIEW MENU (Placeholder) ---
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Hierarchy", nullptr, true); // boolean for checked state
            ImGui::MenuItem("Inspector", nullptr, true);
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    // =========================================================
    // 1. TOP TOOLBAR (Select, Move, Rotate, Scale)
    // =========================================================
    // We position this to the LEFT of the Render Mode dropdown.
    // The Render Dropdown is at (RightEdge - 10).
    // Let's place this at (RightEdge - 220) roughly.

    ImVec2 toolbarPos;
    toolbarPos.x = workPos.x + workSize.x - 220.0f;
    toolbarPos.y = workPos.y + PAD;

    ImGui::SetNextWindowPos(toolbarPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f)); // Pivot top-right
    ImGui::SetNextWindowBgAlpha(0.6f);

    ImGuiWindowFlags toolbarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("Toolbar", nullptr, toolbarFlags)) {
        // Mode 0: Select (No Color Push needed)
        bool isSelectActive = (currentToolMode == 0);
        if (isSelectActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.6f)); // Optional: Grey out if active
        if (ImGui::Button("Select")) currentToolMode = 0;
        if (isSelectActive) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select Only (No Gizmo)");

        ImGui::SameLine();

        // Mode 1: Translate
        bool isMoveActive = (currentToolMode == 1); // Capture state BEFORE button
        if (isMoveActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
        if (ImGui::Button("Move")) currentToolMode = 1;
        if (isMoveActive) ImGui::PopStyleColor(); // Use the CAPTURED state to Pop

        ImGui::SameLine();

        // Mode 2: Rotate
        bool isRotateActive = (currentToolMode == 2);
        if (isRotateActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
        if (ImGui::Button("Rotate")) currentToolMode = 2;
        if (isRotateActive) ImGui::PopStyleColor();

        ImGui::SameLine();

        // Mode 3: Scale
        bool isScaleActive = (currentToolMode == 3);
        if (isScaleActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.66f, 0.7f, 0.7f));
        if (ImGui::Button("Scale")) currentToolMode = 3;
        if (isScaleActive) ImGui::PopStyleColor();
    }
    ImGui::End();

    // =========================================================
    // 2. SCENE HIERARCHY
    // =========================================================
    ImGui::Begin("Scene Hierarchy");
    auto* transformPool = registry.getPool<TransformComponent>();

    // Iterate over TransformComponent, since every entity needs a transform component
    // Safety check: Make sure the pool actually exists
    if (transformPool) {
        for (size_t i = 0; i < transformPool->entities.size(); i++) {
            Entity e = transformPool->entities[i];

            // Give it a better name if it has a specific component
            std::string label = "Entity " + std::to_string(e);

            // Example: Append " (Light)" if it has a light
            if (registry.getPool<LightComponent>()->has(e)) label += " (Light)";

            if (ImGui::Selectable(label.c_str(), selectedEntity == e)) {
                selectedEntity = e;
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Import New Model");
    ImGui::InputText("Path", importPathBuffer, sizeof(importPathBuffer));
    if (ImGui::Button("Import")) {
        try {
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

    // =========================================================
    // 3. INSPECTOR (With Sliders, but NO Radio Buttons)
    // =========================================================
    ImGui::Begin("Inspector");
    if (selectedEntity != NULL_ENTITY) {

        static std::string inspectorError = "";
        static float errorTimer = 0.0f;

        ImGui::Text("Entity ID: %d", (int)selectedEntity);
        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
        if (ImGui::Button("DELETE ENTITY", ImVec2(-1, 0))) { // -1 width = spans full window
            registry.destroyEntity(selectedEntity);
            selectedEntity = NULL_ENTITY; // Deselect so we don't crash
            ImGui::PopStyleColor();
            ImGui::End();

            ImGui::Render();
            return; // Exit early to prevent drawing a dead entity
        }
        ImGui::PopStyleColor();
        ImGui::Separator();

        // --- AUTOMATIC INSPECTOR LOOP ---
        // Iterate over EVERY pool in the registry
        for (auto& [typeIndex, pool] : registry.pools) {

            // Check if the entity has this component
            // We use the base interface IComponentPool so we don't need to know the type!
            if (pool) {
                // Get a clean name
                std::string name = typeIndex.name();
                if (name.find("struct ") != std::string::npos) name = name.substr(7);
                if (name.find("class ") != std::string::npos) name = name.substr(6);

                // Draw the Header
                // We only want to draw the header IF the entity actually has the component.
                // So we need to expose 'has' or 'getVoid' to check first.
                if (pool->getVoid(selectedEntity) != nullptr) {
                    if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {

                        pool->DrawInspector(selectedEntity);

                        ImGui::Spacing();

                        if (name != "TransformComponent") {
                            if (ImGui::Button(("Remove " + name).c_str())) {
                                pool->remove(selectedEntity);
                            }
                        }
                        else {
                            ImGui::TextDisabled("(Required Component)");
                        }
                        ImGui::Separator();
                    }
                }
            }
        }

        // Add Component Button
        ImGui::Spacing();
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (ImGui::BeginPopup("AddComponentPopup")) {

            // Only show "Light" if the entity DOES NOT have a LightComponent
            if (!registry.getPool<LightComponent>()->has(selectedEntity)) {
                if (ImGui::MenuItem("Light")) {
                    registry.addComponent<LightComponent>(selectedEntity);
                }
            }

            // Only show "Mesh" if the entity DOES NOT have a MeshComponent
            if (!registry.getPool<MeshComponent>()->has(selectedEntity)) {
                if (ImGui::MenuItem("Mesh")) {
                    registry.addComponent<MeshComponent>(selectedEntity);
                }
            }

            // Transform is usually added by default, but we'll put the check here for safety
            if (!registry.getPool<TransformComponent>()->has(selectedEntity)) {
                if (ImGui::MenuItem("Transform")) {
                    registry.addComponent<TransformComponent>(selectedEntity);
                }
            }

            // If the list is empty (user added everything possible)
            if (registry.getPool<LightComponent>()->has(selectedEntity) &&
                registry.getPool<MeshComponent>()->has(selectedEntity)) {
                ImGui::TextDisabled("All components added");
            }

            ImGui::EndPopup();
        }

    }
    ImGui::End();

    // =========================================================
    // 4. RENDER MODE DROPDOWN (Existing)
    // =========================================================
    ImVec2 windowPos;
    windowPos.x = workPos.x + workSize.x - PAD;
    windowPos.y = workPos.y + PAD;
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);

    if (ImGui::Begin("RenderModeOverlay", nullptr, toolbarFlags)) {
        ImGui::Text("View Mode");
        ImGui::SameLine();
        const char* items[] = { "Standard", "Wireframe", "Outline Only" };
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("##renderMode", &currentRenderMode, items, IM_ARRAYSIZE(items));
    }
    ImGui::End();

    // =========================================================
        // 5. GIZMO DRAWING
        // =========================================================
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