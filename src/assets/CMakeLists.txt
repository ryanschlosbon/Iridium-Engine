# iridium_assets: identity/metadata/catalog, cooker and cooked-artifact container,
# texture/environment/model/material products, importers and runtime services.
# The SQLite database is a rebuildable editor index; persistent identity remains in
# source-controlled sidecars.
add_library(iridium_assets STATIC
        AssetBrowserModel.cpp
        AssetCatalogService.cpp
        AssetContentOperations.cpp
        AssetDiscovery.cpp
        AssetImport.cpp
        AssetManager.cpp
        AssetMetadata.cpp
        MaterialProvenance.cpp
        SqliteAssetCatalog.cpp
        cooker/AssetCooker.cpp
        cooker/CanonicalSettings.cpp
        cooker/CookKey.cpp
        cooker/CookReceipt.cpp
        cooker/CookTypes.cpp
        cooker/CookedArtifact.cpp
        cooker/DependencyGraph.cpp
        cooker/ImporterRegistry.cpp
        cooker/LocalDerivedDataCache.cpp
        cooker/TextFixtureImporter.cpp
        environment/AssetEnvironmentPreparationService.cpp
        environment/BakedProbeEnvironmentImporter.cpp
        environment/EnvironmentConvolution.cpp
        environment/EnvironmentImporter.cpp
        environment/EnvironmentProduct.cpp
        lighting/BakedLightingProduct.cpp
        material/CompiledMaterialProduct.cpp
        model/AssetModelPreparationService.cpp
        model/GltfModelImporter.cpp
        model/ModelLodGenerator.cpp
        model/ModelProduct.cpp
        model/ModelRuntimeProduct.cpp
        runtime/AssetReimportScheduler.cpp
        runtime/AssetRuntimePublisher.cpp
        runtime/AssetRuntimeService.cpp
        runtime/AssetSourceMonitor.cpp
        runtime/SourceChangeTracker.cpp
        runtime/SourceFileWatcher.cpp
        texture/TextureImporter.cpp
        texture/TextureMipGenerator.cpp
        texture/TextureProduct.cpp
        thumbnail/AssetThumbnail.cpp
        thumbnail/AssetThumbnailService.cpp
        thumbnail/AssetThumbnailUploadQueue.cpp
)
target_link_libraries(iridium_assets PUBLIC
        iridium_rhi
        iridium_material
        iridium_color
        iridium_core
        iridium_vendor_gltf
        iridium_vendor_stb
        nlohmann_json::nlohmann_json
        Threads::Threads)
if(WIN32)
    target_link_libraries(iridium_assets PRIVATE winsqlite3 Bcrypt DirectXTex ole32)
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(iridium_assets PRIVATE SQLite::SQLite3)
endif()
iridium_target_pch(iridium_assets ENGINE)
