#include "SceneHierarchyPanel.h"

#include "assets/AssetBrowserModel.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorSceneHierarchy.h"
#include "editor/EditorTransactionService.h"
#include "scene/Components.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace {
    constexpr const char*
        kEntityOrderPayload =
            "IRIDIUM_SCENE_ENTITY_ORDER_V1";
}

SceneHierarchyPanel::SceneHierarchyPanel(
    Iridium::EditorSelectionState* selection,
    Iridium::EditorTransactionService* transactions,
    Iridium::EditorSceneCommandService* sceneCommands)
    : selection_(selection), transactions_(transactions),
      sceneCommands_(sceneCommands) {
    if (!selection_ || !transactions_ || !sceneCommands_) {
        throw std::invalid_argument(
            "SceneHierarchyPanel requires selection, transaction, and scene command services");
    }
}

void SceneHierarchyPanel::beginRename(
    Registry& registry,
    Entity entity) {
    auto* names =
        registry.getPool<NameComponent>();
    if (!names->has(entity)) return;
    renamingEntity_ = entity;
    renameBuffer_.fill('\0');
    const std::string& name =
        names->get(entity).name;
    std::memcpy(renameBuffer_.data(),
        name.data(),
        (std::min)(name.size(),
            renameBuffer_.size() - 1));
}

void SceneHierarchyPanel::OnImGuiRender(
    Registry& registry,
    Iridium::AssetManager*) {
    ImGui::Begin("Scene Hierarchy");
    auto* transformPool =
        registry.getPool<TransformComponent>();
    auto* namePool =
        registry.getPool<NameComponent>();
    auto* relationshipPool =
        registry.getPool<
            RelationshipComponent>();
    Entity deleteEntity = NULL_ENTITY;
    std::vector<Entity> orderedEntities;

    if (selection_->entities.size() > 1) {
        ImGui::TextDisabled("%zu entities selected (Ctrl-click to toggle)",
            selection_->entities.size());
    }

    if (transformPool) {
        const Iridium::EditorHierarchyResult hierarchy =
            Iridium::rebuildEditorSceneHierarchy(registry);
        if (hierarchy) {
            std::function<void(Entity)> append = [&](Entity entity) {
                orderedEntities.push_back(entity);
                for (Entity child : relationshipPool->get(entity).children) {
                    append(child);
                }
            };
            for (Entity root : hierarchy.roots) append(root);
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s", hierarchy.diagnostic.c_str());
            orderedEntities = transformPool->entities;
        }
        std::optional<std::pair<
            Entity, Entity>> reorder;
        std::optional<std::pair<
            Entity, Entity>> reparent;
        bool insertAfter = false;
        for (const Entity entity :
            orderedEntities) {
            std::string label =
                namePool->has(entity)
                ? namePool->get(entity).name
                : "Entity " +
                    std::to_string(entity.index());
            if (registry.getPool<
                    LightComponent>()->has(entity)) {
                label += " (Light)";
            }

            ImGui::PushID(static_cast<int>(entity.index()));
            const float indentation = relationshipPool->has(entity)
                ? static_cast<float>(relationshipPool->get(entity).depth) * 16.0f
                : 0.0f;
            if (indentation > 0.0f) ImGui::Indent(indentation);
            if (renamingEntity_ == entity) {
                ImGui::SetKeyboardFocusHere();
                const bool accepted =
                    ImGui::InputText(
                        "##entity-name",
                        renameBuffer_.data(),
                        renameBuffer_.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                if (accepted) {
                    const std::string name(
                        renameBuffer_.data());
                    if (!name.empty()) {
                        const std::string before =
                            namePool->get(entity).name;
                        const std::string after =
                            Iridium::uniqueEntityName(
                                registry, name, entity);
                        Iridium::EditorTransaction edit;
                        edit.label = "Rename Entity";
                        edit.operations.push_back(
                            Iridium::makeEditorValueOperation<std::string>(
                                "entity/name",
                                [&registry, entity]() -> std::string* {
                                    auto* pool =
                                        registry.findPool<NameComponent>();
                                    return pool && pool->has(entity)
                                        ? &pool->get(entity).name : nullptr;
                                }, before, after));
                        (void)transactions_->execute(std::move(edit));
                    }
                    renamingEntity_ =
                        NULL_ENTITY;
                }
                if (ImGui::IsKeyPressed(
                        ImGuiKey_Escape)) {
                    renamingEntity_ =
                        NULL_ENTITY;
                }
            }
            else if (ImGui::Selectable(
                    label.c_str(),
                    selection_->contains(entity))) {
                if (ImGui::GetIO().KeyCtrl) selection_->toggle(entity);
                else selection_->selectExclusive(entity);
            }
            const ImVec2 rowMinimum =
                ImGui::GetItemRectMin();
            const ImVec2 rowMaximum =
                ImGui::GetItemRectMax();
            if (renamingEntity_ != entity &&
                ImGui::BeginDragDropSource(
                    ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload(
                    kEntityOrderPayload,
                    &entity, sizeof(entity));
                ImGui::Text(
                    "Move %s",
                    label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            kEntityOrderPayload)) {
                    if (payload->DataSize ==
                            sizeof(Entity)) {
                        const Entity source =
                            *static_cast<
                                const Entity*>(
                                payload->Data);
                        if (source != entity) {
                            const float rowHeight =
                                rowMaximum.y - rowMinimum.y;
                            const float rowPosition = rowHeight > 0.0f
                                ? (ImGui::GetMousePos().y - rowMinimum.y) /
                                    rowHeight
                                : 0.5f;
                            if (rowPosition >= 0.25f && rowPosition <= 0.75f) {
                                reparent = std::pair{ source, entity };
                            }
                            else {
                                reorder = std::pair{ source, entity };
                                insertAfter = rowPosition > 0.5f;
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem(
                    "entity-context")) {
                if (ImGui::MenuItem(
                        "Rename", "F2")) {
                    beginRename(
                        registry, entity);
                }
                if (ImGui::MenuItem("Duplicate", "Ctrl+D") &&
                    sceneCommands_) {
                    (void)sceneCommands_->duplicateEntity(entity);
                }
                if (ImGui::MenuItem("Delete")) {
                    deleteEntity = entity;
                }
                ImGui::EndPopup();
            }
            if (indentation > 0.0f) ImGui::Unindent(indentation);
            ImGui::PopID();
        }
        if (reorder && sceneCommands_) {
            (void)sceneCommands_->reorder(
                reorder->first, reorder->second, insertAfter);
        }
        if (reparent && sceneCommands_) {
            (void)sceneCommands_->reparent(
                reparent->first, reparent->second);
        }
    }

    if (selection_->primary != NULL_ENTITY &&
        ImGui::IsWindowFocused(
            ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_F2)) {
        beginRename(registry,
            selection_->primary);
    }

    // The entire hierarchy surface accepts model assets. The custom target
    // avoids a dedicated drop button and remains active over entity rows.
    ImGuiWindow* hierarchyWindow =
        ImGui::GetCurrentWindow();
    if (ImGui::BeginDragDropTargetCustom(
            hierarchyWindow->InnerRect,
            ImGui::GetID(
                "##hierarchy-model-drop"))) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    Iridium::
                        kAssetBrowserDragPayloadType
                            .data())) {
            const auto decoded =
                Iridium::decodeAssetDragPayload(
                    payload->DataType,
                    std::span(
                        static_cast<const std::byte*>(
                            payload->Data),
                        static_cast<size_t>(
                            payload->DataSize)),
                    Iridium::AssetDragKind::Model);
            if (decoded) {
                if (sceneCommands_) {
                    (void)sceneCommands_->createModel(decoded->guid);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (deleteEntity != NULL_ENTITY) {
        if (sceneCommands_) {
            (void)sceneCommands_->deleteEntity(deleteEntity);
        }
        if (renamingEntity_ == deleteEntity) {
            renamingEntity_ = NULL_ENTITY;
        }
    }
    ImGui::End();
}
