#include "Application.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <span>
#include <string>

#include "renderer/rhi/RenderBackendFactory.h"
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
        renderBackend = createRenderBackend(RenderBackendApi::Vulkan);
        renderBackend->init(window);

        editor.init(window);

        // 2. Inject the backend into the Asset Manager
        assetManager = std::make_unique<AssetManager>(renderBackend.get());

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

            // 4. The frame acquisition must precede UI construction so the
            // viewport texture IDs correspond to the image acquired this frame.
            drawFrame();
        }
    }

    void Application::drawFrame() {
        // If the window was resized, OR acquire requests a swapchain rebuild:
        if (framebufferResized || renderBackend->beginFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
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

        // Build ImGui only after beginFrame selected currentImageIndex. The UI
        // descriptors are per swapchain image, so using them before acquisition
        // can sample a different target that has not yet been transitioned.
        renderBackend->beginUI();
        editor.update(registry, assetManager.get(), viewMatrix, projMatrix,
            renderBackend->getLitSceneTextureID(),
            renderBackend->getGlassDepthTextureID());

        renderBackend->updateCamera(viewMatrix, projMatrix);

        // --- 3. THE EXTRACTION PHASE (Data-Oriented Design) ---
        auto* transformPool = registry.getPool<TransformComponent>();
        auto* meshPool = registry.getPool<MeshComponent>();

        if (transformPool && meshPool) {
            for (uint32_t entity : meshPool->entities) {
                auto& meshComp = meshPool->get(entity);

                // Skip disabled meshes, empty models, or entities without transforms
                if (!meshComp.enabled || !meshComp.model || !transformPool->has(entity)) continue;

                const auto& model = *meshComp.model;
                if (!model.geometry.isValid()) continue;

                auto& transformComp = transformPool->get(entity);

                // Calculate distance for Back-to-Front glass sorting
                float distToCam = glm::distance(cameraPos, glm::vec3(transformComp.worldMatrix[3]));

                // Generate a DrawPacket for every submesh in the model
                for (size_t i = 0; i < model.subMeshes.size(); i++) {
                    const auto& subMesh = model.subMeshes[i];
                    int matIndex = subMesh.materialIndex;
                    if (matIndex < 0 || static_cast<size_t>(matIndex) >= model.materials.size()) {
                        continue;
                    }

                    const MaterialBinding& binding = model.materials[matIndex];
                    if (!binding.material.isValid() || !binding.pipeline.isValid()) {
                        continue;
                    }

                    DrawPacket packet{};
                    packet.geometry = model.geometry;
                    packet.material = binding.material;
                    packet.pipeline = binding.pipeline;
                    packet.opaqueSortKey = binding.opaqueSortKey;
                    packet.indexCount = subMesh.indexCount;
                    packet.firstIndex = subMesh.indexStart;
                    packet.worldTransform = transformComp.worldMatrix;
                    packet.distanceToCamera = distToCam;
                    packet.isSelected = (entity == selectedEntity) ? 1 : 0;

                    // Sort into Opaque or Transparent queues
                    if (binding.renderQueue == RenderQueue::Transparent) {
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

        // Group opaque objects by the PSO/material identity carried by each binding.
        std::sort(opaqueQueue.begin(), opaqueQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
            if (a.opaqueSortKey != b.opaqueSortKey) return a.opaqueSortKey < b.opaqueSortKey;
            if (a.geometry != b.geometry) return a.geometry < b.geometry;
            return a.firstIndex < b.firstIndex;
            });

        // Sort transparent objects Back-to-Front to ensure perfect alpha blending and refraction
        std::sort(transparentQueue.begin(), transparentQueue.end(), [](const DrawPacket& a, const DrawPacket& b) {
            if (a.distanceToCamera != b.distanceToCamera) return a.distanceToCamera > b.distanceToCamera;
            if (a.pipeline != b.pipeline) return a.pipeline < b.pipeline;
            if (a.material != b.material) return a.material < b.material;
            return a.geometry < b.geometry;
            });

        // --- 5. THE SUBMISSION PHASE (The Black Box) ---

        // Pass 1: Opaque G-Buffer
        bool isWireframe = (editor.currentRenderMode == 1);
        renderBackend->submitOpaqueQueue(
            std::span<const DrawPacket>(opaqueQueue.data(), opaqueQueue.size()),
            std::span<const DrawPacket>(selectionQueue.data(), selectionQueue.size()),
            isWireframe);

        // Pass 2: Deferred Lighting 
        renderBackend->submitLightingPass(cameraPos, viewMatrix, projMatrix);

        // Pass 3: The AAA Translucency Pipeline (includes per-layer glass depth)
        const std::span<const DrawPacket> transparentQueueView(transparentQueue.data(), transparentQueue.size());
        renderBackend->submitTransparentQueue(transparentQueueView);

        // Pass 4: ImGui/Editor UI
        renderBackend->submitUIPass();

        if (renderBackend->endFrame() == FrameStatus::RecreateSwapchain) {
            framebufferResized = false;
            recreateSwapchain();
        }
    }

    void Application::cleanup() {
        assetManager.reset();

        if (renderBackend && hdriMap.isValid()) {
            renderBackend->freeTexture(hdriMap);
            hdriMap = {};
        }

        if (renderBackend) {
            renderBackend->cleanup();
            renderBackend.reset();
        }

        glfwDestroyWindow(window);
        glfwTerminate();
    }

    // --- GLFW CALLBACK STUBS ---
    void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<Application*>(glfwGetWindowUserPointer(window));
        if (app) app->framebufferResized = true;
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

    void Application::ProcessMeshSwaps() {
        if (!assetManager) return;

        auto* meshPool = registry.getPool<MeshComponent>();
        if (!meshPool) return;

        for (uint32_t entity : meshPool->entities) {
            auto& meshComp = meshPool->get(entity);
            if (meshComp.requestedMeshPath.empty()) continue;

            const std::filesystem::path requestedPath(meshComp.requestedMeshPath);
            const std::filesystem::path modelPath = requestedPath.is_absolute()
                ? requestedPath
                : std::filesystem::path(PROJECT_ROOT_DIR) / requestedPath;

            try {
                meshComp.model = assetManager->getModel(modelPath.string());
                meshComp.requestedMeshPath.clear();
            }
            catch (const std::exception& error) {
                std::cerr << "Failed to load requested mesh '" << modelPath.string()
                    << "': " << error.what() << '\n';
                // Clear failed requests after one report; the caller must explicitly retry.
                meshComp.requestedMeshPath.clear();
            }
        }
    }

    void Application::recreateSwapchain() {
        if (renderBackend) {
            renderBackend->recreateSwapchain(window);
        }
    }
    void Application::selectEntityAtMouse(double mouseX, double mouseY) { /* ... */ }

} // namespace Iridium
