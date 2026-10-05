# iridium_editor: editor services (transactions, documents, component registry),
# viewport helpers and ImGui panels.
add_library(iridium_editor STATIC
        CoreEditorComponentRegistry.cpp
        EditorAssetDocumentService.cpp
        EditorAssetSettingsTransactionService.cpp
        EditorComponentDrawerRegistry.cpp
        EditorComponentRegistry.cpp
        EditorHost.cpp
        EditorMeshTransaction.cpp
        EditorOrbitCamera.cpp
        EditorPropertyDrawer.cpp
        EditorPropertyTransaction.cpp
        EditorSceneActions.cpp
        EditorSceneCommandService.cpp
        EditorSceneDocumentActions.cpp
        EditorSceneDocumentService.cpp
        EditorSceneHierarchy.cpp
        EditorSystem.cpp
        EditorTransactionService.cpp
        ViewportLayout.cpp
        ViewportPlacement.cpp
        ViewportRenderExtent.cpp
        panels/core/InspectorPanel.cpp
        panels/core/SceneHierarchyPanel.cpp
        panels/core/ViewPortPanel.cpp
        panels/menus/MenuBarPanel.cpp
        panels/windows/AssetBrowserPanel.cpp
        panels/windows/AssetViewerPanel.cpp
        panels/windows/ConsolePanel.cpp
        panels/windows/MaterialDiagnosticsPanel.cpp
        panels/windows/ProfilerPanel.cpp
        panels/windows/ProjectSettingsPanel.cpp
)
target_link_libraries(iridium_editor PUBLIC
        iridium_scene_authoring
        iridium_assets
        iridium_rhi
        iridium_material
        iridium_color
        iridium_ecs
        iridium_platform
        iridium_profiling
        iridium_vendor_imgui
        nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_editor ENGINE)
# ViewportPicking.h defines GLM_FORCE_DEPTH_ZERO_TO_ONE before its first GLM
# include; a PCH containing GLM would silently switch this file to [-1, 1] clip
# depth.
set_source_files_properties(panels/core/ViewPortPanel.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
# EditorHost.cpp builds the scene projection the editor draws against (M7R
# R5a.2) and defines GLM_FORCE_DEPTH_ZERO_TO_ONE the same way.
set_source_files_properties(EditorHost.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
