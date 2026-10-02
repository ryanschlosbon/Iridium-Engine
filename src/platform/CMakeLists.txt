# iridium_platform: native file dialogs and system profile queries.
add_library(iridium_platform STATIC
        SystemProfile.cpp
)
if(APPLE)
    enable_language(OBJCXX)
    target_sources(iridium_platform PRIVATE FileDialogMac.mm)
else()
    target_sources(iridium_platform PRIVATE FileDialog.cpp)
endif()
target_link_libraries(iridium_platform PUBLIC iridium_core glfw ${PLATFORM_LIBS})
iridium_target_pch(iridium_platform ENGINE)
