# Engine modules. Each directory owns one library; dependencies point downward:
#
#   iridium_core          core/** (EngineLog, ProjectAssetRoots, types/*, cli/*, generated BuildInfo), utils/* -> glm
#   iridium_profiling     profiling/* (except CpuAllocationProfile)       -> core
#   iridium_allocation_profile (OBJECT) profiling/CpuAllocationProfile    -> core
#   iridium_platform      platform/*                                       -> core, glfw
#   iridium_ecs           ecs/* (header-only, INTERFACE)                   -> core
#   iridium_color         renderer/color                                   -> core
#   iridium_graph         renderer/graph                                   -> core
#   iridium_capture       capture/*                                        -> core
#   iridium_benchmarks    benchmarks/*                                     -> core
#   iridium_material      material/*                                       -> color, core
#   iridium_rhi           renderer/rhi + renderer/scene                    -> material, color, graph, core
#   iridium_transparency  renderer/transparency (header-only, INTERFACE)   -> rhi
#   iridium_assets        assets/**                                        -> rhi, material, color, core, vendor
#   iridium_scene         scene/** except authoring (JSON-free runtime)     -> ecs, assets, rhi, core
#   iridium_scene_authoring scene/authoring (JSON source scenes)            -> scene, nlohmann_json
#   iridium_lighting      renderer/lighting                                -> scene, rhi
#   iridium_vulkan        renderer/vulkan                                  -> lighting, transparency, rhi, graph, profiling, color, Vulkan, glfw
#   iridium_vulkan_imgui  renderer/vulkan_imgui (IEditorRenderBridge)      -> vulkan, imgui, glfw
#   iridium_render_extraction extraction/* (M7R R5a.3)                     -> lighting, transparency, scene, assets, rhi, profiling (no imgui, editor or glfw)
#   iridium_editor        editor/**                                        -> scene, assets, rhi, platform, profiling, imgui
#   iridium_app           app/* except main.cpp (runtime/editor/renderer CLI) -> everything above except capture, benchmarks
#   IridiumEngine         app/main.cpp                                     -> app, allocation profile, shaders (+ qualification when ON)
#   iridium_qualification qualification/** (IRIDIUM_QUALIFICATION=ON only)   -> app, vulkan, transparency, capture, benchmarks
#                         harness, Vulkan extension, result types and the qualification CLI flags
#
# Vendor code (src/vendor) is compiled once into iridium_vendor_imgui,
# iridium_vendor_gltf and iridium_vendor_stb.

add_subdirectory(vendor)
add_subdirectory(core)
add_subdirectory(profiling)
add_subdirectory(platform)
add_subdirectory(ecs)
add_subdirectory(capture)
add_subdirectory(benchmarks)
add_subdirectory(material)
add_subdirectory(renderer)
add_subdirectory(assets)
add_subdirectory(scene)
add_subdirectory(extraction)
add_subdirectory(editor)
add_subdirectory(app)
add_subdirectory(qualification)
