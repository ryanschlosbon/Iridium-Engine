# 1. VERSION CHECK
cmake_minimum_required(VERSION 3.20)

# 2. PROJECT METADATA
project(IridiumEngine VERSION 0.1 LANGUAGES CXX)

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )
endif()

find_package(Git QUIET)
set(IRIDIUM_SOURCE_COMMIT "unknown")
set(IRIDIUM_SOURCE_BRANCH "unknown")
set(IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE 0)
if(Git_FOUND)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --verify HEAD
            OUTPUT_VARIABLE IRIDIUM_SOURCE_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE IRIDIUM_SOURCE_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain
            OUTPUT_VARIABLE IRIDIUM_SOURCE_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    if(NOT IRIDIUM_SOURCE_STATUS STREQUAL "")
        set(IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE 1)
    endif()
endif()

# 3. C++ STANDARD
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 4. OUTPUT DIRECTORY
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- DEPENDENCIES (FETCH CONTENT) ---
include(FetchContent)

# A. GLFW
message(STATUS "Downloading GLFW...")
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glfw
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# B. GLM
message(STATUS "Downloading GLM...")
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 0.9.9.8
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/glm
)

FetchContent_GetProperties(glm)
if(NOT glm_POPULATED)
    FetchContent_Populate(glm)
    add_library(glm INTERFACE)
    target_include_directories(glm INTERFACE ${glm_SOURCE_DIR})
    add_library(glm::glm ALIAS glm)
endif()

# C. VULKAN
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
set(IRIDIUM_VULKAN_SDK_VERSION "${Vulkan_VERSION}")
if(IRIDIUM_VULKAN_SDK_VERSION STREQUAL "")
    set(IRIDIUM_VULKAN_SDK_VERSION "unavailable")
endif()

# D. JSON
message(STATUS "Downloading nlohmann/json...")
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# E. M3 production texture codec. CPU-only configuration is part of the
# deterministic cook contract; GPU compression and OpenMP are deliberately disabled.
if(WIN32)
    set(BUILD_TOOLS OFF CACHE BOOL "Build DirectXTex command-line tools" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "Build DirectXTex samples" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "Build DirectXTex Direct3D 11 support" FORCE)
    set(BUILD_DX12 OFF CACHE BOOL "Build DirectXTex Direct3D 12 support" FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL "Use OpenMP in DirectXTex" FORCE)
    FetchContent_Declare(
        directxtex
        GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
        GIT_TAG 4feb3e11a020f35b796fc769a74216a555d4f5ef
    )
    FetchContent_MakeAvailable(directxtex)
endif()

# --- PLATFORM DETECTION (MAC vs WINDOWS) ---
if (APPLE)
    message(STATUS "Detected MacOS - Linking Frameworks and MoltenVK")

    # macOS requires these frameworks for windowing and inputs
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)

    set(PLATFORM_LIBS ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK})

    # MoltenVK definition for Vulkan Portability
    add_compile_definitions(VK_USE_PLATFORM_MACOS_MVK)
elseif (WIN32)
    message(STATUS "Detected Windows")
    set(PLATFORM_LIBS Comdlg32)
endif()

# --- AUTOMATIC SHADER COMPILATION (Folder Scan) ---
find_program(GLSLC_COMPILER glslc
        HINTS "$ENV{VULKAN_SDK}/bin" "/usr/local/bin" "/opt/homebrew/bin"
        DOC "Path to the glslc shader compiler"
)

set(IRIDIUM_SHADER_COMPILER "unavailable")

if (NOT GLSLC_COMPILER)
    message(WARNING "glslc not found! Shaders will NOT be compiled automatically.")
else()
    message(STATUS "Found Shader Compiler: ${GLSLC_COMPILER}")

    execute_process(
            COMMAND "${GLSLC_COMPILER}" --version
            OUTPUT_VARIABLE GLSLC_VERSION_OUTPUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    string(REGEX MATCH "^[^\r\n]*" IRIDIUM_SHADER_COMPILER
            "${GLSLC_VERSION_OUTPUT}")
    if(IRIDIUM_SHADER_COMPILER STREQUAL "")
        set(IRIDIUM_SHADER_COMPILER "glslc (version unavailable)")
    endif()

    set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")

    # 1. Scan for all shader files automatically
    # GLOB_RECURSE allows you to put shaders in subfolders if you want (e.g. shaders/postfx/...)
    file(GLOB_RECURSE RAW_SHADER_FILES
            "${SHADER_SOURCE_DIR}/*.vert"
            "${SHADER_SOURCE_DIR}/*.frag"
            "${SHADER_SOURCE_DIR}/*.comp"
            "${SHADER_SOURCE_DIR}/*.geom"
            "${SHADER_SOURCE_DIR}/*.tesc"
            "${SHADER_SOURCE_DIR}/*.tese"
    )
    file(GLOB_RECURSE SHADER_INCLUDE_FILES
            "${SHADER_SOURCE_DIR}/*.glsl"
    )

    foreach(SOURCE_FILE ${RAW_SHADER_FILES})
        # 2. Generate Output Name
        # We need the filename to name the .spv (e.g. "shader.vert" -> "shader_vert.spv")
        get_filename_component(FILE_NAME ${SOURCE_FILE} NAME)

        string(REPLACE "." "_" SPV_BASE_NAME ${FILE_NAME})
        set(OUTPUT_FILE "${SHADER_SOURCE_DIR}/${SPV_BASE_NAME}.spv")
        set(SHADER_COMPILE_OPTIONS)
        if(FILE_NAME STREQUAL "cluster_sort.comp" OR
                FILE_NAME STREQUAL "cluster_sort_prepare.comp")
            # Subgroup shuffles require SPIR-V 1.3; the renderer already creates
            # a Vulkan 1.3 device, while older shaders retain their frozen target.
            list(APPEND SHADER_COMPILE_OPTIONS --target-env=vulkan1.1)
        endif()

        # 3. Create Build Rule
        add_custom_command(
                OUTPUT ${OUTPUT_FILE}
                COMMAND ${GLSLC_COMPILER} ${SHADER_COMPILE_OPTIONS}
                        -I "${SHADER_SOURCE_DIR}" ${SOURCE_FILE} -o ${OUTPUT_FILE}
                DEPENDS ${SOURCE_FILE} ${SHADER_INCLUDE_FILES}
                COMMENT "Compiling ${FILE_NAME}..."
                VERBATIM
        )

        list(APPEND SPV_FILES ${OUTPUT_FILE})
    endforeach()
endif()

# --- BUILD TARGET ---

# M1 backend-neutral render-graph module. It remains unused by the renderer until
# the Vulkan shadow executor slice.
add_library(IridiumRenderGraph STATIC
        "src/renderer/graph/RenderGraph.h"
        "src/renderer/graph/RenderGraph.cpp"
)
target_include_directories(IridiumRenderGraph PUBLIC src)

# M3/M4 backend-neutral cooked artifact container. Keeping this separate from the
# catalog/importer library lets production scene loading validate artifacts without
# acquiring JSON, SQLite, glTF, or editor dependencies.
add_library(IridiumCookedArtifact STATIC
        "src/assets/AssetGuid.h"
        "src/assets/AssetGuid.cpp"
        "src/assets/cooker/CookTypes.h"
        "src/assets/cooker/CookTypes.cpp"
        "src/assets/cooker/CookedArtifact.h"
        "src/assets/cooker/CookedArtifact.cpp"
        "src/assets/cooker/CookKey.h"
        "src/assets/cooker/CookKey.cpp"
        "src/utils/Sha256.h"
        "src/utils/Sha256.cpp"
)
target_include_directories(IridiumCookedArtifact PUBLIC src)
if(WIN32)
    target_link_libraries(IridiumCookedArtifact PUBLIC Bcrypt)
endif()

# M4 JSON/editor-free runtime component identity and registry. Source scene codecs
# live in a separate authoring target so production runtime metadata cannot acquire
# a source JSON dependency through transitive linkage.
add_library(IridiumSceneRuntime STATIC
        "src/scene/SceneEntityUuid.h"
        "src/scene/SceneEntityUuid.cpp"
        "src/scene/SceneIdentityMap.h"
        "src/scene/SceneIdentityMap.cpp"
        "src/scene/SceneWorld.h"
        "src/scene/SceneWorld.cpp"
        "src/scene/runtime/ComponentIdentity.h"
        "src/scene/runtime/ComponentIdentity.cpp"
        "src/scene/runtime/RuntimeComponentRegistry.h"
        "src/scene/runtime/RuntimeComponentRegistry.cpp"
        "src/scene/runtime/CookedComponentIO.h"
        "src/scene/runtime/CookedComponentIO.cpp"
        "src/scene/runtime/CookedScene.h"
        "src/scene/runtime/CookedScene.cpp"
        "src/scene/runtime/CoreComponentIds.h"
        "src/scene/runtime/CoreComponentRegistry.h"
        "src/scene/runtime/CoreComponentRegistry.cpp"
        "src/scene/runtime/SceneDiagnostic.h"
        "src/scene/runtime/SceneDiagnostic.cpp"
        "src/scene/runtime/SceneReferenceState.h"
        "src/scene/runtime/SceneReferenceState.cpp"
)
target_include_directories(IridiumSceneRuntime PUBLIC src)
target_link_libraries(IridiumSceneRuntime PUBLIC IridiumCookedArtifact)

add_library(IridiumLighting STATIC
        "src/renderer/rhi/LightingTypes.h"
        "src/renderer/rhi/ReflectionProbeTypes.h"
        "src/renderer/rhi/LightUploadPlanner.h"
        "src/renderer/lighting/LightExtractor.h"
        "src/renderer/lighting/LightExtractor.cpp"
        "src/renderer/lighting/ClusteredLighting.h"
        "src/renderer/lighting/ClusteredLighting.cpp"
        "src/renderer/lighting/DirectionalShadow.h"
        "src/renderer/lighting/DirectionalShadow.cpp"
        "src/renderer/lighting/LocalShadow.h"
        "src/renderer/lighting/LocalShadow.cpp"
        "src/renderer/lighting/ReflectionProbe.h"
        "src/renderer/lighting/ReflectionProbe.cpp"
        "src/renderer/lighting/ReflectionProbeCapture.h"
        "src/renderer/lighting/ReflectionProbeCapture.cpp"
        "src/renderer/lighting/ClusteredReflectionProbes.h"
        "src/renderer/lighting/ClusteredReflectionProbes.cpp"
)
target_include_directories(IridiumLighting PUBLIC src)
target_link_libraries(IridiumLighting PUBLIC
        IridiumSceneRuntime
        glm::glm)

add_library(IridiumSceneAuthoring STATIC
        "src/scene/authoring/SourceComponentRegistry.h"
        "src/scene/authoring/SourceComponentRegistry.cpp"
        "src/scene/authoring/CoreComponentCodecs.h"
        "src/scene/authoring/CoreComponentCodecs.cpp"
        "src/scene/authoring/SourceJsonParser.h"
        "src/scene/authoring/SourceJsonParser.cpp"
        "src/scene/authoring/SourceSceneDocument.h"
        "src/scene/authoring/SourceSceneDocument.cpp"
        "src/scene/authoring/SourceSceneEnvelopeMigrator.h"
        "src/scene/authoring/SourceSceneEnvelopeMigrator.cpp"
        "src/scene/authoring/SourceSceneLoadTransaction.h"
        "src/scene/authoring/SourceSceneLoadTransaction.cpp"
        "src/scene/authoring/SourceSceneCapture.h"
        "src/scene/authoring/SourceSceneCapture.cpp"
        "src/scene/authoring/AtomicSourceSceneFile.h"
        "src/scene/authoring/AtomicSourceSceneFile.cpp"
        "src/scene/authoring/CookedSceneCompiler.h"
        "src/scene/authoring/CookedSceneCompiler.cpp"
        "src/utils/Sha256.h"
        "src/utils/Sha256.cpp"
)
target_include_directories(IridiumSceneAuthoring PUBLIC src)
target_link_libraries(IridiumSceneAuthoring PUBLIC
        IridiumSceneRuntime
        nlohmann_json::nlohmann_json)

add_executable(IridiumSourceSceneBenchmark
        "tools/SourceSceneBenchmark.cpp"
        "src/profiling/CpuAllocationProfile.cpp"
)
target_link_libraries(IridiumSourceSceneBenchmark PRIVATE
        IridiumSceneAuthoring)
target_compile_definitions(IridiumSourceSceneBenchmark PRIVATE
        IRIDIUM_BENCHMARK_CONFIGURATION="$<CONFIG>"
        IRIDIUM_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(WIN32)
    target_link_libraries(IridiumSourceSceneBenchmark PRIVATE Psapi)
endif()

add_executable(IridiumCookedSceneBenchmark
        "tools/CookedSceneBenchmark.cpp"
        "src/profiling/CpuAllocationProfile.cpp"
)
target_link_libraries(IridiumCookedSceneBenchmark PRIVATE
        IridiumSceneAuthoring
        IridiumAssetCore)
target_compile_definitions(IridiumCookedSceneBenchmark PRIVATE
        IRIDIUM_BENCHMARK_CONFIGURATION="$<CONFIG>"
        IRIDIUM_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        IRIDIUM_BENCHMARK_SOURCE_COMMIT="${IRIDIUM_SOURCE_COMMIT}"
        IRIDIUM_BENCHMARK_SOURCE_DIRTY=${IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE})
if(WIN32)
    target_link_libraries(IridiumCookedSceneBenchmark PRIVATE Psapi)
endif()

add_executable(BakedLightingProductBenchmark
        "tools/BakedLightingProductBenchmark.cpp")
target_link_libraries(BakedLightingProductBenchmark PRIVATE
        IridiumAssetCore)

# M3 backend-neutral identity/metadata/catalog module. The SQLite database is a
# rebuildable editor index; persistent identity remains in source-controlled sidecars.
add_library(IridiumAssetCore STATIC
        "src/core/EngineLog.h"
        "src/core/EngineLog.cpp"
        "src/assets/AssetMetadata.h"
        "src/assets/AssetMetadata.cpp"
        "src/assets/AssetCatalog.h"
        "src/assets/AssetImport.h"
        "src/assets/AssetImport.cpp"
        "src/assets/AssetCatalogService.h"
        "src/assets/AssetCatalogService.cpp"
        "src/assets/AssetContentOperations.h"
        "src/assets/AssetContentOperations.cpp"
        "src/assets/AssetBrowserModel.h"
        "src/assets/AssetBrowserModel.cpp"
        "src/assets/AssetDiscovery.h"
        "src/assets/AssetDiscovery.cpp"
        "src/assets/SqliteAssetCatalog.h"
        "src/assets/SqliteAssetCatalog.cpp"
        "src/assets/cooker/CanonicalSettings.h"
        "src/assets/cooker/CanonicalSettings.cpp"
        "src/assets/cooker/ImporterRegistry.h"
        "src/assets/cooker/ImporterRegistry.cpp"
        "src/assets/cooker/DependencyGraph.h"
        "src/assets/cooker/DependencyGraph.cpp"
        "src/assets/cooker/LocalDerivedDataCache.h"
        "src/assets/cooker/LocalDerivedDataCache.cpp"
        "src/assets/cooker/TextFixtureImporter.h"
        "src/assets/cooker/TextFixtureImporter.cpp"
        "src/assets/cooker/AssetCooker.h"
        "src/assets/cooker/AssetCooker.cpp"
        "src/assets/cooker/CookReceipt.h"
        "src/assets/cooker/CookReceipt.cpp"
        "src/assets/texture/TextureProduct.h"
        "src/assets/texture/TextureProduct.cpp"
        "src/assets/texture/TextureMipGenerator.h"
        "src/assets/texture/TextureMipGenerator.cpp"
        "src/assets/texture/TextureImporter.h"
        "src/assets/texture/TextureImporter.cpp"
        "src/assets/environment/EnvironmentProduct.h"
        "src/assets/environment/EnvironmentProduct.cpp"
        "src/assets/environment/EnvironmentConvolution.h"
        "src/assets/environment/EnvironmentConvolution.cpp"
        "src/assets/environment/EnvironmentImporter.h"
        "src/assets/environment/EnvironmentImporter.cpp"
        "src/assets/environment/BakedProbeEnvironmentImporter.h"
        "src/assets/environment/BakedProbeEnvironmentImporter.cpp"
        "src/assets/environment/AssetEnvironmentPreparationService.h"
        "src/assets/environment/AssetEnvironmentPreparationService.cpp"
        "src/assets/lighting/BakedLightingProduct.h"
        "src/assets/lighting/BakedLightingProduct.cpp"
        "src/assets/material/CompiledMaterialProduct.h"
        "src/assets/material/CompiledMaterialProduct.cpp"
        "src/assets/model/ModelProduct.h"
        "src/assets/model/ModelProduct.cpp"
        "src/assets/model/GltfModelImporter.h"
        "src/assets/model/GltfModelImporter.cpp"
        "src/assets/model/ModelRuntimeProduct.h"
        "src/assets/model/ModelRuntimeProduct.cpp"
        "src/assets/model/AssetModelPreparationService.h"
        "src/assets/model/AssetModelPreparationService.cpp"
        "src/assets/thumbnail/AssetThumbnail.h"
        "src/assets/thumbnail/AssetThumbnail.cpp"
        "src/assets/thumbnail/AssetThumbnailService.h"
        "src/assets/thumbnail/AssetThumbnailService.cpp"
        "src/assets/thumbnail/AssetThumbnailUploadQueue.h"
        "src/assets/thumbnail/AssetThumbnailUploadQueue.cpp"
        "src/assets/runtime/AssetRuntimePublisher.h"
        "src/assets/runtime/AssetRuntimePublisher.cpp"
        "src/assets/runtime/AssetReimportScheduler.h"
        "src/assets/runtime/AssetReimportScheduler.cpp"
        "src/assets/runtime/SourceChangeTracker.h"
        "src/assets/runtime/SourceChangeTracker.cpp"
        "src/assets/runtime/SourceFileWatcher.h"
        "src/assets/runtime/SourceFileWatcher.cpp"
        "src/assets/runtime/AssetSourceMonitor.h"
        "src/assets/runtime/AssetSourceMonitor.cpp"
        "src/assets/runtime/AssetRuntimeService.h"
        "src/assets/runtime/AssetRuntimeService.cpp"
        "src/material/SourceMaterial.h"
        "src/material/SourceMaterial.cpp"
        "src/material/MaterialCompiler.h"
        "src/material/MaterialCompiler.cpp"
        "src/material/MaterialRuntime.h"
        "src/material/MaterialRuntime.cpp"
        "src/renderer/color/SceneColor.h"
        "src/renderer/color/SceneColor.cpp"
        "src/vendor/fastgltf/src/fastgltf.cpp"
        "src/vendor/fastgltf/src/base64.cpp"
        "src/vendor/fastgltf/src/io.cpp"
        "src/vendor/simdjson/simdjson.cpp"
        "src/renderer/rhi/TextureResidency.h"
        "src/renderer/rhi/TextureResidency.cpp"
        "src/vendor/std_image_impl.cpp"
)
target_include_directories(IridiumAssetCore PUBLIC
        src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/vendor/simdjson
)
target_link_libraries(IridiumAssetCore PUBLIC
        IridiumCookedArtifact
        glm::glm
        nlohmann_json::nlohmann_json
        Threads::Threads
)
if(WIN32)
    target_link_libraries(IridiumAssetCore PRIVATE
            winsqlite3 Bcrypt DirectXTex ole32)
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(IridiumAssetCore PRIVATE SQLite::SQLite3)
endif()

# 6. THE EXECUTABLE
# Added ${SPV_FILES} so CMake knows to build shaders before the app
add_executable(IridiumEngine
        ${SPV_FILES}
        src/main.cpp
        "src/renderer/vulkan/VkContext.h"
        "src/renderer/vulkan/VkContext.cpp"
        "src/renderer/vulkan/VkSwapchain.h"
        "src/renderer/vulkan/VkSwapchain.cpp"
        "src/renderer/vulkan/VkRenderPass.h"
        "src/renderer/vulkan/VkRenderPass.cpp"
        "src/utils/File.h"
        "src/renderer/vulkan/VkGraphicsPipeline.h"
        "src/renderer/vulkan/VkGraphicsPipeline.cpp"
        "src/renderer/rhi/Mesh.h"
        "src/assets/AssetManager.h"
        "src/assets/AssetManager.cpp"
        "src/ecs/Entity.h"
        "src/scene/Scene.h"
        "src/scene/Components.h"
        "src/scene/components/NameComponent.h"
        "src/scene/components/TransformComponent.h"
        "src/scene/components/SkyComponent.h"
        "src/scene/components/ReflectionProbeComponent.h"
        "src/ecs/Registry.h"
        "src/scene/components/MeshComponent.h"
        "src/editor/EditorSceneActions.h"
        "src/editor/EditorSceneActions.cpp"
        "src/editor/EditorSceneDocumentActions.h"
        "src/editor/EditorSceneDocumentActions.cpp"
        "src/editor/ViewportPlacement.h"
        "src/editor/ViewportPlacement.cpp"
        "src/vendor/imgui/imgui.cpp"
        "src/vendor/imgui/imgui_demo.cpp"
        "src/vendor/imgui/imgui_draw.cpp"
        "src/vendor/imgui/imgui_tables.cpp"
        "src/vendor/imgui/imgui_widgets.cpp"
        "src/vendor/imgui/backends/imgui_impl_glfw.cpp"
        "src/vendor/imgui/backends/imgui_impl_vulkan.cpp"
        "src/editor/EditorSystem.h"
        "src/editor/EditorSystem.cpp"
        "src/editor/EditorAssetDocumentService.h"
        "src/editor/EditorAssetDocumentService.cpp"
        "src/editor/EditorOrbitCamera.h"
        "src/editor/EditorOrbitCamera.cpp"
        "src/ecs/systems/TransformSystem.cpp"
        "src/ecs/systems/TransformSystem.h"
        "src/scene/components/RelationshipComponent.h"
        "src/vendor/imguizmo/ImGuizmo.cpp"
        "src/vendor/simdjson/simdjson.cpp"
        "src/vendor/fastgltf/src/fastgltf.cpp"
        "src/vendor/fastgltf/src/base64.cpp"
        "src/vendor/fastgltf/src/io.cpp"
 "src/scene/components/LightComponent.h" "src/editor/Reflection.h" "src/editor/ComponentCollectionUI.h" "src/editor/ViewportLayout.h" "src/editor/ViewportLayout.cpp" "src/utils/DeletionQueue.h" "src/core/Application.h" "src/core/Application.cpp" "src/editor/panels/EditorPanel.h" "src/editor/panels/core/SceneHierarchyPanel.h" "src/editor/panels/core/SceneHierarchyPanel.cpp" "src/editor/panels/core/InspectorPanel.h" "src/editor/panels/core/InspectorPanel.cpp"     "src/editor/EditorUIState.h" "src/editor/panels/menus/MenuBarPanel.h" "src/editor/panels/menus/MenuBarPanel.cpp" "src/editor/panels/windows/ProjectSettingsPanel.h" "src/editor/panels/windows/ProjectSettingsPanel.cpp" "src/renderer/vulkan/DescriptorAllocator.h" "src/renderer/vulkan/DescriptorAllocator.cpp" "src/renderer/vulkan/VkUIRenderPass.h" "src/renderer/vulkan/VkUIRenderPass.cpp" "src/editor/panels/core/ViewPortPanel.h" "src/editor/panels/core/ViewPortPanel.cpp" "src/renderer/vulkan/VkLightingPipeline.h" "src/renderer/vulkan/VkLightingPipeline.cpp" "src/renderer/vulkan/VkForwardRenderPass.cpp" "src/renderer/vulkan/VkForwardRenderPass.h" "src/renderer/vulkan/GlassDepthRenderPass.h" "src/renderer/vulkan/GlassDepthRenderPass.cpp" "src/renderer/vulkan/GlassDepthPipeline.h" "src/renderer/vulkan/GlassDepthPipeline.cpp" "src/renderer/rhi/IRenderBackend.h" "src/renderer/rhi/RenderBackendFactory.h" "src/renderer/rhi/RenderBackendFactory.cpp" "src/renderer/rhi/DrawPacket.h" "src/renderer/rhi/RenderHandles.h" "src/renderer/rhi/ResourcePool.h" "src/renderer/rhi/RhiResourceTypes.h" "src/renderer/vulkan/VulkanResourceAllocator.h" "src/renderer/vulkan/VulkanResourceState.h" "src/renderer/vulkan/VulkanResourceState.cpp" "src/renderer/vulkan/VulkanCommandList.h" "src/renderer/vulkan/VulkanUploadContext.h" "src/renderer/vulkan/VulkanIndexedTextureTable.h" "src/renderer/vulkan/VulkanIndexedTextureTable.cpp" "src/renderer/vulkan/VulkanVertexBackend.h" "src/renderer/vulkan/VulkanVertexBackend.cpp" "src/renderer/vulkan/VulkanVertexUtils.h" "src/renderer/vulkan/VulkanMeshLayouts.h" "src/renderer/vulkan/VulkanMeshLayouts.cpp" "src/renderer/rhi/PipelineTypes.h" "src/renderer/rhi/TextureTypes.h" "src/renderer/vulkan/VulkanPipelineLibrary.h" "src/renderer/vulkan/VulkanPipelineLibrary.cpp")

target_sources(IridiumEngine PRIVATE
        "src/platform/FileDialog.h"
        "src/editor/EditorComponentRegistry.h"
        "src/editor/EditorComponentRegistry.cpp"
        "src/editor/EditorComponentDrawerRegistry.h"
        "src/editor/EditorComponentDrawerRegistry.cpp"
        "src/editor/CoreEditorComponentRegistry.h"
        "src/editor/CoreEditorComponentRegistry.cpp"
        "src/editor/EditorTransactionService.h"
        "src/editor/EditorTransactionService.cpp"
        "src/editor/EditorSceneHierarchy.h"
        "src/editor/EditorSceneHierarchy.cpp"
        "src/editor/EditorSceneCommandService.h"
        "src/editor/EditorSceneCommandService.cpp"
        "src/editor/ViewportRenderExtent.h"
        "src/editor/ViewportRenderExtent.cpp"
        "src/editor/EditorMeshTransaction.h"
        "src/editor/EditorMeshTransaction.cpp"
        "src/editor/EditorPropertyTransaction.h"
        "src/editor/EditorPropertyTransaction.cpp"
        "src/editor/EditorSelectionState.h"
        "src/editor/EditorPropertyDrawer.h"
        "src/editor/EditorPropertyDrawer.cpp"
        "src/platform/SystemProfile.h"
        "src/platform/SystemProfile.cpp"
        "src/core/ApplicationConfig.h"
        "src/core/ApplicationConfig.cpp"
        "src/profiling/CpuProfiler.h"
        "src/profiling/CpuProfiler.cpp"
        "src/profiling/GpuTimestamp.h"
        "src/profiling/GpuTimestamp.cpp"
        "src/profiling/MemoryProfile.h"
        "src/profiling/MemoryProfile.cpp"
        "src/profiling/CpuProfileExport.h"
        "src/profiling/CpuProfileExport.cpp"
        "src/profiling/CpuAllocationProfile.h"
        "src/profiling/CpuAllocationProfile.cpp"
        "src/assets/MaterialProvenance.h"
        "src/assets/MaterialProvenance.cpp"
        "src/material/SourceMaterial.h"
        "src/material/SourceMaterial.cpp"
        "src/material/MaterialCompiler.h"
        "src/material/MaterialCompiler.cpp"
        "src/material/StandardMaterialShading.h"
        "src/material/ComplexMaterialShading.h"
        "src/material/MaterialTangentGeneration.h"
        "src/material/MaterialRuntime.h"
        "src/material/MaterialRuntime.cpp"
        "src/material/MaterialDiagnosticEvaluation.h"
        "src/material/MaterialTextureCompatibility.h"
        "src/material/MaterialTextureCompatibility.cpp"
        "src/renderer/rhi/RenderDebugView.h"
        "src/renderer/rhi/FrameCapture.h"
        "src/renderer/color/SceneColor.h"
        "src/renderer/color/SceneColor.cpp"
        "src/renderer/color/OutputTransformConfig.h"
        "src/renderer/color/OutputTransformConfig.cpp"
        "src/renderer/color/AcesOutputLut.h"
        "src/renderer/color/AcesOutputLut.cpp"
        "src/capture/TgaImage.h"
        "src/capture/TgaImage.cpp"
        "src/capture/PfmImage.h"
        "src/capture/PfmImage.cpp"
        "src/capture/CaptureArtifact.h"
        "src/capture/CaptureArtifact.cpp"
        "src/benchmarks/BenchmarkManifest.h"
        "src/benchmarks/BenchmarkManifest.cpp"
        "src/utils/Sha256.h"
        "src/utils/Sha256.cpp"
        "src/renderer/rhi/RenderBackendConfig.h"
        "src/renderer/rhi/RenderBackendRuntimeInfo.h"
        "src/editor/panels/windows/ProfilerPanel.h"
        "src/editor/panels/windows/ProfilerPanel.cpp"
        "src/editor/panels/windows/MaterialDiagnosticsPanel.h"
        "src/editor/panels/windows/MaterialDiagnosticsPanel.cpp"
        "src/editor/panels/windows/AssetBrowserPanel.h"
        "src/editor/panels/windows/AssetBrowserPanel.cpp"
        "src/editor/panels/windows/AssetViewerPanel.h"
        "src/editor/panels/windows/AssetViewerPanel.cpp"
        "src/editor/panels/windows/ConsolePanel.h"
        "src/editor/panels/windows/ConsolePanel.cpp"
)

if (APPLE)
    enable_language(OBJCXX)
    target_sources(IridiumEngine PRIVATE "src/platform/FileDialogMac.mm")
else()
    target_sources(IridiumEngine PRIVATE "src/platform/FileDialog.cpp")
endif()

# Add the Stage 3 allocator implementation alongside its contract.
target_sources(IridiumEngine PRIVATE
        "src/scene/authoring/CoreSceneComponentAdapters.h"
        "src/scene/authoring/CoreSceneComponentAdapters.cpp"
        "src/editor/EditorSceneDocumentService.h"
        "src/editor/EditorSceneDocumentService.cpp"
        "src/renderer/vulkan/VulkanResourceAllocator.cpp"
        "src/renderer/vulkan/VulkanUploadContext.cpp"
        "src/renderer/vulkan/VulkanFrameScheduler.h"
        "src/renderer/vulkan/VulkanFrameScheduler.cpp"
        "src/renderer/vulkan/VulkanFrameTargets.h"
        "src/renderer/vulkan/VulkanFrameTargets.cpp"
        "src/renderer/vulkan/VulkanSceneDescriptors.h"
        "src/renderer/vulkan/VulkanSceneDescriptors.cpp"
        "src/renderer/vulkan/VulkanDirectionalShadowMap.h"
        "src/renderer/vulkan/VulkanDirectionalShadowMap.cpp"
        "src/renderer/vulkan/VulkanSpotShadowAtlas.h"
        "src/renderer/vulkan/VulkanSpotShadowAtlas.cpp"
        "src/renderer/vulkan/VulkanPointShadowPools.h"
        "src/renderer/vulkan/VulkanPointShadowPools.cpp"
        "src/renderer/vulkan/VulkanRenderGraphExecutor.h"
        "src/renderer/vulkan/VulkanRenderGraphExecutor.cpp"
        "src/renderer/vulkan/VulkanProductionRenderGraph.h"
        "src/renderer/vulkan/VulkanProductionRenderGraph.cpp"
        "src/renderer/vulkan/VulkanTransparencyPyramid.h"
        "src/renderer/vulkan/VulkanTransparencyPyramid.cpp"
        "src/renderer/vulkan/VulkanLayeredInterfaceCapturePass.h"
        "src/renderer/vulkan/VulkanLayeredInterfaceCapturePass.cpp"
        "src/renderer/vulkan/VulkanLayeredLocalCompositionPass.h"
        "src/renderer/vulkan/VulkanLayeredLocalCompositionPass.cpp"
        "src/renderer/vulkan/VulkanLayeredSceneResolvePass.h"
        "src/renderer/vulkan/VulkanLayeredSceneResolvePass.cpp"
        "src/renderer/transparency/Ordinary2Atlas.h"
        "src/renderer/transparency/LayeredAtlas.h"
        "src/renderer/vulkan/VulkanClusteredLightingPipeline.h"
        "src/renderer/vulkan/VulkanClusteredLightingPipeline.cpp"
        "src/renderer/vulkan/VulkanReflectionProbePipeline.h"
        "src/renderer/vulkan/VulkanReflectionProbePipeline.cpp"
        "src/renderer/vulkan/VulkanReflectionProbeCaptureTargets.h"
        "src/renderer/vulkan/VulkanReflectionProbeCaptureTargets.cpp"
        "src/renderer/vulkan/VulkanReflectionProbeCapturePass.h"
        "src/renderer/vulkan/VulkanReflectionProbeCapturePass.cpp"
        "src/renderer/vulkan/VulkanOutputPass.h"
        "src/renderer/vulkan/VulkanOutputPass.cpp"
        "src/renderer/vulkan/VulkanHdrEncodePass.h"
        "src/renderer/vulkan/VulkanHdrEncodePass.cpp"
        "src/renderer/vulkan/VulkanOutputTransport.h"
        "src/renderer/vulkan/VulkanOutputTransport.cpp"
)

# Defines PROJECT_ROOT_DIR for C++ to find assets
add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")

target_compile_definitions(IridiumEngine PRIVATE
        IRIDIUM_BUILD_CONFIGURATION="${CMAKE_BUILD_TYPE}"
        IRIDIUM_SOURCE_COMMIT="${IRIDIUM_SOURCE_COMMIT}"
        IRIDIUM_SOURCE_BRANCH="${IRIDIUM_SOURCE_BRANCH}"
        IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE=${IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE}
        IRIDIUM_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        IRIDIUM_SHADER_COMPILER="${IRIDIUM_SHADER_COMPILER}"
        IRIDIUM_VULKAN_SDK="${IRIDIUM_VULKAN_SDK_VERSION}"
)

# 7. INCLUDE PATHS
target_include_directories(IridiumEngine PRIVATE src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/vendor/simdjson
        ${CMAKE_SOURCE_DIR}/src/vendor/imgui
        ${CMAKE_SOURCE_DIR}/src/vendor/imgui/backends
)

# 8. LINKING
target_link_libraries(IridiumEngine PRIVATE
        glfw
        glm::glm
        Vulkan::Vulkan
        nlohmann_json::nlohmann_json
        Threads::Threads
        IridiumRenderGraph
        IridiumLighting
        IridiumSceneRuntime
        IridiumSceneAuthoring
        IridiumAssetCore
        ${PLATFORM_LIBS} # Links Cocoa/IOKit on Mac, nothing on Windows
)

add_executable(IridiumImageCompare
        "tools/ImageCompare.cpp"
        "src/capture/ImageComparison.h"
        "src/capture/ImageComparison.cpp"
        "src/capture/TgaImage.h"
        "src/capture/TgaImage.cpp"
        "src/utils/Sha256.h"
        "src/utils/Sha256.cpp"
)

target_include_directories(IridiumImageCompare PRIVATE src)
target_link_libraries(IridiumImageCompare PRIVATE nlohmann_json::nlohmann_json)

add_executable(IridiumAssetCatalogInspect
        "tools/AssetCatalogInspect.cpp"
)
target_link_libraries(IridiumAssetCatalogInspect PRIVATE IridiumAssetCore)

add_executable(IridiumCookAsset
        "tools/CookAsset.cpp"
)
target_link_libraries(IridiumCookAsset PRIVATE IridiumAssetCore)

add_executable(IridiumBakeEnvironment
        "tools/BakeEnvironment.cpp"
)
target_link_libraries(IridiumBakeEnvironment PRIVATE IridiumAssetCore)

add_executable(IridiumCompareEnvironment
        "tools/CompareEnvironment.cpp"
)
target_link_libraries(IridiumCompareEnvironment PRIVATE IridiumAssetCore)

add_executable(IridiumInspectAssetSource
        "tools/InspectAssetSource.cpp"
)
target_link_libraries(IridiumInspectAssetSource PRIVATE IridiumAssetCore)

add_executable(IridiumImportAsset
        "tools/ImportAsset.cpp"
)
target_link_libraries(IridiumImportAsset PRIVATE IridiumAssetCore)

add_executable(IridiumInspectCookedModel
        "tools/InspectCookedModel.cpp"
)
target_link_libraries(IridiumInspectCookedModel PRIVATE
        IridiumAssetCore)

add_executable(IridiumInspectCookedScene
        "tools/InspectCookedScene.cpp"
)
target_link_libraries(IridiumInspectCookedScene PRIVATE
        IridiumCookedArtifact
        nlohmann_json::nlohmann_json)

add_executable(IridiumCookScene
        "tools/CookScene.cpp"
        "src/scene/authoring/CoreSceneComponentAdapters.cpp"
)
target_include_directories(IridiumCookScene PRIVATE
        src)
target_link_libraries(IridiumCookScene PRIVATE
        IridiumSceneAuthoring
        IridiumAssetCore
        glm::glm)

include(CTest)

if(BUILD_TESTING)
    add_executable(EditorComponentRegistryTests
            "tests/editor/EditorComponentRegistryTests.cpp"
            "src/editor/EditorComponentRegistry.cpp"
            "src/editor/EditorComponentDrawerRegistry.cpp"
            "src/editor/CoreEditorComponentRegistry.cpp"
    )
    target_include_directories(EditorComponentRegistryTests PRIVATE src)
    target_link_libraries(EditorComponentRegistryTests PRIVATE
            IridiumSceneRuntime
            glm::glm)
    add_test(NAME EditorComponentRegistryTests
            COMMAND EditorComponentRegistryTests)

    add_executable(RuntimeComponentRegistryTests
            "tests/scene/RuntimeComponentRegistryTests.cpp"
    )
    target_link_libraries(RuntimeComponentRegistryTests PRIVATE
            IridiumSceneRuntime)
    add_test(NAME RuntimeComponentRegistryTests
            COMMAND RuntimeComponentRegistryTests)

    add_executable(CookedSceneTests
            "tests/scene/CookedSceneTests.cpp"
    )
    target_link_libraries(CookedSceneTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore)
    add_test(NAME CookedSceneTests COMMAND CookedSceneTests)
    add_test(NAME CookedSceneProcessDeterminism
            COMMAND ${CMAKE_COMMAND}
                -DCOOKED_SCENE_TEST=$<TARGET_FILE:CookedSceneTests>
                -DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/m4-process-determinism
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene/CookedSceneProcessDeterminism.cmake)

    add_executable(SceneRuntimeBoundaryTests
            "tests/scene/SceneRuntimeBoundaryTests.cpp"
    )
    target_link_libraries(SceneRuntimeBoundaryTests PRIVATE
            IridiumSceneRuntime)
    add_test(NAME SceneRuntimeBoundaryTests COMMAND SceneRuntimeBoundaryTests)

    add_test(NAME CookSceneCliTests
            COMMAND ${CMAKE_COMMAND}
                -DCOOK_SCENE=$<TARGET_FILE:IridiumCookScene>
                -DINSPECT_SCENE=$<TARGET_FILE:IridiumInspectCookedScene>
                -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/m4-cook-scene-cli
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene/CookSceneCliTests.cmake)

    add_executable(SourceComponentRegistryTests
            "tests/scene/SourceComponentRegistryTests.cpp"
    )
    target_link_libraries(SourceComponentRegistryTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceComponentRegistryTests
            COMMAND SourceComponentRegistryTests)

    add_executable(SourceJsonParserTests
            "tests/scene/SourceJsonParserTests.cpp"
    )
    target_link_libraries(SourceJsonParserTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceJsonParserTests COMMAND SourceJsonParserTests)

    add_executable(SourceSceneDocumentTests
            "tests/scene/SourceSceneDocumentTests.cpp"
    )
    target_link_libraries(SourceSceneDocumentTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceSceneDocumentTests COMMAND SourceSceneDocumentTests)

    add_executable(SourceSceneEnvelopeMigratorTests
            "tests/scene/SourceSceneEnvelopeMigratorTests.cpp"
    )
    target_link_libraries(SourceSceneEnvelopeMigratorTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceSceneEnvelopeMigratorTests
            COMMAND SourceSceneEnvelopeMigratorTests)

    add_executable(CoreComponentRegistryTests
            "tests/scene/CoreComponentRegistryTests.cpp"
    )
    target_link_libraries(CoreComponentRegistryTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME CoreComponentRegistryTests COMMAND CoreComponentRegistryTests)

    add_executable(SourceSceneLoadTransactionTests
            "tests/scene/SourceSceneLoadTransactionTests.cpp"
    )
    target_link_libraries(SourceSceneLoadTransactionTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceSceneLoadTransactionTests
            COMMAND SourceSceneLoadTransactionTests)

    add_executable(SourceSceneCaptureTests
            "tests/scene/SourceSceneCaptureTests.cpp"
    )
    target_link_libraries(SourceSceneCaptureTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME SourceSceneCaptureTests
            COMMAND SourceSceneCaptureTests)

    add_executable(AtomicSourceSceneFileTests
            "tests/scene/AtomicSourceSceneFileTests.cpp"
    )
    target_link_libraries(AtomicSourceSceneFileTests PRIVATE
            IridiumSceneAuthoring)
    add_test(NAME AtomicSourceSceneFileTests
            COMMAND AtomicSourceSceneFileTests)

    add_executable(CoreSceneComponentAdaptersTests
            "tests/scene/CoreSceneComponentAdaptersTests.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(CoreSceneComponentAdaptersTests PRIVATE
            src)
    target_link_libraries(CoreSceneComponentAdaptersTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    add_test(NAME CoreSceneComponentAdaptersTests
            COMMAND CoreSceneComponentAdaptersTests)

    add_executable(BakedLightingProductTests
            "tests/assets/BakedLightingProductTests.cpp"
    )
    target_include_directories(BakedLightingProductTests PRIVATE src)
    target_link_libraries(BakedLightingProductTests PRIVATE
            IridiumAssetCore)
    add_test(NAME BakedLightingProductTests COMMAND BakedLightingProductTests)

    add_executable(LightComponentV2Tests
            "tests/scene/LightComponentV2Tests.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(LightComponentV2Tests PRIVATE src)
    target_link_libraries(LightComponentV2Tests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    add_test(NAME LightComponentV2Tests COMMAND LightComponentV2Tests)

    add_executable(LightExtractorTests
            "tests/renderer/LightExtractorTests.cpp"
            "src/ecs/systems/TransformSystem.cpp"
    )
    target_include_directories(LightExtractorTests PRIVATE src)
    target_link_libraries(LightExtractorTests PRIVATE
            IridiumLighting
            glm::glm)
    add_test(NAME LightExtractorTests COMMAND LightExtractorTests)

    add_executable(ClusteredLightingTests
            "tests/renderer/ClusteredLightingTests.cpp"
    )
    target_include_directories(ClusteredLightingTests PRIVATE src)
    target_link_libraries(ClusteredLightingTests PRIVATE
            IridiumLighting
            glm::glm)
    add_test(NAME ClusteredLightingTests COMMAND ClusteredLightingTests)

    add_executable(DirectionalShadowTests
            "tests/renderer/DirectionalShadowTests.cpp"
    )
    target_include_directories(DirectionalShadowTests PRIVATE src)
    target_link_libraries(DirectionalShadowTests PRIVATE
            IridiumLighting
            glm::glm)
    add_test(NAME DirectionalShadowTests COMMAND DirectionalShadowTests)

    add_executable(LocalShadowTests
            "tests/renderer/LocalShadowTests.cpp"
    )
    target_include_directories(LocalShadowTests PRIVATE src)
    target_link_libraries(LocalShadowTests PRIVATE
            IridiumLighting
            glm::glm)
    add_test(NAME LocalShadowTests COMMAND LocalShadowTests)

    add_executable(M4ProductionCutoverTests
            "tests/scene/M4ProductionCutoverTests.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(M4ProductionCutoverTests PRIVATE src)
    target_link_libraries(M4ProductionCutoverTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    target_compile_definitions(M4ProductionCutoverTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME M4ProductionCutoverTests COMMAND M4ProductionCutoverTests)

    add_executable(M5FixtureContractTests
            "tests/scene/M5FixtureContractTests.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(M5FixtureContractTests PRIVATE src)
    target_link_libraries(M5FixtureContractTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm
            nlohmann_json::nlohmann_json)
    target_compile_definitions(M5FixtureContractTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME M5FixtureContractTests COMMAND M5FixtureContractTests)

    add_executable(M6FixtureContractTests
            "tests/scene/M6FixtureContractTests.cpp"
    )
    target_include_directories(M6FixtureContractTests PRIVATE src)
    target_link_libraries(M6FixtureContractTests PRIVATE
            IridiumAssetCore
            nlohmann_json::nlohmann_json)
    target_compile_definitions(M6FixtureContractTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME M6FixtureContractTests COMMAND M6FixtureContractTests)

    add_executable(EditorSceneDocumentServiceTests
            "tests/editor/EditorSceneDocumentServiceTests.cpp"
            "src/editor/EditorSceneDocumentService.cpp"
            "src/editor/EditorSceneDocumentActions.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(EditorSceneDocumentServiceTests PRIVATE
            src)
    target_link_libraries(EditorSceneDocumentServiceTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    add_test(NAME EditorSceneDocumentServiceTests
            COMMAND EditorSceneDocumentServiceTests)

    add_executable(EditorTransactionServiceTests
            "tests/editor/EditorTransactionServiceTests.cpp"
            "src/editor/EditorComponentRegistry.cpp"
            "src/editor/CoreEditorComponentRegistry.cpp"
            "src/editor/EditorSceneHierarchy.cpp"
            "src/editor/EditorSceneCommandService.cpp"
            "src/editor/EditorSceneActions.cpp"
            "src/editor/EditorTransactionService.cpp"
            "src/editor/EditorMeshTransaction.cpp"
            "src/editor/EditorPropertyTransaction.cpp"
            "src/editor/EditorSceneDocumentService.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(EditorTransactionServiceTests PRIVATE src)
    target_link_libraries(EditorTransactionServiceTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    add_test(NAME EditorTransactionServiceTests
            COMMAND EditorTransactionServiceTests)

    add_executable(EditorAssetDocumentServiceTests
            "tests/editor/EditorAssetDocumentServiceTests.cpp"
            "src/editor/EditorAssetDocumentService.cpp"
            "src/editor/EditorOrbitCamera.cpp"
            "src/editor/EditorSceneDocumentService.cpp"
            "src/editor/EditorTransactionService.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(EditorAssetDocumentServiceTests PRIVATE src)
    target_link_libraries(EditorAssetDocumentServiceTests PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm)
    add_test(NAME EditorAssetDocumentServiceTests
            COMMAND EditorAssetDocumentServiceTests)

    add_executable(Stage3ArchitectureTests
            "tests/renderer/Stage3ArchitectureTests.cpp"
            "src/renderer/vulkan/VulkanResourceState.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(Stage3ArchitectureTests PRIVATE src)
    target_link_libraries(Stage3ArchitectureTests PRIVATE glm::glm Vulkan::Vulkan)

    add_test(NAME Stage3ArchitectureTests COMMAND Stage3ArchitectureTests)

    add_executable(CpuProfilerTests
            "tests/profiling/CpuProfilerTests.cpp"
            "src/profiling/CpuProfiler.cpp"
            "src/profiling/GpuTimestamp.cpp"
            "src/profiling/MemoryProfile.cpp"
            "src/profiling/CpuProfileExport.cpp"
    )

    target_include_directories(CpuProfilerTests PRIVATE src)
    target_link_libraries(CpuProfilerTests PRIVATE
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    add_test(NAME CpuProfilerTests COMMAND CpuProfilerTests)

    add_executable(CpuAllocationProfileTests
            "tests/profiling/CpuAllocationProfileTests.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(CpuAllocationProfileTests PRIVATE src)

    add_test(NAME CpuAllocationProfileTests COMMAND CpuAllocationProfileTests)

    add_executable(CpuAllocationProfileBenchmark
            "tests/profiling/CpuAllocationProfileBenchmark.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(CpuAllocationProfileBenchmark PRIVATE src)

    add_executable(CpuProfilerBenchmark
            "tests/profiling/CpuProfilerBenchmark.cpp"
            "src/profiling/CpuProfiler.cpp"
    )

    target_include_directories(CpuProfilerBenchmark PRIVATE src)
    target_link_libraries(CpuProfilerBenchmark PRIVATE Threads::Threads)

    add_executable(ApplicationConfigTests
            "tests/core/ApplicationConfigTests.cpp"
            "src/core/ApplicationConfig.cpp"
            "src/assets/AssetGuid.cpp"
    )

    target_include_directories(ApplicationConfigTests PRIVATE src)
    if(WIN32)
        target_link_libraries(ApplicationConfigTests PRIVATE Bcrypt)
    endif()

    add_test(NAME ApplicationConfigTests COMMAND ApplicationConfigTests)

    add_executable(AcesOutputLutTests
            "tests/renderer/AcesOutputLutTests.cpp"
            "src/renderer/color/AcesOutputLut.cpp"
    )

    target_include_directories(AcesOutputLutTests PRIVATE src)
    add_test(NAME AcesOutputLutTests COMMAND AcesOutputLutTests)

    add_executable(MaterialProvenanceTests
            "tests/assets/MaterialProvenanceTests.cpp"
            "src/assets/MaterialProvenance.cpp"
            "src/material/SourceMaterial.cpp"
    )

    target_include_directories(MaterialProvenanceTests PRIVATE src)
    target_link_libraries(MaterialProvenanceTests PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )

    add_test(NAME MaterialProvenanceTests COMMAND MaterialProvenanceTests)

    add_executable(AssetCatalogTests
            "tests/assets/AssetCatalogTests.cpp"
    )
    target_link_libraries(AssetCatalogTests PRIVATE IridiumAssetCore)
    target_compile_definitions(AssetCatalogTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetCatalogTests COMMAND AssetCatalogTests)

    add_executable(AssetBrowserModelTests
            "tests/assets/AssetBrowserModelTests.cpp"
    )
    target_link_libraries(AssetBrowserModelTests PRIVATE IridiumAssetCore)
    add_test(NAME AssetBrowserModelTests COMMAND AssetBrowserModelTests)

    add_executable(AssetContentOperationsTests
            "tests/assets/AssetContentOperationsTests.cpp"
    )
    target_link_libraries(AssetContentOperationsTests PRIVATE
            IridiumAssetCore)
    add_test(NAME AssetContentOperationsTests
            COMMAND AssetContentOperationsTests)

    add_executable(AssetCatalogServiceTests
            "tests/assets/AssetCatalogServiceTests.cpp"
    )
    target_link_libraries(AssetCatalogServiceTests PRIVATE IridiumAssetCore)
    target_compile_definitions(AssetCatalogServiceTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetCatalogServiceTests COMMAND AssetCatalogServiceTests)

    add_executable(AssetModelPreparationServiceTests
            "tests/assets/AssetModelPreparationServiceTests.cpp"
    )
    target_link_libraries(AssetModelPreparationServiceTests PRIVATE
            IridiumAssetCore)
    target_compile_definitions(AssetModelPreparationServiceTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetModelPreparationServiceTests
            COMMAND AssetModelPreparationServiceTests)

    add_executable(AssetThumbnailServiceTests
            "tests/assets/AssetThumbnailServiceTests.cpp"
    )
    target_link_libraries(AssetThumbnailServiceTests PRIVATE
            IridiumAssetCore)
    target_compile_definitions(AssetThumbnailServiceTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetThumbnailServiceTests
            COMMAND AssetThumbnailServiceTests)

    add_executable(SceneAssetReferenceTests
            "tests/assets/SceneAssetReferenceTests.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
    )
    target_include_directories(SceneAssetReferenceTests PRIVATE
            src
    )
    target_link_libraries(SceneAssetReferenceTests PRIVATE
            IridiumAssetCore
            IridiumSceneAuthoring
            glm::glm
            nlohmann_json::nlohmann_json
    )
    add_test(NAME SceneAssetReferenceTests COMMAND SceneAssetReferenceTests)

    add_executable(IridiumEcsSceneBenchmark
            "tools/EcsSceneBenchmark.cpp"
            "src/editor/EditorComponentRegistry.cpp"
            "src/editor/CoreEditorComponentRegistry.cpp"
            "src/editor/EditorComponentDrawerRegistry.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(IridiumEcsSceneBenchmark PRIVATE
            src
    )
    target_link_libraries(IridiumEcsSceneBenchmark PRIVATE
            IridiumAssetCore
            IridiumSceneRuntime
            glm::glm
            nlohmann_json::nlohmann_json
    )
    if(WIN32)
        target_link_libraries(IridiumEcsSceneBenchmark PRIVATE Psapi)
    endif()
    target_compile_definitions(IridiumEcsSceneBenchmark PRIVATE
            IRIDIUM_BENCHMARK_CONFIGURATION="$<CONFIG>"
            IRIDIUM_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            IRIDIUM_BENCHMARK_SOURCE_COMMIT="${IRIDIUM_SOURCE_COMMIT}"
            IRIDIUM_BENCHMARK_SOURCE_DIRTY=${IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE}
    )

    add_executable(EditorTransactionBenchmark
            "tools/EditorTransactionBenchmark.cpp"
            "src/editor/EditorTransactionService.cpp"
            "src/editor/EditorSceneDocumentService.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(EditorTransactionBenchmark PRIVATE src)
    target_link_libraries(EditorTransactionBenchmark PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm
            nlohmann_json::nlohmann_json)

    add_executable(LightComponentV2Benchmark
            "tools/LightComponentV2Benchmark.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(LightComponentV2Benchmark PRIVATE src)
    target_link_libraries(LightComponentV2Benchmark PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm
            nlohmann_json::nlohmann_json)
    target_compile_definitions(LightComponentV2Benchmark PRIVATE
            IRIDIUM_BENCHMARK_CONFIGURATION="$<CONFIG>"
            IRIDIUM_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    add_executable(LightExtractorBenchmark
            "tools/LightExtractorBenchmark.cpp"
            "src/ecs/systems/TransformSystem.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(LightExtractorBenchmark PRIVATE src)
    target_link_libraries(LightExtractorBenchmark PRIVATE
            IridiumLighting
            glm::glm
            nlohmann_json::nlohmann_json)
    target_compile_definitions(LightExtractorBenchmark PRIVATE
            IRIDIUM_BENCHMARK_CONFIGURATION="$<CONFIG>"
            IRIDIUM_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    add_executable(ClusteredLightingBenchmark
            "tools/ClusteredLightingBenchmark.cpp")
    target_include_directories(ClusteredLightingBenchmark PRIVATE src)
    target_link_libraries(ClusteredLightingBenchmark PRIVATE
            IridiumLighting
            glm::glm)

    add_executable(EditorStructuralBenchmark
            "tools/EditorStructuralBenchmark.cpp"
            "src/editor/EditorComponentRegistry.cpp"
            "src/editor/CoreEditorComponentRegistry.cpp"
            "src/editor/EditorSceneHierarchy.cpp"
            "src/editor/EditorSceneCommandService.cpp"
            "src/editor/EditorSceneActions.cpp"
            "src/editor/EditorTransactionService.cpp"
            "src/editor/EditorSceneDocumentService.cpp"
            "src/scene/authoring/CoreSceneComponentAdapters.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(EditorStructuralBenchmark PRIVATE src)
    target_link_libraries(EditorStructuralBenchmark PRIVATE
            IridiumSceneAuthoring
            IridiumAssetCore
            glm::glm
            nlohmann_json::nlohmann_json)

    add_executable(ViewportLayoutTests
            "tests/editor/ViewportLayoutTests.cpp"
            "src/editor/EditorSceneActions.cpp"
            "src/editor/ViewportRenderExtent.cpp"
            "src/editor/ViewportLayout.cpp"
            "src/editor/ViewportPlacement.cpp"
            "src/vendor/imgui/imgui.cpp"
            "src/vendor/imgui/imgui_draw.cpp"
            "src/vendor/imgui/imgui_tables.cpp"
            "src/vendor/imgui/imgui_widgets.cpp"
    )
    target_include_directories(ViewportLayoutTests PRIVATE
            src
            src/vendor/imgui
    )
    target_link_libraries(ViewportLayoutTests PRIVATE
            IridiumAssetCore
            glm::glm
    )
    add_test(NAME ViewportLayoutTests COMMAND ViewportLayoutTests)

    add_executable(AssetCatalogBenchmark
            "tools/AssetCatalogBenchmark.cpp"
    )
    target_link_libraries(AssetCatalogBenchmark PRIVATE IridiumAssetCore)
    if(WIN32)
        target_link_libraries(AssetCatalogBenchmark PRIVATE Psapi)
    endif()

    add_executable(AssetCookerTests
            "tests/assets/AssetCookerTests.cpp"
    )
    target_link_libraries(AssetCookerTests PRIVATE IridiumAssetCore)
    target_compile_definitions(AssetCookerTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetCookerTests COMMAND AssetCookerTests)

    add_executable(TextureProductTests
            "tests/assets/TextureProductTests.cpp"
    )
    target_link_libraries(TextureProductTests PRIVATE IridiumAssetCore)
    add_test(NAME TextureProductTests COMMAND TextureProductTests)

    add_executable(ModelProductTests
            "tests/assets/ModelProductTests.cpp"
    )
    target_link_libraries(ModelProductTests PRIVATE IridiumAssetCore)
    add_test(NAME ModelProductTests COMMAND ModelProductTests)

    add_executable(CompiledMaterialProductTests
            "tests/assets/CompiledMaterialProductTests.cpp"
    )
    target_link_libraries(CompiledMaterialProductTests PRIVATE
            IridiumAssetCore)
    add_test(NAME CompiledMaterialProductTests
            COMMAND CompiledMaterialProductTests)

    add_executable(GltfModelImporterTests
            "tests/assets/GltfModelImporterTests.cpp"
    )
    target_link_libraries(GltfModelImporterTests PRIVATE IridiumAssetCore)
    target_compile_definitions(GltfModelImporterTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME GltfModelImporterTests COMMAND GltfModelImporterTests)

    add_executable(AssetRuntimePublisherTests
            "tests/assets/AssetRuntimePublisherTests.cpp"
    )
    target_link_libraries(AssetRuntimePublisherTests PRIVATE
            IridiumAssetCore)
    add_test(NAME AssetRuntimePublisherTests
            COMMAND AssetRuntimePublisherTests)

    add_executable(SourceChangeTrackerTests
            "tests/assets/SourceChangeTrackerTests.cpp"
    )
    target_link_libraries(SourceChangeTrackerTests PRIVATE
            IridiumAssetCore)
    add_test(NAME SourceChangeTrackerTests
            COMMAND SourceChangeTrackerTests)

    add_executable(AssetReimportSchedulerTests
            "tests/assets/AssetReimportSchedulerTests.cpp"
    )
    target_link_libraries(AssetReimportSchedulerTests PRIVATE
            IridiumAssetCore)
    add_test(NAME AssetReimportSchedulerTests
            COMMAND AssetReimportSchedulerTests)

    add_executable(SourceFileWatcherTests
            "tests/assets/SourceFileWatcherTests.cpp"
    )
    target_link_libraries(SourceFileWatcherTests PRIVATE
            IridiumAssetCore)
    add_test(NAME SourceFileWatcherTests
            COMMAND SourceFileWatcherTests)

    add_executable(AssetSourceMonitorTests
            "tests/assets/AssetSourceMonitorTests.cpp"
    )
    target_link_libraries(AssetSourceMonitorTests PRIVATE
            IridiumAssetCore)
    add_test(NAME AssetSourceMonitorTests
            COMMAND AssetSourceMonitorTests)

    add_executable(AssetRuntimeServiceTests
            "tests/assets/AssetRuntimeServiceTests.cpp"
    )
    target_link_libraries(AssetRuntimeServiceTests PRIVATE
            IridiumAssetCore)
    target_compile_definitions(AssetRuntimeServiceTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME AssetRuntimeServiceTests
            COMMAND AssetRuntimeServiceTests)

    add_executable(AssetRuntimePublisherBenchmark
            "tools/AssetRuntimePublisherBenchmark.cpp"
    )
    target_link_libraries(AssetRuntimePublisherBenchmark PRIVATE
            IridiumAssetCore)

    add_executable(IridiumCloneCookedRevisionFixture
            "tools/CloneCookedRevisionFixture.cpp"
    )
    target_link_libraries(IridiumCloneCookedRevisionFixture PRIVATE
            IridiumAssetCore)

    add_executable(AssetCookerBenchmark
            "tools/AssetCookerBenchmark.cpp"
    )
    target_link_libraries(AssetCookerBenchmark PRIVATE IridiumAssetCore)

    add_executable(SourceMaterialTests
            "tests/material/SourceMaterialTests.cpp"
            "src/material/SourceMaterial.cpp"
    )

    target_include_directories(SourceMaterialTests PRIVATE src)
    target_link_libraries(SourceMaterialTests PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(SourceMaterialTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )

    add_test(NAME SourceMaterialTests COMMAND SourceMaterialTests)

    add_executable(MaterialCompilerTests
            "tests/material/MaterialCompilerTests.cpp"
            "src/material/MaterialCompiler.cpp"
            "src/material/SourceMaterial.cpp"
            "src/renderer/color/SceneColor.cpp"
            "src/utils/Sha256.cpp"
    )

    target_include_directories(MaterialCompilerTests PRIVATE src)
    target_link_libraries(MaterialCompilerTests PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(MaterialCompilerTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )

    add_test(NAME MaterialCompilerTests COMMAND MaterialCompilerTests)

    add_executable(StandardMaterialShadingTests
            "tests/material/StandardMaterialShadingTests.cpp"
    )
    target_include_directories(StandardMaterialShadingTests PRIVATE src)
    target_link_libraries(StandardMaterialShadingTests PRIVATE glm::glm)
    target_compile_definitions(StandardMaterialShadingTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME StandardMaterialShadingTests COMMAND StandardMaterialShadingTests)

    add_executable(LightingReferenceTests
            "tests/renderer/LightingReferenceTests.cpp"
    )
    target_include_directories(LightingReferenceTests PRIVATE src)
    target_link_libraries(LightingReferenceTests PRIVATE glm::glm)
    target_compile_definitions(LightingReferenceTests PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME LightingReferenceTests COMMAND LightingReferenceTests)

    add_executable(ReflectionProbeTests
            "tests/renderer/ReflectionProbeTests.cpp"
    )
    target_link_libraries(ReflectionProbeTests PRIVATE IridiumLighting)
    add_test(NAME ReflectionProbeTests COMMAND ReflectionProbeTests)

    add_executable(MaterialRuntimeTests
            "tests/material/MaterialRuntimeTests.cpp"
            "src/material/MaterialRuntime.cpp"
            "src/material/MaterialCompiler.cpp"
            "src/material/SourceMaterial.cpp"
            "src/renderer/color/SceneColor.cpp"
            "src/utils/Sha256.cpp"
    )

    target_include_directories(MaterialRuntimeTests PRIVATE src)
    target_link_libraries(MaterialRuntimeTests PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )

    add_test(NAME MaterialRuntimeTests COMMAND MaterialRuntimeTests)

    add_executable(MaterialTextureCompatibilityTests
            "tests/material/MaterialTextureCompatibilityTests.cpp"
            "src/material/MaterialTextureCompatibility.cpp"
            "src/renderer/color/SceneColor.cpp"
    )
    target_include_directories(MaterialTextureCompatibilityTests PRIVATE src)
    target_link_libraries(MaterialTextureCompatibilityTests PRIVATE glm::glm)
    add_test(NAME MaterialTextureCompatibilityTests COMMAND MaterialTextureCompatibilityTests)

    add_executable(MaterialRuntimeBenchmark
            "tools/MaterialRuntimeBenchmark.cpp"
            "src/material/MaterialRuntime.cpp"
            "src/material/MaterialCompiler.cpp"
            "src/material/SourceMaterial.cpp"
            "src/renderer/color/SceneColor.cpp"
            "src/utils/Sha256.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(MaterialRuntimeBenchmark PRIVATE src)
    target_link_libraries(MaterialRuntimeBenchmark PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )

    add_executable(MaterialCompilerBenchmark
            "tools/MaterialCompilerBenchmark.cpp"
            "src/material/MaterialCompiler.cpp"
            "src/material/SourceMaterial.cpp"
            "src/renderer/color/SceneColor.cpp"
            "src/utils/Sha256.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(MaterialCompilerBenchmark PRIVATE src)
    target_link_libraries(MaterialCompilerBenchmark PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(MaterialCompilerBenchmark PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )

    add_executable(SourceMaterialBenchmark
            "tools/SourceMaterialBenchmark.cpp"
            "src/material/SourceMaterial.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )

    target_include_directories(SourceMaterialBenchmark PRIVATE src)
    target_link_libraries(SourceMaterialBenchmark PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(SourceMaterialBenchmark PRIVATE
            PROJECT_ROOT_DIR="${CMAKE_SOURCE_DIR}"
    )

    add_executable(BenchmarkManifestTests
            "tests/benchmarks/BenchmarkManifestTests.cpp"
            "src/benchmarks/BenchmarkManifest.cpp"
            "src/utils/Sha256.cpp"
    )

    target_include_directories(BenchmarkManifestTests PRIVATE src)
    target_link_libraries(BenchmarkManifestTests PRIVATE
            glm::glm
            nlohmann_json::nlohmann_json
    )

    add_test(NAME BenchmarkManifestTests COMMAND BenchmarkManifestTests)

    add_executable(TransformSystemTests
            "tests/ecs/TransformSystemTests.cpp"
            "src/ecs/systems/TransformSystem.cpp"
            "src/vendor/imgui/imgui.cpp"
            "src/vendor/imgui/imgui_draw.cpp"
            "src/vendor/imgui/imgui_tables.cpp"
            "src/vendor/imgui/imgui_widgets.cpp"
    )

    target_include_directories(TransformSystemTests PRIVATE
            src
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/vendor/imgui
    )
    target_link_libraries(TransformSystemTests PRIVATE glm::glm)

    add_test(NAME TransformSystemTests COMMAND TransformSystemTests)

    add_executable(EntityIdentityTests
            "tests/ecs/EntityIdentityTests.cpp"
            "src/ecs/systems/TransformSystem.cpp"
            "src/vendor/imgui/imgui.cpp"
            "src/vendor/imgui/imgui_draw.cpp"
            "src/vendor/imgui/imgui_tables.cpp"
            "src/vendor/imgui/imgui_widgets.cpp"
    )
    target_include_directories(EntityIdentityTests PRIVATE
            src
            src/vendor/imgui
    )
    target_link_libraries(EntityIdentityTests PRIVATE
            IridiumAssetCore
            IridiumSceneRuntime
            glm::glm
    )
    add_test(NAME EntityIdentityTests COMMAND EntityIdentityTests)

    add_executable(RegistryPropertyTests
            "tests/ecs/RegistryPropertyTests.cpp"
    )
    target_include_directories(RegistryPropertyTests PRIVATE src)
    add_test(NAME RegistryPropertyTests COMMAND RegistryPropertyTests)

    add_executable(CaptureArtifactTests
            "tests/capture/CaptureArtifactTests.cpp"
            "src/capture/TgaImage.cpp"
            "src/capture/PfmImage.cpp"
            "src/capture/CaptureArtifact.cpp"
            "src/utils/Sha256.cpp"
    )

    target_include_directories(CaptureArtifactTests PRIVATE src)
    target_link_libraries(CaptureArtifactTests PRIVATE
            nlohmann_json::nlohmann_json
    )

    add_test(NAME CaptureArtifactTests COMMAND CaptureArtifactTests)

    add_executable(ImageComparisonTests
            "tests/capture/ImageComparisonTests.cpp"
            "src/capture/ImageComparison.cpp"
    )

    target_include_directories(ImageComparisonTests PRIVATE src)

    add_test(NAME ImageComparisonTests COMMAND ImageComparisonTests)

    add_executable(SceneColorTests
            "tests/renderer/SceneColorTests.cpp"
            "src/renderer/color/SceneColor.cpp"
    )

    target_include_directories(SceneColorTests PRIVATE src)

    add_test(NAME SceneColorTests COMMAND SceneColorTests)

    add_executable(OutputTransformConfigTests
            "tests/renderer/OutputTransformConfigTests.cpp"
            "src/renderer/color/OutputTransformConfig.cpp"
    )
    target_include_directories(OutputTransformConfigTests PRIVATE src)
    add_test(NAME OutputTransformConfigTests COMMAND OutputTransformConfigTests)

    add_executable(RenderGraphTests
            "tests/renderer/RenderGraphTests.cpp"
    )
    target_link_libraries(RenderGraphTests PRIVATE IridiumRenderGraph)
    add_test(NAME RenderGraphTests COMMAND RenderGraphTests)

    add_executable(VulkanRenderGraphExecutorTests
            "tests/renderer/VulkanRenderGraphExecutorTests.cpp"
            "src/renderer/vulkan/VulkanRenderGraphExecutor.cpp"
            "src/renderer/vulkan/VulkanProductionRenderGraph.cpp"
            "src/renderer/vulkan/VulkanResourceAllocator.cpp"
            "src/profiling/MemoryProfile.cpp"
    )
    target_include_directories(VulkanRenderGraphExecutorTests PRIVATE src)
    target_link_libraries(VulkanRenderGraphExecutorTests PRIVATE
            IridiumRenderGraph
            Vulkan::Vulkan
    )
    add_test(NAME VulkanRenderGraphExecutorTests
            COMMAND VulkanRenderGraphExecutorTests)

    add_executable(VulkanOutputTransportTests
            "tests/renderer/VulkanOutputTransportTests.cpp"
            "src/renderer/vulkan/VulkanOutputTransport.cpp"
    )
    target_include_directories(VulkanOutputTransportTests PRIVATE src)
    target_link_libraries(VulkanOutputTransportTests PRIVATE Vulkan::Vulkan)
    add_test(NAME VulkanOutputTransportTests COMMAND VulkanOutputTransportTests)

    add_executable(RenderGraphBenchmark
            "tests/renderer/RenderGraphBenchmark.cpp"
            "src/profiling/CpuAllocationProfile.cpp"
    )
    target_include_directories(RenderGraphBenchmark PRIVATE src)
    target_link_libraries(RenderGraphBenchmark PRIVATE IridiumRenderGraph)
endif()
