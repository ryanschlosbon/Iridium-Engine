# GLSL -> SPIR-V compilation (target: iridium_shaders).
#
# Every *.vert/*.frag/*.comp/... under assets/shaders becomes assets/shaders/
# <name>_<stage>.spv; the runtime loads that relative path, so the output stays in
# the source tree for now. glslc writes a Make-style depfile per shader (-MD -MF),
# so editing a .glsl include rebuilds only the shaders that include it. The glob
# uses CONFIGURE_DEPENDS so added or removed shaders re-run configure.
#
# Exports to the including scope: GLSLC_COMPILER, IRIDIUM_SHADER_COMPILER,
# IRIDIUM_SHADER_SPV_FILES and the iridium_shaders target (when glslc exists).

find_program(GLSLC_COMPILER glslc
        HINTS "$ENV{VULKAN_SDK}/bin" "/usr/local/bin" "/opt/homebrew/bin"
        DOC "Path to the glslc shader compiler"
)

set(IRIDIUM_SHADER_COMPILER "unavailable")
set(IRIDIUM_SHADER_SPV_FILES)

if(NOT GLSLC_COMPILER)
    message(WARNING "glslc not found! Shaders will NOT be compiled automatically.")
    return()
endif()

message(STATUS "Found Shader Compiler: ${GLSLC_COMPILER}")

execute_process(
        COMMAND "${GLSLC_COMPILER}" --version
        OUTPUT_VARIABLE GLSLC_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
string(REGEX MATCH "^[^\r\n]*" IRIDIUM_SHADER_COMPILER "${GLSLC_VERSION_OUTPUT}")
if(IRIDIUM_SHADER_COMPILER STREQUAL "")
    set(IRIDIUM_SHADER_COMPILER "glslc (version unavailable)")
endif()

set(IRIDIUM_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
set(IRIDIUM_SHADER_DEPFILE_DIR "${CMAKE_CURRENT_BINARY_DIR}/shader-deps")
file(MAKE_DIRECTORY "${IRIDIUM_SHADER_DEPFILE_DIR}")

file(GLOB_RECURSE IRIDIUM_SHADER_SOURCES CONFIGURE_DEPENDS
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.vert"
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.frag"
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.comp"
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.geom"
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.tesc"
        "${IRIDIUM_SHADER_SOURCE_DIR}/*.tese"
)

# iridium_compile_shader(<source> <output .spv> [glslc options...])
function(iridium_compile_shader source output)
    get_filename_component(_output_name "${output}" NAME)
    get_filename_component(_source_name "${source}" NAME)
    set(_depfile "${IRIDIUM_SHADER_DEPFILE_DIR}/${_output_name}.d")
    add_custom_command(
            OUTPUT "${output}"
            COMMAND "${GLSLC_COMPILER}" ${ARGN}
                    -I "${IRIDIUM_SHADER_SOURCE_DIR}" "${source}" -o "${output}"
                    -MD -MF "${_depfile}"
            DEPENDS "${source}"
            DEPFILE "${_depfile}"
            COMMENT "Compiling ${_source_name} -> ${_output_name}"
            VERBATIM
    )
    set_property(GLOBAL APPEND PROPERTY IRIDIUM_SHADER_SPV_FILES "${output}")
endfunction()

foreach(_shader_source IN LISTS IRIDIUM_SHADER_SOURCES)
    # "shader.vert" -> "shader_vert.spv"
    get_filename_component(_shader_name "${_shader_source}" NAME)
    string(REPLACE "." "_" _spv_base_name "${_shader_name}")
    set(_shader_options)
    if(_shader_name STREQUAL "cluster_sort.comp" OR
            _shader_name STREQUAL "cluster_sort_prepare.comp")
        # Subgroup shuffles require SPIR-V 1.3; the renderer already creates
        # a Vulkan 1.3 device, while older shaders retain their frozen target.
        list(APPEND _shader_options --target-env=vulkan1.1)
    endif()
    iridium_compile_shader("${_shader_source}"
            "${IRIDIUM_SHADER_SOURCE_DIR}/${_spv_base_name}.spv"
            ${_shader_options})
endforeach()

# M7.6 qualified Hi-Z compaction uses the same source contract with one
# additional read-only result binding. Keep the default shader unchanged.
iridium_compile_shader("${IRIDIUM_SHADER_SOURCE_DIR}/gpu_scene_frustum_compact.comp"
        "${IRIDIUM_SHADER_SOURCE_DIR}/gpu_scene_frustum_occlusion_compact_comp.spv"
        -DIRIDIUM_GPU_SCENE_OCCLUSION=1)

get_property(IRIDIUM_SHADER_SPV_FILES GLOBAL PROPERTY IRIDIUM_SHADER_SPV_FILES)
add_custom_target(iridium_shaders ALL DEPENDS ${IRIDIUM_SHADER_SPV_FILES})
