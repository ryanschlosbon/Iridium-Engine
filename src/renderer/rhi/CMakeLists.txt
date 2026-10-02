# iridium_rhi: backend-neutral render types and CPU-side GPU-scene planning.
# renderer/transparency (header-only) and renderer/scene belong to this library:
# rhi and transparency still include each other until M7R R2.
add_library(iridium_rhi STATIC
        DepthPyramid.cpp
        GeometryArena.cpp
        GpuScene.cpp
        GpuSceneIndirect.cpp
        GpuSceneLod.cpp
        GpuSceneVisibility.cpp
        ReflectionProbeCapture.cpp
        TextureResidency.cpp
        VirtualShadowMap.cpp
        ../scene/GpuScenePublisher.cpp
)
target_link_libraries(iridium_rhi PUBLIC
        iridium_material
        iridium_color
        iridium_graph
        iridium_core)
iridium_target_pch(iridium_rhi ENGINE)
