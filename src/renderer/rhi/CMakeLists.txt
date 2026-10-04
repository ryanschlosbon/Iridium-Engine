# iridium_rhi: backend-neutral render types and CPU-side GPU-scene planning.
# renderer/scene (GPU-scene publication) belongs to this library; the header-only
# renderer/transparency is iridium_transparency, which depends on it.
add_library(iridium_rhi STATIC
        CompactDrawSort.cpp
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
