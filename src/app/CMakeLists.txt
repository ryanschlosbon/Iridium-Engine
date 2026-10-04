# iridium_app: application frame loop and the runtime/editor/renderer command
# line. The IridiumEngine executable is main.cpp on top of it; qualification
# (capture, benchmarks, validators, oracles and their flags) is linked only in
# IRIDIUM_QUALIFICATION=ON builds (src/qualification).
add_library(iridium_app STATIC
        Application.cpp
        ApplicationConfig.cpp
        AssetIntegration.cpp
        ../extraction/RenderExtractor.cpp
        cli/CliValueParsers.cpp
        cli/RuntimeOptions.cpp
        cli/EditorOptions.cpp
        cli/RendererOptions.cpp
)
target_link_libraries(iridium_app PUBLIC
        iridium_vulkan
        iridium_vulkan_imgui
        iridium_editor
        iridium_lighting
        iridium_scene
        iridium_assets
        iridium_transparency
        iridium_rhi
        iridium_material
        iridium_color
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

add_executable(IridiumEngine
        main.cpp
)
target_link_libraries(IridiumEngine PRIVATE iridium_app)
iridium_link_allocation_profile(IridiumEngine)
if(TARGET iridium_shaders)
    add_dependencies(IridiumEngine iridium_shaders)
endif()
