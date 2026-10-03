# Third-party dependencies. Tag-pinned repositories are cloned shallowly; the
# commit-pinned DirectXTex clone stays a full clone because a shallow clone cannot
# check out an arbitrary commit.
include(FetchContent)

# A. GLFW
message(STATUS "Downloading GLFW...")
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
        GIT_SHALLOW TRUE
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glfw
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# B. GLM (header-only). Its 0.9.9.8 CMakeLists declares cmake_minimum_required(3.2),
# which CMake 4 rejects without a policy floor (3.10 also avoids the <3.10
# deprecation warning). It only defines the INTERFACE
# target glm (alias glm::glm); tests are added only when GLM is the top-level
# project, and static/shared libraries only with BUILD_STATIC_LIBS/BUILD_SHARED_LIBS.
message(STATUS "Downloading GLM...")
FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 0.9.9.8
        GIT_SHALLOW TRUE
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glm
)
set(_iridium_saved_policy_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(glm)
set(CMAKE_POLICY_VERSION_MINIMUM "${_iridium_saved_policy_minimum}")
unset(_iridium_saved_policy_minimum)

# C. VULKAN
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
set(IRIDIUM_VULKAN_SDK_VERSION "${Vulkan_VERSION}")
if(IRIDIUM_VULKAN_SDK_VERSION STREQUAL "")
    set(IRIDIUM_VULKAN_SDK_VERSION "unavailable")
endif()

# C2. Vulkan Memory Allocator (MIT, header-only; M7R R4b). Only the INTERFACE
# target GPUOpen::VulkanMemoryAllocator is used: install, and with it the samples
# and documentation options, stay off. iridium_vulkan compiles the implementation
# in one translation unit (VulkanMemoryAllocatorImpl.cpp). SYSTEM (CMake 3.25+)
# keeps its header's warnings out of engine builds. The short dependency name keeps
# the FetchContent stamp paths under MAX_PATH in nested worktrees.
message(STATUS "Downloading Vulkan Memory Allocator...")
set(_iridium_vma_system)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    set(_iridium_vma_system SYSTEM)
endif()
FetchContent_Declare(
        vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
        ${_iridium_vma_system}
)
unset(_iridium_vma_system)
set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vma)

# D. JSON
message(STATUS "Downloading nlohmann/json...")
FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json)

# E. M3 production texture codec. CPU-only configuration is part of the
# deterministic cook contract; GPU compression and OpenMP are deliberately disabled.
if(WIN32)
    set(BUILD_TOOLS OFF CACHE BOOL "Build DirectXTex command-line tools" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "Build DirectXTex samples" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "Build DirectXTex Direct3D 11 support" FORCE)
    set(BUILD_DX12 OFF CACHE BOOL "Build DirectXTex Direct3D 12 support" FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL "Use OpenMP in DirectXTex" FORCE)
    FetchContent_Declare(
            directxtex
            GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
            GIT_TAG 4feb3e11a020f35b796fc769a74216a555d4f5ef
    )
    FetchContent_MakeAvailable(directxtex)
endif()

# --- PLATFORM DETECTION (MAC vs WINDOWS) ---
if(APPLE)
    message(STATUS "Detected MacOS - Linking Frameworks and MoltenVK")

    # macOS requires these frameworks for windowing and inputs
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)

    set(PLATFORM_LIBS ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK})

    # MoltenVK definition for Vulkan Portability
    add_compile_definitions(VK_USE_PLATFORM_MACOS_MVK)
elseif(WIN32)
    message(STATUS "Detected Windows")
    set(PLATFORM_LIBS Comdlg32)
endif()
