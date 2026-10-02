# iridium_scene: scene world and identity, runtime component registry and cooked
# scenes (scene/runtime), source-scene authoring codecs (scene/authoring) and
# scene systems.
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
        systems/TransformSystem.cpp
)
target_link_libraries(iridium_scene PUBLIC
        iridium_ecs
        iridium_assets
        iridium_rhi
        iridium_core
        nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_scene ENGINE)
