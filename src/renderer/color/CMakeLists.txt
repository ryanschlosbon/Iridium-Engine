# iridium_color: scene-linear color spaces and the output transform.
add_library(iridium_color STATIC
        SceneColor.cpp
        OutputTransformConfig.cpp
        AcesOutputLut.cpp
)
target_link_libraries(iridium_color PUBLIC iridium_core)
iridium_target_pch(iridium_color ENGINE)
