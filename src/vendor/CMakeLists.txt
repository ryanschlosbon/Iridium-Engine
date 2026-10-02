# Vendored third-party code, compiled once per configuration (no PCH, warnings off).
# The sources in these directories are unmodified upstream files.

# Dear ImGui core, the GLFW/Vulkan backends, and ImGuizmo.
iridium_vendor_library(iridium_vendor_imgui
        imgui/imgui.cpp
        imgui/imgui_demo.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/backends/imgui_impl_glfw.cpp
        imgui/backends/imgui_impl_vulkan.cpp
        imguizmo/ImGuizmo.cpp
)
target_include_directories(iridium_vendor_imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
)
target_link_libraries(iridium_vendor_imgui PUBLIC glfw Vulkan::Vulkan)

# fastgltf (headers under include/fastgltf) and its simdjson dependency.
iridium_vendor_library(iridium_vendor_gltf
        fastgltf/src/fastgltf.cpp
        fastgltf/src/base64.cpp
        fastgltf/src/io.cpp
        simdjson/simdjson.cpp
)
target_include_directories(iridium_vendor_gltf PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/simdjson
)

# stb_image implementation (header under include/).
iridium_vendor_library(iridium_vendor_stb
        std_image_impl.cpp
)
target_include_directories(iridium_vendor_stb PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
