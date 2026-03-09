# 1. VERSION CHECK
cmake_minimum_required(VERSION 3.20)

# 2. PROJECT METADATA
project(IridiumEngine VERSION 0.1 LANGUAGES CXX)

# 3. C++ STANDARD
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. OUTPUT DIRECTORY
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- DEPENDENCIES (FETCH CONTENT) ---
include(FetchContent)

# A. GLFW
message(STATUS "Downloading GLFW...")
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glfw
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# B. GLM
message(STATUS "Downloading GLM...")
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 0.9.9.8
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glm
)

FetchContent_GetProperties(glm)
if(NOT glm_POPULATED)
    FetchContent_Populate(glm)
    add_library(glm INTERFACE)
    target_include_directories(glm INTERFACE ${glm_SOURCE_DIR})
    add_library(glm::glm ALIAS glm)
endif()

# C. VULKAN
find_package(Vulkan REQUIRED)

# D. JSON
message(STATUS "Downloading nlohmann/json...")
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# --- PLATFORM DETECTION (MAC vs WINDOWS) ---
if (APPLE)
    message(STATUS "Detected MacOS - Linking Frameworks and MoltenVK")

    # macOS requires these frameworks for windowing and inputs
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)

    set(PLATFORM_LIBS ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK})

    # MoltenVK definition for Vulkan Portability
    add_compile_definitions(VK_USE_PLATFORM_MACOS_MVK)
elseif (WIN32)
    message(STATUS "Detected Windows")
    set(PLATFORM_LIBS)
endif()

# --- AUTOMATIC SHADER COMPILATION (Folder Scan) ---
find_program(GLSLC_COMPILER glslc
        HINTS "$ENV{VULKAN_SDK}/bin" "/usr/local/bin" "/opt/homebrew/bin"
        DOC "Path to the glslc shader compiler"
)

if (NOT GLSLC_COMPILER)
    message(WARNING "glslc not found! Shaders will NOT be compiled automatically.")
else()
    message(STATUS "Found Shader Compiler: ${GLSLC_COMPILER}")

    set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")

    # 1. Scan for all shader files automatically
    # GLOB_RECURSE allows you to put shaders in subfolders if you want (e.g. shaders/postfx/...)
    file(GLOB_RECURSE RAW_SHADER_FILES
            "${SHADER_SOURCE_DIR}/*.vert"
            "${SHADER_SOURCE_DIR}/*.frag"
            "${SHADER_SOURCE_DIR}/*.comp"
            "${SHADER_SOURCE_DIR}/*.geom"
            "${SHADER_SOURCE_DIR}/*.tesc"
            "${SHADER_SOURCE_DIR}/*.tese"
    )

    foreach(SOURCE_FILE ${RAW_SHADER_FILES})
        # 2. Generate Output Name
        # We need the filename to name the .spv (e.g. "shader.vert" -> "shader_vert.spv")
        get_filename_component(FILE_NAME ${SOURCE_FILE} NAME)

        string(REPLACE "." "_" SPV_BASE_NAME ${FILE_NAME})
        set(OUTPUT_FILE "${SHADER_SOURCE_DIR}/${SPV_BASE_NAME}.spv")

        # 3. Create Build Rule
        add_custom_command(
                OUTPUT ${OUTPUT_FILE}
                COMMAND ${GLSLC_COMPILER} ${SOURCE_FILE} -o ${OUTPUT_FILE}
                DEPENDS ${SOURCE_FILE} # Only recompile if source changes
                COMMENT "Compiling ${FILE_NAME}..."
                VERBATIM
        )

        list(APPEND SPV_FILES ${OUTPUT_FILE})
    endforeach()
endif()

# --- BUILD TARGET ---

# 6. THE EXECUTABLE
# Added ${SPV_FILES} so CMake knows to build shaders before the app
add_executable(IridiumEngine
        ${SPV_FILES}
        src/main.cpp
        "src/renderer/VkContext.h"
        "src/renderer/VkContext.cpp"
        "src/renderer/VkSwapchain.h"
        "src/renderer/VkSwapchain.cpp"
        "src/renderer/VkRenderPass.h"
        "src/renderer/VkRenderPass.cpp"
        "src/renderer/VkFramebuffer.h"
        "src/renderer/VkFramebuffer.cpp"
        "src/utils/File.h"
        "src/renderer/VkGraphicsPipeline.h"
        "src/renderer/VkGraphicsPipeline.cpp"
        "src/renderer/VkCommandManager.h"
        "src/renderer/VkCommandManager.cpp"
        "src/renderer/VkSyncObjects.h"
        "src/renderer/VkSyncObjects.cpp"
        "src/renderer/VkMesh.h"
        "src/vendor/std_image_impl.cpp"
        "src/assets/AssetManager.h"
        "src/assets/AssetManager.cpp"
        "src/ecs/Entity.h"
        "src/scene/Scene.h"
        "src/scene/Components.h"
        "src/scene/components/TransformComponent.h"
        "src/ecs/Registry.h"
        "src/scene/components/MeshComponent.h"
        "src/vendor/imgui/imgui.cpp"
        "src/vendor/imgui/imgui_demo.cpp"
        "src/vendor/imgui/imgui_draw.cpp"
        "src/vendor/imgui/imgui_tables.cpp"
        "src/vendor/imgui/imgui_widgets.cpp"
        "src/vendor/imgui/backends/imgui_impl_glfw.cpp"
        "src/vendor/imgui/backends/imgui_impl_vulkan.cpp"
        "src/editor/EditorSystem.h"
        "src/editor/EditorSystem.cpp"
        "src/ecs/systems/TransformSystem.cpp"
        "src/ecs/systems/TransformSystem.h"
        "src/scene/components/RelationshipComponent.h"
        "src/vendor/imguizmo/ImGuizmo.cpp"
        "src/vendor/simdjson/simdjson.cpp"
        "src/vendor/fastgltf/src/fastgltf.cpp"
        "src/vendor/fastgltf/src/base64.cpp"
        "src/vendor/fastgltf/src/io.cpp"
 "src/scene/SceneSerializer.h" "src/scene/SceneSerializer.cpp" "src/scene/components/LightComponent.h" "src/editor/Reflection.h" "src/utils/DeletionQueue.h" "src/core/Application.h" "src/core/Application.cpp" "src/editor/panels/EditorPanel.h" "src/editor/panels/core/SceneHierarchyPanel.h" "src/editor/panels/core/SceneHierarchyPanel.cpp" "src/editor/panels/core/InspectorPanel.h" "src/editor/panels/core/InspectorPanel.cpp"     "src/editor/EditorUIState.h" "src/editor/panels/menus/MenuBarPanel.h" "src/editor/panels/menus/MenuBarPanel.cpp" "src/editor/panels/windows/ProjectSettingsPanel.h" "src/editor/panels/windows/ProjectSettingsPanel.cpp" "src/renderer/DescriptorAllocator.h" "src/renderer/DescriptorAllocator.cpp" "src/renderer/VkUIRenderPass.h" "src/renderer/VkUIRenderPass.cpp" "src/editor/panels/core/ViewPortPanel.h" "src/editor/panels/core/ViewPortPanel.cpp" "src/renderer/VkLightingPipeline.h" "src/renderer/VkLightingPipeline.cpp" "src/renderer/VkForwardRenderPass.cpp" "src/renderer/VkForwardRenderPass.h" "src/renderer/VkForwardPipeline.h" "src/renderer/VkForwardPipeline.cpp" "src/renderer/GlassDepthRenderPass.h" "src/renderer/GlassDepthRenderPass.cpp" "src/renderer/GlassDepthPipeline.h" "src/renderer/GlassDepthPipeline.cpp" "src/scene/ComponentRegistry.h" "src/scene/JsonArchive.h" "src/editor/ImGuiArchive.h" "src/utils/FileDialogs.h" "src/utils/FileDialogs.cpp")

# Defines PROJECT_ROOT_DIR for C++ to find assets
add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")

# 7. INCLUDE PATHS
target_include_directories(IridiumEngine PRIVATE src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/vendor/simdjson
        ${CMAKE_SOURCE_DIR}/src/vendor/imgui
        ${CMAKE_SOURCE_DIR}/src/vendor/imgui/backends
)

# 8. LINKING
target_link_libraries(IridiumEngine PRIVATE
        glfw
        glm::glm
        Vulkan::Vulkan
        nlohmann_json::nlohmann_json
        ${PLATFORM_LIBS} # Links Cocoa/IOKit on Mac, nothing on Windows
)