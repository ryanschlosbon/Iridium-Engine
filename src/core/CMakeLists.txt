# iridium_core: logging, identity/handle PODs (core/types), the command-line
# option registry (core/cli), build provenance (core/BuildInfo) and small
# utilities (src/utils). Bottom of the module DAG; publishes the `src` include
# root and GLM.

# Build/source provenance (cmake/IridiumSourceInfo.cmake, the shader and Vulkan
# SDK probes) is compiled into one generated translation unit, so a new commit
# recompiles only that file. Values are escaped as C string literals.
function(_iridium_build_info_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
_iridium_build_info_string(IRIDIUM_BUILD_INFO_CONFIGURATION "${CMAKE_BUILD_TYPE}")
_iridium_build_info_string(IRIDIUM_BUILD_INFO_SOURCE_COMMIT "${IRIDIUM_SOURCE_COMMIT}")
_iridium_build_info_string(IRIDIUM_BUILD_INFO_SOURCE_BRANCH "${IRIDIUM_SOURCE_BRANCH}")
_iridium_build_info_string(IRIDIUM_BUILD_INFO_COMPILER "${IRIDIUM_COMPILER_ID}")
_iridium_build_info_string(IRIDIUM_BUILD_INFO_SHADER_COMPILER "${IRIDIUM_SHADER_COMPILER}")
_iridium_build_info_string(IRIDIUM_BUILD_INFO_VULKAN_SDK "${IRIDIUM_VULKAN_SDK_VERSION}")
if(IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE)
    set(IRIDIUM_BUILD_INFO_SOURCE_DIRTY 1)
else()
    set(IRIDIUM_BUILD_INFO_SOURCE_DIRTY 0)
endif()
configure_file("${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cpp.in"
        "${CMAKE_BINARY_DIR}/generated/core/BuildInfo.cpp" @ONLY)

add_library(iridium_core STATIC
        EngineLog.cpp
        cli/CliOptionRegistry.cpp
        types/AssetGuid.cpp
        types/SceneEntityUuid.cpp
        ../utils/Sha256.cpp
        "${CMAKE_BINARY_DIR}/generated/core/BuildInfo.cpp"
)
target_include_directories(iridium_core PUBLIC ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(iridium_core PUBLIC glm::glm Threads::Threads)
if(WIN32)
    # AssetGuid/SceneEntityUuid generation and Sha256 use the CNG API.
    target_link_libraries(iridium_core PUBLIC Bcrypt)
endif()
iridium_target_pch(iridium_core ENGINE)
