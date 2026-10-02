# iridium_qualification: the M7R R2 qualification harness. It owns oracles,
# readback validation, frame captures, validators, deterministic benchmark scenes,
# run/profile reports and their command-line flags, attached to the app through
# IFrameObserver and to the Vulkan backend through the extension the harness
# supplies (IFrameObserver::backendExtensions). Built only when
# IRIDIUM_QUALIFICATION=ON; production code never depends on it.
if(NOT IRIDIUM_QUALIFICATION)
    return()
endif()

add_library(iridium_qualification STATIC
        QualificationBuild.cpp
        QualificationOptions.cpp
)
target_link_libraries(iridium_qualification PUBLIC
        iridium_app
        iridium_vulkan
        iridium_transparency
        iridium_capture
        iridium_benchmarks
        iridium_profiling
        iridium_core)
iridium_target_pch(iridium_qualification ENGINE NO_GLM)

# App-side harness (IFrameObserver implementation) and Vulkan-side extension
# sources are listed in their own subdirectories.
add_subdirectory(harness)
add_subdirectory(vulkan)

target_link_libraries(IridiumEngine PRIVATE iridium_qualification)
