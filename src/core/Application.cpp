#include "Application.h"
#include <iostream>
#include <algorithm>

#include "renderer/vulkan/VulkanVertexBackend.h" 
#include "scene/components/MeshComponent.h"
#include "renderer/rhi/Mesh.h"

namespace Iridium {

    void Application::run() {
        initWindow();
        initRenderer();
        mainLoop();
        cleanup();
    }

    void Application::initWindow() {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Tell GLFW not to create an OpenGL context
        window = glfwCreateWindow(1280, 720, "Iridium Engine", nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetScrollCallback(window, scroll_callback);
        glfwSetMouseButtonCallback(window, mouse_button_callback);
    }

    void Application::initRenderer() {
        // 1. Instantiate the RHI (The Strategy Pattern in action)
        // If you write a DX12 backend later, you literally just change this ONE line.
        renderBackend = new VulkanVertexBackend();
        renderBackend->init(window);

        editor.init(window);

        // 2. Inject the backend into the Asset Manager
        assetManager = new AssetManager(renderBackend);

        // 3. Load your default scene/assets
        // (Your AssetManager now handles all the RHI calls internally)
        mainModel = assetManager->getModel(std::string(PROJECT_ROOT_DIR) + "assets/models/alfa_romeo/scene.gltf");
        hdriMap = assetManager->loadHDRI(std::string(PROJECT_ROOT_DIR) + "assets/hdri/cobblestone_street_night_8k.hdr");
        renderBackend->setEnvironmentMap(hdriMap);

        // 2. SPAWN THE CAR INTO THE WORLD
        Entity carEntity = registry.createEntity();

        // Let the ECS create it first, then assign the values to fix the compiler error!
        auto& transform = registry.addComponent<TransformComponent>(carEntity);
        transform.position = glm::vec3(0.0f, -1.0f, 0.0f);
        transform.rotation = glm::vec3(0.0f, 0.0f, 0.0f);
        transform.scale = glm::vec3(1.0f, 1.0f, 1.0f);
        transform.worldMatrix = glm::mat4(1.0f); // Safety initialization
        transform.isDirty = true;

        // Add the Mesh
        auto& meshComp = registry.addComponent<MeshComponent>(carEntity);
        meshComp.model = mainModel;
        meshComp.enabled = true;

        editor.setSelectedEntity(carEntity);
    }

    void Application::mainLoop() {
        float lastFrameTime = 0.0f;

        // --- Added for FPS Tracking ---
        int frameCount = 0;
        float timeAccumulator = 0.0f;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // 1. Time & Input
            float currentFrameTime = static_cast<float>(glfwGetTime());
            deltaTime = currentFrameTime - lastFrameTime;
            lastFrameTime = currentFrameTime;

            // --- FPS CALCULATION ---
            frameCount++;
            timeAccumulator += deltaTime;

            // Update the window title once every second
            if (timeAccumulator >= 1.0f) {
                std::string title = "Iridium Engine - FPS: " + std::to_string(frameCount) +
                    " (" + std::to_string(1000.0f / frameCount).substr(0, 4) + " ms/frame)";
                glfwSetWindowTitle(window, title.c_str());

                frameCount = 0;
                timeAccumulator -= 1.0f;
            }

            processInput(window);

            // 2. Process delayed ECS events (like swapping meshes on the main thread)
            ProcessMeshSwaps();

            // 3. Update ECS Systems (Physics, Transforms, Animations)
            // This recalculates all local/world matrices before we extract them.
            transformSystem.update(registry);

            // --- CALCULATE REAL CAMERA MATRICES ---
            glm::mat4 viewMatrix = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

            // Note: 16.0f / 9.0f is a standard 16:9 aspect ratio. You can swap this with 
            // (float)windowWidth / (float)windowHeight later if you want dynamic resizing!
            glm::mat4 projMatrix = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);
            projMatrix[1][1] *= -1.0f;

            // 4. Update Editor UI
            renderBackend->beginUI();

            editor.update(registry, assetManager,
                viewMatrix, // Pass the calculated view matrix
                projMatrix, // Pass the calculated projection matrix
                renderBackend->getLitSceneTextureID(),
                renderBackend->getGlassDepthTextureID());

            // 5. The Magic Data-Driven Render Loop
            drawFrame();
        }
    }

    void Application::drawFrame() {
        // If the window was resized, OR Vulkan tells us the swapchain is out of date:
        if (framebufferResized || !renderBackend->beginFrame()) {
            framebufferResized = false;
            renderBackend->recreateSwapchain(window); // Safely rebuild!
            ImGui::EndFrame(); // Cancel the UI frame we started in mainLoop
            return;
        }

        // --- 1. CLEAR THE QUEUES ---
        opaqueQueue.clear();
        transparentQueue.clear();
        selectionQueue.clear();

        Entity selectedEntity = editor.getSelectedEntity();

        // --- 2. GET CAMERA DATA ---
        glm::mat4 viewMatrix = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
        glm::mat4 projMatrix = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        projMatrix[1][1] *= -1.0f; // Vulkan inverted Y

        renderBackend->updateCamera(viewMatrix, projMatrix);

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        auto* transformPool = registry.getPool<TransformComponent>();
        auto* meshPool = registry.getPool<MeshComponent>();

        if (transformPool && meshPool) {
            for (uint32_t entity : meshPool->entities) {
                auto& meshComp = meshPool->get(entity);

                // Skip disabled meshes, empty models, or entities without transforms
                if (!meshComp.enabled || !meshComp.model || !transformPool->has(entity)) continue;

                auto& transformComp = transformPool->get(entity);

                // Calculate distance for Back-to-Front glass sorting
                float distToCam = glm::distance(cameraPos, glm::vec3(transformComp.worldMatrix[3]));

                // Generate a DrawPacket for every submesh in the model
                for (size_t i = 0; i < meshComp.model->subMeshes.size(); i++) {

                    int matIndex = meshComp.model->subMeshes[i].materialIndex;
                    MaterialHandle matHandle = meshComp.model->materials[matIndex];

                    DrawPacket packet{};
                    packet.geometry = meshComp.model->geometry;
                    packet.material = matHandle;
                    packet.indexCount = meshComp.model->subMeshes[i].indexCount;
                    packet.firstIndex = meshComp.model->subMeshes[i].indexStart;
                    packet.worldTransform = transformComp.worldMatrix;
                    packet.distanceToCamera = distToCam;
                    packet.isSelected = (entity == selectedEntity) ? 1 : 0;

                    // Sort into Opaque or Transparent queues
                    // (Assuming you track AlphaMode inside your updated Material struct or Asset)
                    // For now, checking a hypothetical boolean:
                    if (meshComp.model->materialIsTransparent[matIndex]) {
                        transparentQueue.push_back(packet);
                    }
                    else {
                        opaqueQueue.push_back(packet);
                    }

                    // OPTIMIZATION: If this is the selected entity, copy the packet to the selection queue!
                    if (entity == selectedEntity) {
                        selectionQueue.push_back(packet);
                    }
                }
            }
        }

        // --- 4. THE SORTING PHASE (CPU Cache Optimization) ---

        // Group opaque objects by material ticket to eliminate redundant Vulkan pipeline state changes
        std::sort(opaqueQueue.begin(), opaqueQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
            return a.material < b.material;
            });

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        std::sort(transparentQueue.begin(), transparentQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
            return a.distanceToCamera > b.distanceToCamera;
            });

        // --- 5. THE SUBMISSION PHASE (The Black Box) ---

        // Pass 1: Opaque G-Buffer
        bool isWireframe = (editor.currentRenderMode == 1);
        renderBackend->submitOpaqueQueue(opaqueQueue, selectionQueue, isWireframe);

        // Pass 2: Deferred Lighting 
        renderBackend->submitLightingPass(cameraPos, viewMatrix, projMatrix);

        // Pass 3 & 4: The AAA Translucency Pipeline
        renderBackend->submitGlassDepthPass(transparentQueue);
        renderBackend->submitTransparentQueue(transparentQueue);

        // Pass 5: ImGui/Editor UI
        renderBackend->submitUIPass();

        renderBackend->endFrame();
    }

    void Application::cleanup() {
        if (assetManager) {
            delete assetManager;
        }

        // The backend's cleanup handles all Vulkan destruction (Swapchains, Buffers, Images)
        if (renderBackend) {
            renderBackend->cleanup();
            delete renderBackend;
        }

        glfwDestroyWindow(window);
        glfwTerminate();
    }

    // --- GLFW CALLBACK STUBS ---
    void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
        if (app->renderBackend) {
            app->renderBackend->recreateSwapchain(window);
        }
    }

    void Application::mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
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

        // Only rotate if Right Click is held down
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
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
            app->cameraFront = glm::normalize(front);
        }
    }

    void Application::scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        // Use scroll wheel to change camera fly speed
        app->cameraSpeed += static_cast<float>(yoffset) * 0.5f;
        if (app->cameraSpeed < 0.1f) app->cameraSpeed = 0.1f;
        if (app->cameraSpeed > 20.0f) app->cameraSpeed = 20.0f;
    }

    void Application::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (!app) return;

        // Only activate camera look on Right Click
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_PRESS) {
                app->firstMouse = true; // Prevent violent camera snapping
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); // Hide cursor
            }
            else if (action == GLFW_RELEASE) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // Show cursor
            }
        }
    }
    void Application::processInput(GLFWwindow* window) {
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // Only move camera if Right Mouse Button is held down (standard editor behavior)
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            float velocity = cameraSpeed * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                cameraPos += cameraFront * velocity;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                cameraPos -= cameraFront * velocity;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * velocity;
        }
    }

    void Application::ProcessMeshSwaps() { /* ... */ }
    void Application::recreateSwapchain() { /* ... */ }
    void Application::selectEntityAtMouse(double mouseX, double mouseY) { /* ... */ }

} // namespace Iridium