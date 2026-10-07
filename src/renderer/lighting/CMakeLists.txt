# iridium_lighting: light extraction, clustered lighting, shadows and probes.
add_library(iridium_lighting STATIC
        LightExtractor.cpp
        ClusteredLighting.cpp
        DirectionalShadow.cpp
        LocalShadow.cpp
        ReflectionProbe.cpp
        ClusteredReflectionProbes.cpp
)
target_link_libraries(iridium_lighting PUBLIC iridium_scene iridium_rhi)
iridium_target_pch(iridium_lighting ENGINE)
