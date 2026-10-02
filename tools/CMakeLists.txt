# Command-line tools and benchmarks. They link the engine module libraries; no
# production source is compiled here.

set(_iridium_psapi)
if(WIN32)
    set(_iridium_psapi Psapi)
endif()

# --- Asset and scene tools -----------------------------------------------------------
foreach(_tool IN ITEMS
        AssetCatalogInspect
        CookAsset
        BakeEnvironment
        CompareEnvironment
        InspectAssetSource
        ImportAsset
        InspectCookedModel
        InspectCookedScene)
    iridium_add_executable(Iridium${_tool}
            SOURCES ${_tool}.cpp
            LIBS iridium_assets)
endforeach()

iridium_add_executable(IridiumCookScene
        SOURCES CookScene.cpp
        LIBS iridium_scene iridium_assets)

iridium_add_executable(IridiumImageCompare
        SOURCES ImageCompare.cpp
        LIBS iridium_capture)

# --- Benchmarks ------------------------------------------------------------------------
iridium_add_executable(IridiumSourceSceneBenchmark
        SOURCES SourceSceneBenchmark.cpp
        LIBS iridium_scene ${_iridium_psapi}
        ALLOCATION_PROFILE
        BENCHMARK_INFO)

iridium_add_executable(IridiumCookedSceneBenchmark
        SOURCES CookedSceneBenchmark.cpp
        LIBS iridium_scene iridium_assets ${_iridium_psapi}
        ALLOCATION_PROFILE
        BENCHMARK_SOURCE_INFO)

iridium_add_executable(BakedLightingProductBenchmark
        SOURCES BakedLightingProductBenchmark.cpp
        LIBS iridium_assets)

if(BUILD_TESTING)
    iridium_add_executable(IridiumEcsSceneBenchmark
            SOURCES EcsSceneBenchmark.cpp
            LIBS iridium_editor iridium_scene iridium_assets ${_iridium_psapi}
            ALLOCATION_PROFILE
            BENCHMARK_SOURCE_INFO)

    iridium_add_executable(EditorTransactionBenchmark
            SOURCES EditorTransactionBenchmark.cpp
            LIBS iridium_editor
            ALLOCATION_PROFILE)

    iridium_add_executable(EditorStructuralBenchmark
            SOURCES EditorStructuralBenchmark.cpp
            LIBS iridium_editor
            ALLOCATION_PROFILE)

    iridium_add_executable(LightComponentV2Benchmark
            SOURCES LightComponentV2Benchmark.cpp
            LIBS iridium_scene iridium_assets
            ALLOCATION_PROFILE
            BENCHMARK_INFO)

    iridium_add_executable(LightExtractorBenchmark
            SOURCES LightExtractorBenchmark.cpp
            LIBS iridium_lighting
            ALLOCATION_PROFILE
            BENCHMARK_INFO)

    iridium_add_executable(ClusteredLightingBenchmark
            SOURCES ClusteredLightingBenchmark.cpp
            LIBS iridium_lighting)

    iridium_add_executable(AssetCatalogBenchmark
            SOURCES AssetCatalogBenchmark.cpp
            LIBS iridium_assets ${_iridium_psapi})

    iridium_add_executable(AssetRuntimePublisherBenchmark
            SOURCES AssetRuntimePublisherBenchmark.cpp
            LIBS iridium_assets)

    iridium_add_executable(IridiumCloneCookedRevisionFixture
            SOURCES CloneCookedRevisionFixture.cpp
            LIBS iridium_assets)

    iridium_add_executable(AssetCookerBenchmark
            SOURCES AssetCookerBenchmark.cpp
            LIBS iridium_assets)

    foreach(_material_benchmark IN ITEMS
            MaterialRuntimeBenchmark
            MaterialCompilerBenchmark
            SourceMaterialBenchmark)
        iridium_add_executable(${_material_benchmark}
                SOURCES ${_material_benchmark}.cpp
                LIBS iridium_material
                ALLOCATION_PROFILE)
    endforeach()
endif()
