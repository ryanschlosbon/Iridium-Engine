# iridium_core: logging, identity/handle PODs (core/types) and small utilities
# (src/utils). Bottom of the module DAG; publishes the `src` include root and GLM.
add_library(iridium_core STATIC
        EngineLog.cpp
        types/AssetGuid.cpp
        types/SceneEntityUuid.cpp
        ../utils/Sha256.cpp
)
target_include_directories(iridium_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(iridium_core PUBLIC glm::glm Threads::Threads)
if(WIN32)
    # AssetGuid/SceneEntityUuid generation and Sha256 use the CNG API.
    target_link_libraries(iridium_core PUBLIC Bcrypt)
endif()
iridium_target_pch(iridium_core ENGINE)
