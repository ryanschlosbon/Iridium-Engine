# Tests link the engine module libraries; no production source is compiled here.
#
# ctest labels:
#   unit     (default) self-contained, no GPU
#   vulkan   needs a Vulkan device
#   process  spawns child processes (cmake -P drivers, re-invoked executables)
#   slow     takes more than ~5 s (in either configuration)
#
# Every test reads PROJECT_ROOT_DIR from the directory-level definition
# ("<source>/"). Several targets used to add PROJECT_ROOT_DIR="<source>" (no
# trailing slash) as well; the directory definition was emitted last and won, so
# those duplicate definitions were dead and are gone.

# --- core / ecs / profiling --------------------------------------------------------
iridium_add_test(ApplicationConfigTests
        SOURCES core/ApplicationConfigTests.cpp
        LIBS iridium_app)

iridium_add_test(TransformSystemTests
        SOURCES ecs/TransformSystemTests.cpp
        LIBS iridium_scene iridium_vendor_imgui)

iridium_add_test(EntityIdentityTests
        SOURCES ecs/EntityIdentityTests.cpp
        LIBS iridium_scene iridium_assets iridium_vendor_imgui)

iridium_add_test(RegistryPropertyTests
        SOURCES ecs/RegistryPropertyTests.cpp
        LIBS iridium_ecs)

iridium_add_test(CpuProfilerTests
        SOURCES profiling/CpuProfilerTests.cpp
        LIBS iridium_profiling)

iridium_add_test(CpuAllocationProfileTests
        SOURCES profiling/CpuAllocationProfileTests.cpp
        ALLOCATION_PROFILE)

iridium_add_executable(CpuAllocationProfileBenchmark
        SOURCES profiling/CpuAllocationProfileBenchmark.cpp
        ALLOCATION_PROFILE)

iridium_add_executable(CpuProfilerBenchmark
        SOURCES profiling/CpuProfilerBenchmark.cpp
        LIBS iridium_profiling)

# --- capture / benchmarks ---------------------------------------------------------
iridium_add_test(CaptureArtifactTests
        SOURCES capture/CaptureArtifactTests.cpp
        LIBS iridium_capture)

iridium_add_test(ImageComparisonTests
        SOURCES capture/ImageComparisonTests.cpp
        LIBS iridium_capture)

iridium_add_test(BenchmarkManifestTests
        SOURCES benchmarks/BenchmarkManifestTests.cpp
        LIBS iridium_benchmarks)

# --- material ------------------------------------------------------------------------
iridium_add_test(SourceMaterialTests
        SOURCES material/SourceMaterialTests.cpp
        LIBS iridium_material)

iridium_add_test(MaterialCompilerTests
        SOURCES material/MaterialCompilerTests.cpp
        LIBS iridium_material)

iridium_add_test(MaterialRuntimeTests
        SOURCES material/MaterialRuntimeTests.cpp
        LIBS iridium_material)

iridium_add_test(MaterialTextureCompatibilityTests
        SOURCES material/MaterialTextureCompatibilityTests.cpp
        LIBS iridium_material)

iridium_add_test(StandardMaterialShadingTests
        SOURCES material/StandardMaterialShadingTests.cpp
        LIBS iridium_material)

# --- renderer ------------------------------------------------------------------------
iridium_add_test(AcesOutputLutTests
        SOURCES renderer/AcesOutputLutTests.cpp
        LIBS iridium_color)

iridium_add_test(SceneColorTests
        SOURCES renderer/SceneColorTests.cpp
        LIBS iridium_color)

iridium_add_test(OutputTransformConfigTests
        SOURCES renderer/OutputTransformConfigTests.cpp
        LIBS iridium_color)

iridium_add_test(RenderGraphTests
        SOURCES renderer/RenderGraphTests.cpp
        LIBS iridium_graph)

iridium_add_executable(RenderGraphBenchmark
        SOURCES renderer/RenderGraphBenchmark.cpp
        LIBS iridium_graph
        ALLOCATION_PROFILE)

iridium_add_test(LightingReferenceTests
        SOURCES renderer/LightingReferenceTests.cpp)

iridium_add_test(GpuSceneTests
        SOURCES renderer/GpuSceneTests.cpp
        LIBS iridium_rhi iridium_assets)

iridium_add_test(GpuSceneVisibilityTests
        SOURCES renderer/GpuSceneVisibilityTests.cpp
        LIBS iridium_rhi)

iridium_add_test(GpuSceneIndirectTests
        SOURCES renderer/GpuSceneIndirectTests.cpp
        LIBS iridium_rhi)

iridium_add_test(GpuSceneLodTests
        SOURCES renderer/GpuSceneLodTests.cpp
        LIBS iridium_rhi)

iridium_add_test(DepthPyramidTests
        SOURCES renderer/DepthPyramidTests.cpp
        LIBS iridium_rhi)

iridium_add_test(GeometryArenaTests
        SOURCES renderer/GeometryArenaTests.cpp
        LIBS iridium_rhi iridium_assets)

iridium_add_test(VirtualShadowMapTests
        SOURCES renderer/VirtualShadowMapTests.cpp
        LIBS iridium_lighting)

iridium_add_test(LightExtractorTests
        SOURCES renderer/LightExtractorTests.cpp
        LIBS iridium_lighting)

iridium_add_test(ClusteredLightingTests
        SOURCES renderer/ClusteredLightingTests.cpp
        LIBS iridium_lighting)

iridium_add_test(DirectionalShadowTests
        SOURCES renderer/DirectionalShadowTests.cpp
        LIBS iridium_lighting)

iridium_add_test(LocalShadowTests
        SOURCES renderer/LocalShadowTests.cpp
        LIBS iridium_lighting)

iridium_add_test(ReflectionProbeTests
        SOURCES renderer/ReflectionProbeTests.cpp
        LIBS iridium_lighting)

iridium_add_test(Stage3ArchitectureTests
        SOURCES renderer/Stage3ArchitectureTests.cpp
        LIBS iridium_vulkan
        ALLOCATION_PROFILE)

iridium_add_test(VulkanRenderGraphExecutorTests
        SOURCES renderer/VulkanRenderGraphExecutorTests.cpp
        LIBS iridium_vulkan)

iridium_add_test(VulkanOutputTransportTests
        SOURCES renderer/VulkanOutputTransportTests.cpp
        LIBS iridium_vulkan)

# Hardware qualification: these create a Vulkan device and load SPIR-V from
# assets/shaders, so they carry the `vulkan` label and depend on iridium_shaders.
# Both exceed 5 s in Debug (VulkanVirtualShadowMarkingTests ~95 s), hence `slow`.
if(TARGET iridium_shaders)
    iridium_add_test(VulkanDepthPyramidTests
            SOURCES renderer/VulkanDepthPyramidTests.cpp
            LIBS iridium_rhi Vulkan::Vulkan
            LABELS vulkan slow)
    add_dependencies(VulkanDepthPyramidTests iridium_shaders)

    iridium_add_test(VulkanVirtualShadowMarkingTests
            SOURCES renderer/VulkanVirtualShadowMarkingTests.cpp
            LIBS iridium_vulkan iridium_lighting
            LABELS vulkan slow)
    add_dependencies(VulkanVirtualShadowMarkingTests iridium_shaders)
endif()

# --- assets --------------------------------------------------------------------------
foreach(_asset_test IN ITEMS
        AssetBrowserModelTests
        AssetCatalogServiceTests
        AssetCatalogTests
        AssetContentOperationsTests
        AssetModelPreparationServiceTests
        AssetReimportSchedulerTests
        AssetRuntimePublisherTests
        AssetRuntimeServiceTests
        AssetSourceMonitorTests
        AssetThumbnailServiceTests
        BakedLightingProductTests
        CompiledMaterialProductTests
        GltfModelImporterTests
        MaterialProvenanceTests
        ModelProductTests
        SourceChangeTrackerTests
        SourceFileWatcherTests
        TextureProductTests)
    iridium_add_test(${_asset_test}
            SOURCES assets/${_asset_test}.cpp
            LIBS iridium_assets)
endforeach()

# Re-invokes itself as child processes to check cross-process cook determinism.
iridium_add_test(AssetCookerTests
        SOURCES assets/AssetCookerTests.cpp
        LIBS iridium_assets
        LABELS process)

iridium_add_test(SceneAssetReferenceTests
        SOURCES assets/SceneAssetReferenceTests.cpp
        LIBS iridium_scene iridium_assets)

# --- scene ---------------------------------------------------------------------------
foreach(_scene_test IN ITEMS
        AtomicSourceSceneFileTests
        CookedSceneTests
        CoreComponentRegistryTests
        CoreSceneComponentAdaptersTests
        LightComponentV2Tests
        M4ProductionCutoverTests
        M5FixtureContractTests
        RuntimeComponentRegistryTests
        SceneRuntimeBoundaryTests
        SourceComponentRegistryTests
        SourceJsonParserTests
        SourceSceneCaptureTests
        SourceSceneDocumentTests
        SourceSceneEnvelopeMigratorTests
        SourceSceneLoadTransactionTests)
    iridium_add_test(${_scene_test}
            SOURCES scene/${_scene_test}.cpp
            LIBS iridium_scene)
endforeach()

iridium_add_test(M6FixtureContractTests
        SOURCES scene/M6FixtureContractTests.cpp
        LIBS iridium_assets)

add_test(NAME CookedSceneProcessDeterminism
        COMMAND ${CMAKE_COMMAND}
            -DCOOKED_SCENE_TEST=$<TARGET_FILE:CookedSceneTests>
            -DOUTPUT_DIRECTORY=${CMAKE_BINARY_DIR}/m4-process-determinism
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scene/CookedSceneProcessDeterminism.cmake
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(CookedSceneProcessDeterminism PROPERTIES LABELS process)

add_test(NAME CookSceneCliTests
        COMMAND ${CMAKE_COMMAND}
            -DCOOK_SCENE=$<TARGET_FILE:IridiumCookScene>
            -DINSPECT_SCENE=$<TARGET_FILE:IridiumInspectCookedScene>
            -DTEST_ROOT=${CMAKE_BINARY_DIR}/m4-cook-scene-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/scene/CookSceneCliTests.cmake
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(CookSceneCliTests PROPERTIES LABELS process)

# --- editor --------------------------------------------------------------------------
foreach(_editor_test IN ITEMS
        EditorAssetDocumentServiceTests
        EditorAssetSettingsTransactionServiceTests
        EditorComponentRegistryTests
        EditorSceneDocumentServiceTests
        EditorTransactionServiceTests)
    iridium_add_test(${_editor_test}
            SOURCES editor/${_editor_test}.cpp
            LIBS iridium_editor)
endforeach()

# Defines GLM_FORCE_DEPTH_ZERO_TO_ONE before its first include, so it cannot use a
# PCH that contains GLM.
iridium_add_test(ViewportLayoutTests
        SOURCES editor/ViewportLayoutTests.cpp
        LIBS iridium_editor iridium_vendor_imgui
        NO_PCH)
