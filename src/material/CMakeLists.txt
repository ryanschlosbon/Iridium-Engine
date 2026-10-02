# iridium_material: source materials, compiler, runtime tables and shading models.
add_library(iridium_material STATIC
        SourceMaterial.cpp
        MaterialCompiler.cpp
        MaterialRuntime.cpp
        MaterialTextureCompatibility.cpp
)
target_link_libraries(iridium_material PUBLIC
        iridium_color
        iridium_core
        nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_material ENGINE)
