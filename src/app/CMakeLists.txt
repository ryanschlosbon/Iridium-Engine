# iridium_app: application frame loop and command-line configuration. Links every
# engine module; the IridiumEngine executable is main.cpp on top of it.
add_library(iridium_app STATIC
        Application.cpp
        ApplicationConfig.cpp
        cli/CliValueParsers.cpp
        cli/RuntimeOptions.cpp
        cli/EditorOptions.cpp
        cli/RendererOptions.cpp
        cli/QualificationOptions.cpp
)
target_link_libraries(iridium_app PUBLIC
        iridium_vulkan
        iridium_editor
        iridium_lighting
        iridium_scene
        iridium_assets
        iridium_rhi
        iridium_material
        iridium_color
        iridium_capture
        iridium_benchmarks
        iridium_platform
        iridium_profiling
        iridium_ecs
        iridium_core
        iridium_vendor_imgui
        Vulkan::Vulkan
        glfw
        nlohmann_json::nlohmann_json
        Threads::Threads)
# Application.h defines GLM_FORCE_DEPTH_ZERO_TO_ONE before its first GLM include,
# so this target's PCH must not contain GLM.
iridium_target_pch(iridium_app ENGINE NO_GLM)

# Build/source provenance printed into capture and run metadata. Only
# Application.cpp reads these, so only it recompiles when they change.
set_property(SOURCE Application.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        IRIDIUM_BUILD_CONFIGURATION="${CMAKE_BUILD_TYPE}"
        IRIDIUM_SOURCE_COMMIT="${IRIDIUM_SOURCE_COMMIT}"
        IRIDIUM_SOURCE_BRANCH="${IRIDIUM_SOURCE_BRANCH}"
        IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE=${IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE}
        IRIDIUM_COMPILER="${IRIDIUM_COMPILER_ID}"
        IRIDIUM_SHADER_COMPILER="${IRIDIUM_SHADER_COMPILER}"
        IRIDIUM_VULKAN_SDK="${IRIDIUM_VULKAN_SDK_VERSION}")

add_executable(IridiumEngine
        main.cpp
)
target_link_libraries(IridiumEngine PRIVATE iridium_app)
iridium_link_allocation_profile(IridiumEngine)
if(TARGET iridium_shaders)
    add_dependencies(IridiumEngine iridium_shaders)
endif()
