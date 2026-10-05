# iridium_scene: scene world and identity, runtime component registry and cooked
# scenes (scene/runtime), and scene systems. JSON-free by design (ADR-0004/M4):
# runtime builds consume cooked binary scenes.
add_library(iridium_scene STATIC
        SceneIdentityMap.cpp
        SceneWorld.cpp
        runtime/ComponentIdentity.cpp
        runtime/CookedComponentIO.cpp
        runtime/CookedScene.cpp
        runtime/CoreComponentRegistry.cpp
        runtime/RuntimeComponentRegistry.cpp
        runtime/SceneDiagnostic.cpp
        runtime/SceneReferenceState.cpp
        systems/TransformSystem.cpp
)
target_link_libraries(iridium_scene PUBLIC
        iridium_ecs
        iridium_assets
        iridium_rhi
        iridium_tasks
        iridium_core)
iridium_target_pch(iridium_scene ENGINE)

# iridium_scene_authoring: versioned JSON source-scene codecs, migration, capture,
# load transactions and the cooked-scene compiler (editor and tools only).
add_library(iridium_scene_authoring STATIC
        authoring/AtomicSourceSceneFile.cpp
        authoring/CookedSceneCompiler.cpp
        authoring/CoreComponentCodecs.cpp
        authoring/CoreSceneComponentAdapters.cpp
        authoring/SourceComponentRegistry.cpp
        authoring/SourceJsonParser.cpp
        authoring/SourceSceneCapture.cpp
        authoring/SourceSceneDocument.cpp
        authoring/SourceSceneEnvelopeMigrator.cpp
        authoring/SourceSceneLoadTransaction.cpp
)
target_link_libraries(iridium_scene_authoring PUBLIC
        iridium_scene
        nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_scene_authoring ENGINE)
