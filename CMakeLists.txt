# 1. VERSION CHECK
# Ensures the user has a modern version of CMake (3.20+) installed.
# We need 3.20+ for the 'FetchContent' feature to work smoothly.
cmake_minimum_required(VERSION 3.20)

# 2. PROJECT METADATA
# Sets the solution name to "IridiumEngine". 
# Also tells CMake we are using C++ (CXX).
project(IridiumEngine VERSION 0.1 LANGUAGES CXX)

# 3. C++ STANDARD
# We enforce C++20. This allows us to use modern features like concepts and modules.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. OUTPUT DIRECTORY
# By default, CMake builds .exe files deep inside messy folders like "build/Debug/".
# This line forces the .exe to go to "build/bin/" so it's easy to find.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- DEPENDENCIES ---

# 5. FETCH CONTENT (The Magic Tool)
# This module allows CMake to download libraries from GitHub automatically.
include(FetchContent)

# A. GLFW (Windowing)
message(STATUS "Downloading GLFW...")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glfw
)
# These flags stop GLFW from building its own docs/examples, which speeds up our build.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Actually download and build GLFW now.
FetchContent_MakeAvailable(glfw)

# B. GLM (Math)
message(STATUS "Downloading GLM...")

# 1. Suppress the "Deprecated" warning for manual population
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

    # 2. Create the library manually
    add_library(glm INTERFACE)
    target_include_directories(glm INTERFACE ${glm_SOURCE_DIR})

    # 3. CRITICAL FIX: Create the Alias "glm::glm"
    # This maps the name "glm::glm" to our manual "glm" library.
    add_library(glm::glm ALIAS glm)
endif()

# C. VULKAN (Graphics Driver)
# We don't download Vulkan (it's a driver). We look for the SDK installed on your PC.
# If you didn't install the LunarG SDK, this line will fail.
find_package(Vulkan REQUIRED)

# --- BUILD TARGET ---

# 6. THE EXECUTABLE
# This tells CMake: "Take src/main.cpp and turn it into Iridium.exe"
add_executable(IridiumEngine 
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
    "src/scene/Entity.h" 
    "src/scene/Scene.h" 
    "src/scene/Components.h" 
    "src/scene/components/TransformComponent.h" 
    "src/scene/Registry.h" 
    "src/scene/components/MeshComponent.h"
    "src/vendor/imgui/imgui.cpp"
    "src/vendor/imgui/imgui_demo.cpp"
    "src/vendor/imgui/imgui_draw.cpp"
    "src/vendor/imgui/imgui_tables.cpp"
    "src/vendor/imgui/imgui_widgets.cpp"
    "src/vendor/imgui/backends/imgui_impl_glfw.cpp"
    "src/vendor/imgui/backends/imgui_impl_vulkan.cpp" 
    "src/editor/EditorSystem.h" 
    "src/editor/EditorSystem.cpp")

add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")

# 7. INCLUDE PATHS
# Allows your code to say #include "core/Window.h" instead of "../src/core/Window.h"
target_include_directories(IridiumEngine PRIVATE src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/vendor/simdjson
    ${CMAKE_SOURCE_DIR}/src/vendor/imgui
    ${CMAKE_SOURCE_DIR}/src/vendor/imgui/backends
)

# 8. LINKING
# This connects your code to the libraries we downloaded.
# If you miss this, you get "Unresolved External Symbol" errors.
target_link_libraries(IridiumEngine PRIVATE
    glfw            # The Window library
    glm::glm        # The Math library
    Vulkan::Vulkan  # The Graphics Driver
)

target_sources(IridiumEngine PRIVATE 
    src/vendor/simdjson/simdjson.cpp
    src/vendor/fastgltf/src/fastgltf.cpp
    src/vendor/fastgltf/src/base64.cpp
    src/vendor/fastgltf/src/io.cpp
    src/vendor/std_image_impl.cpp 
    src/vendor/imgui/imgui.cpp
    src/vendor/imgui/imgui_demo.cpp
    src/vendor/imgui/imgui_draw.cpp
    src/vendor/imgui/imgui_tables.cpp
    src/vendor/imgui/imgui_widgets.cpp
    src/vendor/imgui/backends/imgui_impl_glfw.cpp
    src/vendor/imgui/backends/imgui_impl_vulkan.cpp
    src/editor/EditorSystem.cpp)