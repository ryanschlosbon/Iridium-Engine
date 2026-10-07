# iridium_capture: capture image formats, artifacts and comparison.
add_library(iridium_capture STATIC
        TgaImage.cpp
        PfmImage.cpp
        CaptureArtifact.cpp
        ImageComparison.cpp
)
target_link_libraries(iridium_capture PUBLIC iridium_core nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_capture ENGINE)
