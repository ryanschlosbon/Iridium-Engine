# iridium_render_extraction: render extraction (M7R R5a.3). GPU-scene
# publication, light and reflection-probe extraction, main-view classification
# and draw-packet extraction, sorts, the shadow and probe-capture schedules and
# RenderFrame assembly. It reads the editor only through the EditorViewState
# value type; it must not reach ImGui, the editor or GLFW (configure-time check
# in tests/CMakeLists.txt).
add_library(iridium_render_extraction STATIC
        GpuSceneObservation.cpp
        ParallelDrawSort.cpp
        RenderExtractor.cpp
)
target_link_libraries(iridium_render_extraction PUBLIC
        iridium_lighting
        iridium_transparency
        iridium_scene
        iridium_assets
        iridium_rhi
        iridium_profiling
        iridium_ecs
        iridium_tasks
        iridium_core)
# RenderExtractor.cpp builds the scene projection with Vulkan [0, 1] clip depth
# (GLM_FORCE_DEPTH_ZERO_TO_ONE before its first GLM include), so this target's
# PCH must not contain GLM.
iridium_target_pch(iridium_render_extraction ENGINE NO_GLM)
