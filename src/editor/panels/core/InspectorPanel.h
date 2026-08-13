#pragma once

#include "../EditorPanel.h"
#include "assets/AssetGuid.h"
#include "editor/ComponentCollectionUI.h"
#include "editor/EditorComponentDrawerRegistry.h"
#include "editor/EditorComponentRegistry.h"
#include "editor/EditorPropertyDrawer.h"
#include "editor/EditorSelectionState.h"
#include "ecs/Entity.h"

#include <array>
#include <cstddef>
#include <map>
#include <string>

namespace Iridium {
    class AssetCatalog;
    class AssetThumbnailService;
    class EditorSceneCommandService;
    class EditorTransactionService;
}

class MeshComponent;
struct TransformComponent;
struct LightComponent;
struct SkyComponent;
struct RelationshipComponent;

class InspectorPanel : public EditorPanel {
public:
    InspectorPanel(
        Iridium::EditorSelectionState* selection,
        const Iridium::AssetCatalog*
            assetCatalog,
        Iridium::AssetThumbnailService*
            thumbnailService,
        Iridium::EditorTransactionService*
            transactionService,
        Iridium::EditorSceneCommandService*
            sceneCommands);

    void OnImGuiRender(
        Registry& registry,
        Iridium::AssetManager*
            assetManager) override;

private:
    enum class AssetPickerKind {
        None,
        Model,
        Material,
    };

    void openAssetPicker(
        AssetPickerKind kind,
        Iridium::AssetGuid sourceMaterial = {});
    void drawAssetPicker(
        Registry& registry,
        Entity entity,
        MeshComponent& mesh);
    void assignMaterial(
        MeshComponent& mesh,
        Iridium::AssetGuid source,
        Iridium::AssetGuid replacement);
    [[nodiscard]] std::string materialName(
        Iridium::AssetGuid guid);
    void drawMeshComponent(
        Iridium::EditorComponentDrawContext& context);
    void drawTransformComponent(
        Iridium::EditorComponentDrawContext& context);
    void drawLightComponent(
        Iridium::EditorComponentDrawContext& context);
    void drawSkyComponent(
        Iridium::EditorComponentDrawContext& context);
    void drawReflectionProbeComponent(
        Iridium::EditorComponentDrawContext& context);
    void drawBakedLightingSetComponent(
        Iridium::EditorComponentDrawContext& context);
    static void drawRelationshipComponent(
        const RelationshipComponent& relationship);
    void resetEditActivity() noexcept;
    bool recordEditActivity(bool changed);
    [[nodiscard]] uint64_t coalescingSessionForLastEdit();

    Entity* selectedEntity = nullptr;
    Iridium::EditorSelectionState* selection_ = nullptr;
    const Iridium::AssetCatalog*
        assetCatalog_ = nullptr;
    Iridium::AssetThumbnailService*
        thumbnailService_ = nullptr;
    Iridium::EditorTransactionService*
        transactionService_ = nullptr;
    Iridium::EditorSceneCommandService*
        sceneCommands_ = nullptr;
    Entity uniformScaleEntity = NULL_ENTITY;
    bool uniformScale = false;
    Entity nameEntity_ = NULL_ENTITY;
    std::array<char, 256> nameBuffer_{};
    std::string observedName_;
    uint32_t changedItemId_ = 0;
    bool changedItemActivated_ = false;
    uint32_t activeEditItemId_ = 0;
    uint64_t activeEditSession_ = 0;
    uint64_t nextEditSession_ = 1;
    AssetPickerKind pickerKind_ =
        AssetPickerKind::None;
    bool pickerPending_ = false;
    Iridium::AssetGuid
        pickerSourceMaterial_;
    std::array<char, 256> pickerSearch_{};
    Entity materialPageEntity_ =
        NULL_ENTITY;
    Iridium::AssetGuid materialPageModel_;
    size_t materialPage_ = 0;
    Iridium::EditorComponentUI::
        CollectionViewState materialView_;
    Iridium::AssetGuid materialCatalogRoot_;
    Iridium::AssetGuid pinnedModelGuid_;
    std::map<Iridium::AssetGuid,
        std::string> materialNames_;
    Iridium::EditorComponentRegistry
        componentRegistry_;
    Iridium::EditorComponentDrawerRegistry
        drawerRegistry_;
    Iridium::EditorPropertyEditSessionState
        genericPropertySessions_;
};
