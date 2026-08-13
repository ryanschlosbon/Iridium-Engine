#include "InspectorPanel.h"

#include "assets/AssetBrowserModel.h"
#include "assets/AssetCatalog.h"
#include "assets/AssetManager.h"
#include "assets/thumbnail/AssetThumbnailService.h"
#include "editor/ComponentCollectionUI.h"
#include "editor/CoreEditorComponentRegistry.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorTransactionService.h"
#include "editor/EditorMeshTransaction.h"
#include "editor/EditorPropertyDrawer.h"
#include "editor/Reflection.h"
#include "renderer/rhi/Mesh.h"
#include "scene/Components.h"
#include "scene/lighting/LightPhotometry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>

#include <imgui.h>

namespace {

    void explainLastItem(const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", text);
        }
    }

    void commitMeshAuthoringEdit(
        Iridium::EditorTransactionService* transactions,
        Registry& registry, std::span<const Entity> entities,
        Entity entity, MeshComponent& mesh,
        Iridium::EditorMeshAuthoringState before,
        Iridium::EditorMeshAuthoringState after,
        std::string label, std::string coalescingKey = {},
        uint64_t coalescingSession = 0) {
        if (Iridium::sameEditorMeshAuthoringState(before, after)) return;
        Iridium::restoreRawEditorMeshAuthoringState(mesh, before);
        if (!transactions) return;
        Iridium::EditorTransaction edit;
        edit.label = std::move(label);
        edit.coalescingKey = std::move(coalescingKey);
        edit.coalescingSession = coalescingSession;
        const bool enabledChanged = before.enabled != after.enabled;
        const bool modelChanged = before.modelGuid != after.modelGuid;
        const bool materialsChanged =
            before.materialOverrides != after.materialOverrides;
        const auto appendTarget = [&](Entity target) {
            auto* pool = registry.findPool<MeshComponent>();
            if (!pool || !pool->has(target)) return;
            const auto targetBefore =
                Iridium::captureEditorMeshAuthoringState(pool->get(target));
            auto targetAfter = targetBefore;
            if (enabledChanged) targetAfter.enabled = after.enabled;
            if (modelChanged) targetAfter.modelGuid = after.modelGuid;
            if (materialsChanged) {
                targetAfter.materialOverrides = after.materialOverrides;
            }
            edit.operations.push_back(
                Iridium::makeEditorMeshAuthoringOperation(
                    registry, target, targetBefore, targetAfter));
        };
        if (entities.empty()) appendTarget(entity);
        else for (Entity target : entities) appendTarget(target);
        (void)transactions->execute(std::move(edit));
    }

}

InspectorPanel::InspectorPanel(
    Iridium::EditorSelectionState* selection,
    const Iridium::AssetCatalog*
        assetCatalog,
    Iridium::AssetThumbnailService*
        thumbnailService,
    Iridium::EditorTransactionService*
        transactionService,
    Iridium::EditorSceneCommandService*
        sceneCommands)
    : selectedEntity(&selection->primary),
      selection_(selection),
      assetCatalog_(assetCatalog),
      thumbnailService_(thumbnailService),
      transactionService_(transactionService),
      sceneCommands_(sceneCommands) {
    if (!selection_ || !transactionService_ || !sceneCommands_) {
        throw std::invalid_argument(
            "InspectorPanel requires selection, transaction, and scene command services");
    }
    auto core = Iridium::createCoreEditorComponentRegistry();
    if (!core) {
        throw std::logic_error(core.status.message);
    }
    componentRegistry_ = std::move(core.registry);

    Iridium::CoreEditorComponentDrawerCallbacks callbacks;
    callbacks.transform =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawTransformComponent(context);
        };
    callbacks.mesh =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawMeshComponent(context);
        };
    callbacks.relationship =
        [](Iridium::EditorComponentDrawContext& context) {
            drawRelationshipComponent(
                *static_cast<RelationshipComponent*>(context.component));
        };
    callbacks.light =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawLightComponent(context);
        };
    callbacks.sky =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawSkyComponent(context);
        };
    callbacks.reflectionProbe =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawReflectionProbeComponent(context);
        };
    callbacks.bakedLightingSet =
        [this](Iridium::EditorComponentDrawContext& context) {
            drawBakedLightingSetComponent(context);
        };
    auto drawers = Iridium::createCoreEditorComponentDrawerRegistry(
        componentRegistry_, std::move(callbacks));
    if (!drawers) {
        throw std::logic_error(drawers.status.message);
    }
    drawerRegistry_ = std::move(drawers.registry);
}

void InspectorPanel::openAssetPicker(
    AssetPickerKind kind,
    Iridium::AssetGuid sourceMaterial) {
    pickerKind_ = kind;
    pickerSourceMaterial_ =
        sourceMaterial;
    pickerPending_ = true;
    pickerSearch_.fill('\0');
}

void InspectorPanel::assignMaterial(
    MeshComponent& mesh,
    Iridium::AssetGuid source,
    Iridium::AssetGuid replacement) {
    const auto existing =
        std::ranges::find_if(
            mesh.materialOverrides,
            [source](const MeshComponent::
                MaterialOverride& candidate) {
                return candidate
                    .sourceMaterialGuid == source;
            });
    if (existing !=
            mesh.materialOverrides.end()) {
        existing->materialGuid =
            replacement;
    }
    else {
        mesh.materialOverrides.push_back({
            .sourceMaterialGuid = source,
            .materialGuid = replacement,
        });
    }
    mesh.requestedMaterialAssetRoots.clear();
}

std::string InspectorPanel::materialName(
    Iridium::AssetGuid guid) {
    if (const auto found =
            materialNames_.find(guid);
        found != materialNames_.end()) {
        return found->second;
    }
    if (assetCatalog_) {
        const auto records =
            assetCatalog_->recordsForGuid(
                guid);
        const auto material =
            std::ranges::find_if(
                records,
                [](const Iridium::
                        AssetCatalogRecord&
                        record) {
                    return record.assetType ==
                        "iridium.material";
                });
        if (material != records.end()) {
            materialNames_.emplace(
                guid,
                material->displayName);
            return material->displayName;
        }
    }
    return "Material";
}

void InspectorPanel::drawAssetPicker(
    Registry& registry,
    Entity entity,
    MeshComponent& mesh) {
    if (pickerKind_ ==
        AssetPickerKind::None) {
        return;
    }
    const char* title =
        pickerKind_ ==
            AssetPickerKind::Model
        ? "Choose Model Asset"
        : "Choose Material Asset";
    if (pickerPending_) {
        ImGui::OpenPopup(title);
        pickerPending_ = false;
    }
    ImGui::SetNextWindowSize(
        ImVec2(620.0f, 470.0f),
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            title, nullptr,
            ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    ImGui::InputTextWithHint(
        "##asset-picker-search",
        "Search project assets",
        pickerSearch_.data(),
        pickerSearch_.size());
    ImGui::Separator();
    if (!assetCatalog_) {
        ImGui::TextDisabled(
            "The project catalog is unavailable.");
    }
    else if (ImGui::BeginChild(
            "asset-picker-results",
            ImVec2(0.0f, -38.0f),
            ImGuiChildFlags_Borders)) {
        Iridium::AssetCatalogQuery query{
            .text = pickerSearch_.data(),
            .assetType =
                pickerKind_ ==
                    AssetPickerKind::Model
                ? "iridium.model"
                : "iridium.material",
            .status =
                Iridium::AssetCatalogStatus::
                    Ready,
            .limit = 200,
            .calculateTotalMatches = false,
        };
        const Iridium::AssetCatalogQueryPage
            page = assetCatalog_->query(query);
        for (const Iridium::AssetCatalogRecord&
                record : page.records) {
            if (pickerKind_ ==
                    AssetPickerKind::Model &&
                record.parentGuid) {
                continue;
            }
            ImGui::PushID(
                record.guid.toString().c_str());
            if (ImGui::Selectable(
                    record.displayName.c_str())) {
                const Iridium::EditorMeshAuthoringState before =
                    Iridium::captureEditorMeshAuthoringState(mesh);
                if (pickerKind_ ==
                    AssetPickerKind::Model) {
                    mesh.requestedAssetGuid =
                        record.guid;
                    mesh.materialOverrides.clear();
                    mesh.requestedMaterialAssetRoots
                        .clear();
                }
                else {
                    assignMaterial(
                        mesh,
                        pickerSourceMaterial_,
                        record.guid);
                }
                commitMeshAuthoringEdit(transactionService_, registry,
                    selection_->selected(), entity, mesh, before,
                    Iridium::captureEditorMeshAuthoringState(mesh),
                    pickerKind_ == AssetPickerKind::Model
                        ? "Assign Model" : "Assign Material");
                pickerKind_ =
                    AssetPickerKind::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::TextDisabled(
                "%s%s%s",
                record.sourcePath.c_str(),
                record.sourceKey.empty()
                    ? "" : " : ",
                record.sourceKey.c_str());
            ImGui::PopID();
        }
        if (page.records.empty()) {
            ImGui::TextDisabled(
                "No matching assets.");
        }
    }
    ImGui::EndChild();

    if (ImGui::Button(
            "Cancel",
            ImVec2(100.0f, 0.0f))) {
        pickerKind_ =
            AssetPickerKind::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void InspectorPanel::resetEditActivity() noexcept {
    changedItemId_ = 0;
    changedItemActivated_ = false;
}

bool InspectorPanel::recordEditActivity(bool changed) {
    if (changed) {
        changedItemId_ = ImGui::GetItemID();
        changedItemActivated_ = ImGui::IsItemActivated();
    }
    return changed;
}

uint64_t InspectorPanel::coalescingSessionForLastEdit() {
    if (changedItemId_ == 0) return 0;
    if (changedItemActivated_ || changedItemId_ != activeEditItemId_) {
        activeEditItemId_ = changedItemId_;
        activeEditSession_ = nextEditSession_++;
        if (nextEditSession_ == 0) nextEditSession_ = 1;
    }
    return activeEditSession_;
}

void InspectorPanel::drawTransformComponent(
    Iridium::EditorComponentDrawContext& context) {
    auto& transform = *static_cast<TransformComponent*>(context.component);
    const TransformComponent before = transform;
    const Entity entity = context.entity;
    resetEditActivity();
    if (uniformScaleEntity != entity) {
        uniformScaleEntity = entity;
        uniformScale = false;
    }
    bool changed = recordEditActivity(Reflection::DrawField(
        "Position", transform.position));
    changed |= recordEditActivity(Reflection::DrawField(
        "Rotation", transform.rotation));
    const bool uniformChanged = ImGui::Checkbox(
        "Uniform scale", &uniformScale);
    if (uniformChanged && uniformScale) {
        transform.scale = glm::vec3(transform.scale.x);
        changed = true;
        (void)recordEditActivity(true);
    }
    if (uniformScale) {
        float scale = transform.scale.x;
        if (Reflection::DrawField("Scale", scale)) {
            transform.scale = glm::vec3(scale);
            changed = true;
            (void)recordEditActivity(true);
        }
    }
    else {
        changed |= recordEditActivity(
            Reflection::DrawField("Scale", transform.scale));
    }
    transform.isDirty |= changed;
    if (!changed || !transactionService_) return;

    TransformComponent after = transform;
    TransformComponent undo = before;
    undo.isDirty = true;
    after.isDirty = true;
    transform = before;
    const auto equalTransform = [](const TransformComponent& lhs,
        const TransformComponent& rhs) {
        return glm::all(glm::equal(lhs.position, rhs.position)) &&
            glm::all(glm::equal(lhs.rotation, rhs.rotation)) &&
            glm::all(glm::equal(lhs.scale, rhs.scale));
    };
    Iridium::EditorTransaction edit;
    edit.label = "Edit Transform";
    edit.coalescingKey = "transform/" + std::to_string(entity.index()) +
        ":" + std::to_string(entity.generation()) + "/" +
        std::to_string(changedItemId_);
    edit.coalescingSession = coalescingSessionForLastEdit();
    const bool positionChanged = !glm::all(glm::equal(
        undo.position, after.position));
    const bool rotationChanged = !glm::all(glm::equal(
        undo.rotation, after.rotation));
    const bool scaleChanged = !glm::all(glm::equal(
        undo.scale, after.scale));
    const auto appendTarget = [&](Entity target) {
        auto* pool = context.registry.findPool<TransformComponent>();
        if (!pool || !pool->has(target)) return;
        TransformComponent targetBefore = pool->get(target);
        TransformComponent targetAfter = targetBefore;
        targetBefore.isDirty = true;
        if (positionChanged) targetAfter.position = after.position;
        if (rotationChanged) targetAfter.rotation = after.rotation;
        if (scaleChanged) targetAfter.scale = after.scale;
        targetAfter.isDirty = true;
        edit.operations.push_back(
            Iridium::makeEditorValueOperation<TransformComponent>(
                "entity/transform",
                [&registry = context.registry, target]()
                    -> TransformComponent* {
                    auto* targetPool =
                        registry.findPool<TransformComponent>();
                    return targetPool && targetPool->has(target)
                        ? &targetPool->get(target) : nullptr;
                }, std::move(targetBefore), std::move(targetAfter),
                equalTransform));
    };
    if (selection_->selected().empty()) appendTarget(entity);
    else for (Entity target : selection_->selected()) appendTarget(target);
    (void)transactionService_->execute(std::move(edit));
}

void InspectorPanel::drawLightComponent(
    Iridium::EditorComponentDrawContext& context) {
    auto& light = *static_cast<LightComponent*>(context.component);
    const LightComponent before = light;
    const Entity entity = context.entity;
    resetEditActivity();
    bool changed = false;
    int type = static_cast<int>(light.type);
    if (ImGui::Combo("Type", &type,
            "Directional\0Point\0Spot\0Area (unsupported)\0")) {
        light.type = static_cast<LightType>(type);
        changed |= recordEditActivity(true);
    }
    glm::vec3 displayColor = Iridium::linearRec709ToSrgb(
        light.colorLinearRec709);
    if (Reflection::DrawColor("Color (sRGB display)", displayColor)) {
        light.colorLinearRec709 = Iridium::srgbToLinearRec709(displayColor);
        changed |= recordEditActivity(true);
    }
    if (light.type == LightType::Directional) {
        if (ImGui::DragFloat("Illuminance (lux)", &light.illuminanceLux,
                0.02f, 0.0f, 10'000'000.0f, "%.4g",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
            changed |= recordEditActivity(true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Physical illuminance at the surface. Typical sun: "
                "100,000 lx; overcast daylight: about 20,000 lx.");
        }
        Reflection::ExplainTextEntry();
        if (ImGui::Button("Moon 0.25 lx")) {
            light.illuminanceLux = 0.25f;
            changed |= recordEditActivity(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Overcast 20k lx")) {
            light.illuminanceLux = 20'000.0f;
            changed |= recordEditActivity(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Sun 100k lx")) {
            light.illuminanceLux = 100'000.0f;
            changed |= recordEditActivity(true);
        }
    }
    else {
        if (ImGui::DragFloat("Luminous intensity (cd)",
                &light.luminousIntensityCandela, 0.02f, 0.0f,
                1'000'000'000.0f, "%.4g",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
            changed |= recordEditActivity(true);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Physical luminous intensity. Apparent brightness "
                "also depends on inverse-square distance, exposure, color, and range.");
        }
        Reflection::ExplainTextEntry();
        if (ImGui::Button("Bulb 80 cd")) {
            light.luminousIntensityCandela = 80.0f;
            changed |= recordEditActivity(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Studio 10k cd")) {
            light.luminousIntensityCandela = 10'000.0f;
            changed |= recordEditActivity(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Hero 100k cd")) {
            light.luminousIntensityCandela = 100'000.0f;
            changed |= recordEditActivity(true);
        }
        const float lumens = light.type == LightType::Spot
            ? Iridium::spotLumensFromCandela(light.luminousIntensityCandela,
                light.innerConeDegrees, light.outerConeDegrees)
            : Iridium::pointLumensFromCandela(light.luminousIntensityCandela);
        ImGui::Text("Equivalent flux: %.3f lm", lumens);
    }
    changed |= recordEditActivity(
        Reflection::DrawField("Casts shadows", light.castsShadows));
    int shadowQuality = static_cast<int>(light.shadowQuality);
    if (ImGui::Combo("Shadow quality", &shadowQuality,
            "Low\0Medium\0High\0Ultra\0")) {
        light.shadowQuality = static_cast<LightShadowQuality>(shadowQuality);
        changed |= recordEditActivity(true);
    }
    changed |= recordEditActivity(
        Reflection::DrawField("Priority", light.priority));
    if (light.type != LightType::Directional) {
        changed |= recordEditActivity(
            Reflection::DrawField("Range (m)", light.rangeMeters, 0.0f, 1000.0f));
        changed |= recordEditActivity(
            Reflection::DrawField("Source radius (m)",
                light.sourceRadiusMeters, 0.0f, 50.0f));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls physically sized contact-hardening: smaller "
                "values make sharper shadows; larger values widen distant penumbrae.");
        }
    }
    if (light.type == LightType::Spot) {
        ImGui::SeparatorText("Spot angles");
        changed |= recordEditActivity(
            Reflection::DrawField("Inner cone (degrees)",
                light.innerConeDegrees, 0.0f, 90.0f));
        changed |= recordEditActivity(
            Reflection::DrawField("Outer cone (degrees)",
                light.outerConeDegrees, 0.0f, 90.0f));
    }
    if (light.type == LightType::Area) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f),
            "Area lights are readable for migration but cannot be cooked.");
    }
    if (!changed || !transactionService_) return;

    const LightComponent after = light;
    light = before;
    const auto equalLight = [](const LightComponent& lhs,
        const LightComponent& rhs) {
        return lhs.type == rhs.type &&
            glm::all(glm::equal(lhs.colorLinearRec709,
                rhs.colorLinearRec709)) &&
            lhs.illuminanceLux == rhs.illuminanceLux &&
            lhs.luminousIntensityCandela == rhs.luminousIntensityCandela &&
            lhs.rangeMeters == rhs.rangeMeters &&
            lhs.sourceRadiusMeters == rhs.sourceRadiusMeters &&
            lhs.innerConeDegrees == rhs.innerConeDegrees &&
            lhs.outerConeDegrees == rhs.outerConeDegrees &&
            lhs.castsShadows == rhs.castsShadows &&
            lhs.shadowQuality == rhs.shadowQuality &&
            lhs.priority == rhs.priority;
    };
    Iridium::EditorTransaction edit;
    edit.label = "Edit Light";
    edit.coalescingKey = "light/" + std::to_string(entity.index()) +
        ":" + std::to_string(entity.generation()) + "/" +
        std::to_string(changedItemId_);
    edit.coalescingSession = coalescingSessionForLastEdit();
    const auto appendTarget = [&](Entity target) {
        auto* pool = context.registry.findPool<LightComponent>();
        if (!pool || !pool->has(target)) return;
        const LightComponent targetBefore = pool->get(target);
        LightComponent targetAfter = targetBefore;
        if (before.type != after.type) targetAfter.type = after.type;
        if (!glm::all(glm::equal(before.colorLinearRec709,
                after.colorLinearRec709))) {
            targetAfter.colorLinearRec709 = after.colorLinearRec709;
        }
        if (before.illuminanceLux != after.illuminanceLux) {
            targetAfter.illuminanceLux = after.illuminanceLux;
        }
        if (before.luminousIntensityCandela !=
            after.luminousIntensityCandela) {
            targetAfter.luminousIntensityCandela =
                after.luminousIntensityCandela;
        }
        if (before.rangeMeters != after.rangeMeters) {
            targetAfter.rangeMeters = after.rangeMeters;
        }
        if (before.sourceRadiusMeters != after.sourceRadiusMeters) {
            targetAfter.sourceRadiusMeters = after.sourceRadiusMeters;
        }
        if (before.innerConeDegrees != after.innerConeDegrees) {
            targetAfter.innerConeDegrees = after.innerConeDegrees;
        }
        if (before.outerConeDegrees != after.outerConeDegrees) {
            targetAfter.outerConeDegrees = after.outerConeDegrees;
        }
        if (before.castsShadows != after.castsShadows) {
            targetAfter.castsShadows = after.castsShadows;
        }
        if (before.shadowQuality != after.shadowQuality) {
            targetAfter.shadowQuality = after.shadowQuality;
        }
        if (before.priority != after.priority) {
            targetAfter.priority = after.priority;
        }
        edit.operations.push_back(
            Iridium::makeEditorValueOperation<LightComponent>(
                "entity/light",
                [&registry = context.registry, target]() -> LightComponent* {
                    auto* targetPool = registry.findPool<LightComponent>();
                    return targetPool && targetPool->has(target)
                        ? &targetPool->get(target) : nullptr;
                }, targetBefore, targetAfter, equalLight));
    };
    if (selection_->selected().empty()) appendTarget(entity);
    else for (Entity target : selection_->selected()) appendTarget(target);
    (void)transactionService_->execute(std::move(edit));
}

void InspectorPanel::drawSkyComponent(
    Iridium::EditorComponentDrawContext& context) {
    auto& sky = *static_cast<Iridium::SkyComponent*>(context.component);
    const Iridium::SkyComponent before = sky;
    const Entity entity = context.entity;
    resetEditActivity();
    bool changed = false;

    changed |= recordEditActivity(
        Reflection::DrawField("Enabled", sky.enabled));
    int mode = static_cast<int>(sky.mode);
    if (ImGui::Combo("Mode", &mode, "Skybox\0HDRI\0Simulated\0")) {
        sky.mode = static_cast<Iridium::SkyMode>(mode);
        changed |= recordEditActivity(true);
    }
    changed |= recordEditActivity(
        Reflection::DrawField("Priority", sky.priority));

    if (sky.mode == Iridium::SkyMode::Hdri) {
        ImGui::SeparatorText("HDRI environment");
        const Iridium::AssetGuid displayedGuid =
            !sky.requestedEnvironmentAssetGuid.isNil()
            ? sky.requestedEnvironmentAssetGuid
            : sky.hdri.environmentAssetGuid;
        const float previewExtent = std::clamp(
            ImGui::GetContentRegionAvail().x, 96.0f, 144.0f);
        void* thumbnail = context.assetManager && !displayedGuid.isNil()
            ? context.assetManager->getEditorThumbnail(displayedGuid)
            : nullptr;
        if (thumbnail) {
            ImGui::ImageButton("##current-hdri",
                ImTextureRef(reinterpret_cast<ImTextureID>(thumbnail)),
                ImVec2(previewExtent, previewExtent));
        }
        else {
            ImGui::Button(displayedGuid.isNil() ? "Drop HDRI" : "HDRI",
                ImVec2(previewExtent, previewExtent));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Drop a cooked HDRI environment here.");
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    Iridium::kAssetBrowserDragPayloadType.data())) {
                const auto decoded = Iridium::decodeAssetDragPayload(
                    payload->DataType,
                    std::span(static_cast<const std::byte*>(payload->Data),
                        static_cast<size_t>(payload->DataSize)),
                    Iridium::AssetDragKind::Environment);
                if (decoded) {
                    sky.hdri.environmentAssetGuid = decoded->guid;
                    sky.requestedEnvironmentAssetGuid = decoded->guid;
                    sky.requestedAssetSourcePath.clear();
                    sky.assetResolutionDiagnostic.clear();
                    changed |= recordEditActivity(true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (!displayedGuid.isNil()) {
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Environment asset");
            ImGui::TextWrapped("ID: %s", displayedGuid.toString().c_str());
            if (ImGui::Button("Clear HDRI")) {
                sky.hdri.environmentAssetGuid = {};
                sky.requestedEnvironmentAssetGuid = {};
                sky.requestedAssetSourcePath.clear();
                sky.assetResolutionDiagnostic.clear();
                changed |= recordEditActivity(true);
            }
            ImGui::EndGroup();
        }
        if (!sky.assetResolutionDiagnostic.empty()) {
            ImGui::TextWrapped("%s", sky.assetResolutionDiagnostic.c_str());
        }
        ImGui::SeparatorText("HDRI controls");
        changed |= recordEditActivity(Reflection::DrawField(
            "Lighting intensity", sky.hdri.lightingIntensity, 0.0f, 64.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Background intensity", sky.hdri.backgroundIntensity,
            0.0f, 64.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Rotation (degrees)", sky.hdri.rotationDegrees,
            -360.0f, 360.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Visible to camera", sky.hdri.visibleToCamera));
        changed |= recordEditActivity(Reflection::DrawField(
            "Affects lighting", sky.hdri.affectsLighting));
    }
    else if (sky.mode == Iridium::SkyMode::Skybox) {
        ImGui::SeparatorText("Skybox");
        ImGui::TextDisabled(
            "Skybox rendering is reserved by the component schema and is not implemented yet.");
        changed |= recordEditActivity(Reflection::DrawField(
            "Intensity", sky.skybox.intensity, 0.0f, 64.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Rotation (degrees)", sky.skybox.rotationDegrees,
            -360.0f, 360.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Visible to camera", sky.skybox.visibleToCamera));
    }
    else {
        ImGui::SeparatorText("Simulated atmosphere");
        ImGui::TextDisabled(
            "Physical atmosphere rendering is reserved and not implemented yet.");
        changed |= recordEditActivity(Reflection::DrawField(
            "Turbidity", sky.simulated.turbidity, 1.0f, 20.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Ozone", sky.simulated.ozone, 0.0f, 1.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Ground albedo", sky.simulated.groundAlbedo, 0.0f, 1.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Atmosphere height (km)",
            sky.simulated.atmosphereHeightKilometers, 1.0f, 1000.0f));
        changed |= recordEditActivity(Reflection::DrawField(
            "Sun disk", sky.simulated.sunDisk));
        changed |= recordEditActivity(Reflection::DrawField(
            "Aerial perspective", sky.simulated.aerialPerspective));
    }

    if (!changed || !transactionService_) return;
    const Iridium::SkyComponent after = sky;
    sky = before;
    const auto equalSky = [](const Iridium::SkyComponent& lhs,
        const Iridium::SkyComponent& rhs) {
        return lhs.enabled == rhs.enabled && lhs.mode == rhs.mode &&
            lhs.skybox.cubemapAssetGuid == rhs.skybox.cubemapAssetGuid &&
            lhs.skybox.intensity == rhs.skybox.intensity &&
            lhs.skybox.rotationDegrees == rhs.skybox.rotationDegrees &&
            lhs.skybox.visibleToCamera == rhs.skybox.visibleToCamera &&
            lhs.hdri.environmentAssetGuid == rhs.hdri.environmentAssetGuid &&
            lhs.hdri.lightingIntensity == rhs.hdri.lightingIntensity &&
            lhs.hdri.backgroundIntensity == rhs.hdri.backgroundIntensity &&
            lhs.hdri.rotationDegrees == rhs.hdri.rotationDegrees &&
            lhs.hdri.visibleToCamera == rhs.hdri.visibleToCamera &&
            lhs.hdri.affectsLighting == rhs.hdri.affectsLighting &&
            lhs.simulated.turbidity == rhs.simulated.turbidity &&
            lhs.simulated.ozone == rhs.simulated.ozone &&
            lhs.simulated.groundAlbedo == rhs.simulated.groundAlbedo &&
            lhs.simulated.atmosphereHeightKilometers ==
                rhs.simulated.atmosphereHeightKilometers &&
            lhs.simulated.sunDisk == rhs.simulated.sunDisk &&
            lhs.simulated.aerialPerspective ==
                rhs.simulated.aerialPerspective &&
            lhs.priority == rhs.priority;
    };
    Iridium::EditorTransaction edit;
    edit.label = "Edit Sky";
    edit.coalescingKey = "sky/" + std::to_string(entity.index()) + ":" +
        std::to_string(entity.generation()) + "/" +
        std::to_string(changedItemId_);
    edit.coalescingSession = coalescingSessionForLastEdit();
    const auto appendTarget = [&](Entity target) {
        auto* pool = context.registry.findPool<Iridium::SkyComponent>();
        if (!pool || !pool->has(target)) return;
        const Iridium::SkyComponent targetBefore = pool->get(target);
        Iridium::SkyComponent targetAfter = after;
        targetAfter.resolvedEnvironmentAssetGuid =
            targetBefore.resolvedEnvironmentAssetGuid;
        targetAfter.requestedAssetSourcePath =
            targetBefore.requestedAssetSourcePath;
        targetAfter.assetResolutionDiagnostic =
            targetBefore.assetResolutionDiagnostic;
        edit.operations.push_back(
            Iridium::makeEditorValueOperation<Iridium::SkyComponent>(
                "entity/sky",
                [&registry = context.registry, target]()
                    -> Iridium::SkyComponent* {
                    auto* targetPool =
                        registry.findPool<Iridium::SkyComponent>();
                    return targetPool && targetPool->has(target)
                        ? &targetPool->get(target) : nullptr;
                }, targetBefore, targetAfter, equalSky));
    };
    if (selection_->selected().empty()) appendTarget(entity);
    else for (Entity target : selection_->selected()) appendTarget(target);
    (void)transactionService_->execute(std::move(edit));
}

void InspectorPanel::drawRelationshipComponent(
    const RelationshipComponent& relationship) {
    ImGui::Text("Depth: %d", relationship.depth);
    ImGui::Text("Children: %d",
        static_cast<int>(relationship.children.size()));
    ImGui::Text("Order: %d", relationship.siblingOrder);
    if (relationship.parent != NULL_ENTITY) {
        ImGui::Text("Parent: %u:%u", relationship.parent.index(),
            relationship.parent.generation());
    }
    else {
        ImGui::TextUnformatted("Parent: None");
    }
}

void InspectorPanel::drawReflectionProbeComponent(
    Iridium::EditorComponentDrawContext& context) {
    auto& probe = *static_cast<Iridium::ReflectionProbeComponent*>(
        context.component);
    const Iridium::ReflectionProbeComponent before = probe;
    const Entity entity = context.entity;
    resetEditActivity();
    bool changed = false;
    bool captureRequested = false;

    if (ImGui::CollapsingHeader("How to use this probe")) {
        ImGui::TextWrapped(
            "1. Move the entity to the reflection center, then choose a box "
            "for rooms or a sphere for open/local regions. The cyan viewport "
            "gizmo is the influence boundary; its paler inner boundary marks "
            "where blending begins.");
        ImGui::TextWrapped(
            "2. For authored HDR lighting, drop an Environment below. For a "
            "scene capture, leave that slot empty, choose On demand, and use "
            "Capture Now after the scene is ready.");
        ImGui::TextWrapped(
            "3. Use Box projection for interior boxes. Overlapping probes blend "
            "by influence; Priority only resolves otherwise comparable choices.");
        ImGui::TextWrapped(
            "4. Baked captures are reusable assets. Re-bake after nearby static "
            "geometry or lighting changes. Realtime is the most expensive mode.");
        ImGui::Spacing();
    }

    changed |= recordEditActivity(Reflection::DrawField(
        "Enabled", probe.enabled));
    int shape = static_cast<int>(probe.shape);
    if (ImGui::Combo("Shape", &shape, "Sphere\0Box\0")) {
        probe.shape = static_cast<Iridium::ReflectionProbeShape>(shape);
        changed |= recordEditActivity(true);
    }
    explainLastItem("Sphere is useful for open/local regions. Box matches rooms "
        "and supports box-projected parallax correction.");
    if (probe.shape == Iridium::ReflectionProbeShape::Sphere) {
        changed |= recordEditActivity(Reflection::DrawField(
            "Radius (m)", probe.sphereRadiusMeters, 0.01f, 10000.0f));
        explainLastItem("Outer influence radius. Select the entity to see it in cyan.");
    }
    else {
        changed |= recordEditActivity(Reflection::DrawField(
            "Box extents (m)", probe.boxExtentsMeters));
        explainLastItem("Half-size of the oriented influence box in local X/Y/Z.");
    }
    changed |= recordEditActivity(Reflection::DrawField(
        "Blend distance (m)", probe.blendDistanceMeters, 0.0f, 10000.0f));
    explainLastItem("Distance inward from the outer bound over which this probe "
        "fades. The inner blend boundary is shown by the pale cyan gizmo.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Intensity", probe.intensity, 0.0f, 64.0f));
    explainLastItem("Scene-linear multiplier for this probe's specular contribution.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Priority", probe.priority));
    explainLastItem("Tie-breaker for overlapping probes with comparable influence. "
        "Prefer spatial bounds and blending before raising priority.");

    ImGui::SeparatorText("Published environment");
    const Iridium::AssetGuid displayedGuid =
        !probe.requestedEnvironmentAssetGuid.isNil()
        ? probe.requestedEnvironmentAssetGuid
        : probe.environmentAssetGuid;
    const float previewExtent = std::clamp(
        ImGui::GetContentRegionAvail().x, 96.0f, 144.0f);
    void* thumbnail = context.assetManager && !displayedGuid.isNil()
        ? context.assetManager->getEditorThumbnail(displayedGuid)
        : nullptr;
    if (thumbnail) {
        ImGui::ImageButton("##reflection-probe-environment",
            ImTextureRef(reinterpret_cast<ImTextureID>(thumbnail)),
            ImVec2(previewExtent, previewExtent));
    }
    else {
        ImGui::Button(displayedGuid.isNil() ? "Drop Environment" : "Environment",
            ImVec2(previewExtent, previewExtent));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Drop a cooked HDRI environment for this probe.");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                Iridium::kAssetBrowserDragPayloadType.data())) {
            const auto decoded = Iridium::decodeAssetDragPayload(
                payload->DataType,
                std::span(static_cast<const std::byte*>(payload->Data),
                    static_cast<size_t>(payload->DataSize)),
                Iridium::AssetDragKind::Environment);
            if (decoded) {
                probe.environmentAssetGuid = decoded->guid;
                probe.requestedEnvironmentAssetGuid = decoded->guid;
                probe.publicationDiagnostic.clear();
                changed |= recordEditActivity(true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!displayedGuid.isNil()) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Environment asset");
        ImGui::TextWrapped("ID: %s", displayedGuid.toString().c_str());
        if (ImGui::Button("Clear Environment")) {
            probe.environmentAssetGuid = {};
            probe.requestedEnvironmentAssetGuid = {};
            probe.publicationDiagnostic.clear();
            changed |= recordEditActivity(true);
        }
        ImGui::EndGroup();
    }
    if (!probe.publicationDiagnostic.empty()) {
        ImGui::TextWrapped("%s", probe.publicationDiagnostic.c_str());
    }

    ImGui::SeparatorText("Capture");
    int updateMode = static_cast<int>(probe.updateMode);
    if (ImGui::Combo("Update mode", &updateMode,
            "Baked\0On demand\0Realtime\0")) {
        probe.updateMode = static_cast<Iridium::ReflectionProbeUpdateMode>(
            updateMode);
        changed |= recordEditActivity(true);
    }
    explainLastItem("Baked stores a reusable capture; On demand updates only when "
        "requested; Realtime may recapture continuously and has the highest cost.");
    int parallax = static_cast<int>(probe.parallaxMode);
    if (ImGui::Combo("Parallax correction", &parallax,
            "None\0Box projection\0")) {
        probe.parallaxMode = static_cast<Iridium::ReflectionProbeParallaxMode>(
            parallax);
        changed |= recordEditActivity(true);
    }
    explainLastItem("Box projection corrects reflection lookup for box-shaped "
        "interiors. It is intentionally unavailable as a spherical approximation.");
    constexpr std::array resolutions{ 128, 256, 512, 1024, 2048, 4096 };
    int resolutionIndex = 2;
    for (size_t index = 0; index < resolutions.size(); ++index) {
        if (probe.captureResolution == resolutions[index]) {
            resolutionIndex = static_cast<int>(index);
            break;
        }
    }
    if (ImGui::Combo("Resolution", &resolutionIndex,
            "128\0" "256\0" "512\0" "1024\0" "2048\0" "4096\0")) {
        probe.captureResolution = resolutions[static_cast<size_t>(
            resolutionIndex)];
        changed |= recordEditActivity(true);
    }
    explainLastItem("Cubemap face resolution. 512 is a practical default; use "
        "1024-2048 for hero interiors and validate GPU memory/capture cost.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Near plane (m)", probe.captureNearMeters, 0.001f, 100.0f));
    explainLastItem("Closest captured surface. Keep this as large as the local "
        "geometry permits to preserve depth precision.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Far plane (m)", probe.captureFarMeters, 0.01f, 100000.0f));
    explainLastItem("Farthest captured surface. It should cover the visible region, "
        "not the entire world.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Capture sky", probe.captureSky));
    if (!displayedGuid.isNil()) ImGui::BeginDisabled();
    const bool baked = probe.updateMode ==
        Iridium::ReflectionProbeUpdateMode::Baked;
    captureRequested = ImGui::Button(baked ? "Bake Capture" : "Capture Now");
    if (!displayedGuid.isNil()) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(
                "Clear the assigned environment to use scene capture.");
    }
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(baked
            ? "Capture all six faces, filter them, and write a reusable cooked "
                "environment. Save the scene before baking."
            : "Capture all six faces and publish the complete runtime cube.");
    }

    if (changed) {
        probe.sphereRadiusMeters = (std::max)(
            probe.sphereRadiusMeters, 0.01f);
        probe.boxExtentsMeters = glm::max(
            probe.boxExtentsMeters, glm::vec3(0.01f));
        probe.blendDistanceMeters = (std::max)(
            probe.blendDistanceMeters, 0.0f);
        probe.intensity = (std::max)(probe.intensity, 0.0f);
        probe.captureNearMeters = (std::max)(
            probe.captureNearMeters, 0.001f);
        probe.captureFarMeters = (std::max)(probe.captureFarMeters,
            probe.captureNearMeters + 0.001f);
    }

    const auto requestCapture = [&]() {
        const auto requestTarget = [&](Entity target) {
            auto* pool = context.registry.findPool<
                Iridium::ReflectionProbeComponent>();
            if (!pool || !pool->has(target)) return;
            auto& targetProbe = pool->get(target);
            ++targetProbe.explicitCaptureRevision;
            if (targetProbe.explicitCaptureRevision == 0u)
                ++targetProbe.explicitCaptureRevision;
            targetProbe.publicationDiagnostic = baked
                ? "Baked capture queued."
                : "Runtime capture queued.";
        };
        if (selection_->selected().empty()) requestTarget(entity);
        else for (Entity target : selection_->selected()) requestTarget(target);
    };
    if (!changed || !transactionService_) {
        if (captureRequested) requestCapture();
        return;
    }
    const Iridium::ReflectionProbeComponent after = probe;
    probe = before;
    const auto equalProbe = [](const Iridium::ReflectionProbeComponent& lhs,
        const Iridium::ReflectionProbeComponent& rhs) {
        return lhs.enabled == rhs.enabled && lhs.shape == rhs.shape &&
            lhs.sphereRadiusMeters == rhs.sphereRadiusMeters &&
            glm::all(glm::equal(lhs.boxExtentsMeters, rhs.boxExtentsMeters)) &&
            lhs.blendDistanceMeters == rhs.blendDistanceMeters &&
            lhs.intensity == rhs.intensity && lhs.priority == rhs.priority &&
            lhs.updateMode == rhs.updateMode &&
            lhs.parallaxMode == rhs.parallaxMode &&
            lhs.captureResolution == rhs.captureResolution &&
            lhs.captureNearMeters == rhs.captureNearMeters &&
            lhs.captureFarMeters == rhs.captureFarMeters &&
            lhs.captureSky == rhs.captureSky &&
            lhs.environmentAssetGuid == rhs.environmentAssetGuid &&
            lhs.requestedEnvironmentAssetGuid ==
                rhs.requestedEnvironmentAssetGuid;
    };
    Iridium::EditorTransaction edit;
    edit.label = "Edit Reflection Probe";
    edit.coalescingKey = "reflection_probe/" +
        std::to_string(entity.index()) + ":" +
        std::to_string(entity.generation()) + "/" +
        std::to_string(changedItemId_);
    edit.coalescingSession = coalescingSessionForLastEdit();
    const auto appendTarget = [&](Entity target) {
        auto* pool = context.registry.findPool<
            Iridium::ReflectionProbeComponent>();
        if (!pool || !pool->has(target)) return;
        const Iridium::ReflectionProbeComponent targetBefore = pool->get(target);
        Iridium::ReflectionProbeComponent targetAfter = after;
        targetAfter.resolvedEnvironmentAssetGuid =
            targetBefore.resolvedEnvironmentAssetGuid;
        targetAfter.publicationDiagnostic =
            targetBefore.publicationDiagnostic;
        edit.operations.push_back(Iridium::makeEditorValueOperation<
            Iridium::ReflectionProbeComponent>(
                "entity/reflection_probe",
                [&registry = context.registry, target]()
                    -> Iridium::ReflectionProbeComponent* {
                    auto* targetPool = registry.findPool<
                        Iridium::ReflectionProbeComponent>();
                    return targetPool && targetPool->has(target)
                        ? &targetPool->get(target) : nullptr;
                }, targetBefore, targetAfter, equalProbe));
    };
    if (selection_->selected().empty()) appendTarget(entity);
    else for (Entity target : selection_->selected()) appendTarget(target);
    (void)transactionService_->execute(std::move(edit));
    if (captureRequested) requestCapture();
}

void InspectorPanel::drawBakedLightingSetComponent(
    Iridium::EditorComponentDrawContext& context) {
    auto& lighting = *static_cast<Iridium::BakedLightingSetComponent*>(
        context.component);
    const Iridium::BakedLightingSetComponent before = lighting;
    const Entity entity = context.entity;
    resetEditActivity();
    bool changed = false;

    if (ImGui::CollapsingHeader("How to use baked lighting")) {
        ImGui::TextWrapped(
            "A Baked Lighting Set is the scene-level owner for a validated "
            "iridium.baked-lighting product: lightmaps, probe-volume irradiance, "
            "and baked visibility can be enabled independently below.");
        ImGui::TextWrapped(
            "Drop the cooked product into the slot, keep one authoritative set "
            "enabled for a region, then use the contribution controls for artistic "
            "balancing. A missing or invalid product contributes neutral lighting.");
        ImGui::TextWrapped(
            "The M5 component and product contract are complete, but the full GI "
            "bake solver and final runtime GI consumption arrive in later roadmap "
            "milestones; this component alone does not bake a scene today.");
        ImGui::Spacing();
    }
    changed |= recordEditActivity(Reflection::DrawField(
        "Enabled", lighting.enabled));

    ImGui::SeparatorText("Cooked product");
    const Iridium::AssetGuid displayed =
        !lighting.requestedLightingAssetGuid.isNil()
            ? lighting.requestedLightingAssetGuid
            : lighting.lightingAssetGuid;
    const float previewExtent = std::clamp(
        ImGui::GetContentRegionAvail().x, 96.0f, 144.0f);
    void* thumbnail = context.assetManager && !displayed.isNil()
        ? context.assetManager->getEditorThumbnail(displayed) : nullptr;
    if (thumbnail) {
        ImGui::ImageButton("##baked-lighting-product",
            ImTextureRef(reinterpret_cast<ImTextureID>(thumbnail)),
            ImVec2(previewExtent, previewExtent));
    }
    else {
        ImGui::Button(displayed.isNil() ? "Drop Baked Lighting" :
            "Baked Lighting", ImVec2(previewExtent, previewExtent));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Drop a validated iridium.baked-lighting product. Bad or missing "
            "products preserve this authored asset identity and contribute neutral lighting.");
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                Iridium::kAssetBrowserDragPayloadType.data())) {
            const auto decoded = Iridium::decodeAssetDragPayload(
                payload->DataType,
                std::span(static_cast<const std::byte*>(payload->Data),
                    static_cast<size_t>(payload->DataSize)),
                Iridium::AssetDragKind::BakedLighting);
            if (decoded) {
                lighting.lightingAssetGuid = decoded->guid;
                lighting.requestedLightingAssetGuid = decoded->guid;
                lighting.publicationDiagnostic.clear();
                changed |= recordEditActivity(true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!displayed.isNil()) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Baked lighting asset");
        ImGui::TextWrapped("ID: %s", displayed.toString().c_str());
        if (ImGui::Button("Clear Baked Lighting")) {
            lighting.lightingAssetGuid = {};
            lighting.requestedLightingAssetGuid = {};
            lighting.publicationDiagnostic.clear();
            changed |= recordEditActivity(true);
        }
        ImGui::EndGroup();
    }
    if (!lighting.publicationDiagnostic.empty())
        ImGui::TextWrapped("%s", lighting.publicationDiagnostic.c_str());

    ImGui::SeparatorText("Contribution");
    changed |= recordEditActivity(Reflection::DrawField(
        "Diffuse intensity", lighting.diffuseIntensity, 0.0f, 64.0f));
    explainLastItem("Multiplier for diffuse lightmap and probe-volume irradiance.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Specular intensity", lighting.specularIntensity, 0.0f, 64.0f));
    explainLastItem("Multiplier for any baked specular contribution in the product.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Apply lightmaps", lighting.applyLightmaps));
    explainLastItem("Use surface lightmap payloads when the assigned product contains them.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Apply probe volumes", lighting.applyProbeVolumes));
    explainLastItem("Use volumetric irradiance samples for dynamic and unwrapped objects.");
    changed |= recordEditActivity(Reflection::DrawField(
        "Apply visibility", lighting.applyVisibility));
    explainLastItem("Use baked occlusion/visibility payloads when present.");
    if (changed) {
        lighting.diffuseIntensity = (std::max)(lighting.diffuseIntensity, 0.0f);
        lighting.specularIntensity = (std::max)(lighting.specularIntensity, 0.0f);
    }
    if (!changed || !transactionService_) return;

    const Iridium::BakedLightingSetComponent after = lighting;
    lighting = before;
    const auto equal = [](const Iridium::BakedLightingSetComponent& lhs,
        const Iridium::BakedLightingSetComponent& rhs) {
        return lhs.enabled == rhs.enabled &&
            lhs.lightingAssetGuid == rhs.lightingAssetGuid &&
            lhs.requestedLightingAssetGuid == rhs.requestedLightingAssetGuid &&
            lhs.diffuseIntensity == rhs.diffuseIntensity &&
            lhs.specularIntensity == rhs.specularIntensity &&
            lhs.applyLightmaps == rhs.applyLightmaps &&
            lhs.applyProbeVolumes == rhs.applyProbeVolumes &&
            lhs.applyVisibility == rhs.applyVisibility;
    };
    Iridium::EditorTransaction edit;
    edit.label = "Edit Baked Lighting Set";
    edit.coalescingKey = "baked_lighting_set/" +
        std::to_string(entity.index()) + ":" +
        std::to_string(entity.generation()) + "/" +
        std::to_string(changedItemId_);
    edit.coalescingSession = coalescingSessionForLastEdit();
    const auto appendTarget = [&](Entity target) {
        auto* pool = context.registry.findPool<
            Iridium::BakedLightingSetComponent>();
        if (!pool || !pool->has(target)) return;
        const Iridium::BakedLightingSetComponent targetBefore = pool->get(target);
        Iridium::BakedLightingSetComponent targetAfter = after;
        targetAfter.resolvedLightingAssetGuid =
            targetBefore.resolvedLightingAssetGuid;
        targetAfter.publicationDiagnostic =
            targetBefore.publicationDiagnostic;
        edit.operations.push_back(Iridium::makeEditorValueOperation<
            Iridium::BakedLightingSetComponent>(
                "entity/baked_lighting_set",
                [&registry = context.registry, target]()
                    -> Iridium::BakedLightingSetComponent* {
                    auto* targetPool = registry.findPool<
                        Iridium::BakedLightingSetComponent>();
                    return targetPool && targetPool->has(target)
                        ? &targetPool->get(target) : nullptr;
                }, targetBefore, targetAfter, equal));
    };
    if (selection_->selected().empty()) appendTarget(entity);
    else for (Entity target : selection_->selected()) appendTarget(target);
    (void)transactionService_->execute(std::move(edit));
}

void InspectorPanel::drawMeshComponent(
    Iridium::EditorComponentDrawContext& context) {
    void* component = context.component;
    Registry& registry = context.registry;
    const Entity selectedEntityValue = context.entity;
    const Entity* selectedEntity = &selectedEntityValue;
    Iridium::AssetManager* assetManager = context.assetManager;
    // Asset-aware Mesh editing is registered as a custom drawer.
    auto& mesh =
        *static_cast<MeshComponent*>(component);
    const Iridium::EditorMeshAuthoringState enabledBefore =
        Iridium::captureEditorMeshAuthoringState(mesh);
    if (Reflection::DrawField("Enabled", mesh.enabled)) {
        commitMeshAuthoringEdit(transactionService_, registry,
            selection_->selected(), selectedEntityValue, mesh, enabledBefore,
            Iridium::captureEditorMeshAuthoringState(mesh), "Set Mesh Enabled");
    }
    ImGui::SeparatorText("Model");
    const Iridium::AssetGuid
        displayedGuid =
            !mesh.requestedAssetGuid
                .isNil()
            ? mesh.requestedAssetGuid
            : !mesh.assetGuid.isNil()
                ? mesh.assetGuid
            : mesh.model
                ? mesh.model->assetGuid
                : Iridium::AssetGuid{};
    if (!displayedGuid.isNil()) {
        const float previewExtent =
            std::clamp(
                ImGui::GetContentRegionAvail().x,
                96.0f, 144.0f);
        void* thumbnail =
            assetManager
            ? assetManager
                ->getEditorThumbnail(
                    displayedGuid)
            : nullptr;
        if (thumbnail) {
            ImGui::ImageButton(
                "##current-mesh",
                ImTextureRef(
                    reinterpret_cast<
                        ImTextureID>(
                        thumbnail)),
                ImVec2(previewExtent,
                    previewExtent));
        }
        else {
            ImGui::Button(
                "Model",
                ImVec2(previewExtent,
                    previewExtent));
        }
        if (ImGui::IsItemHovered(
                ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "Drop a model here to replace the current mesh.");
        }
        if (ImGui::BeginDragDropTarget()) {
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
                        Iridium::AssetDragKind::
                            Model);
                if (decoded) {
                    const Iridium::EditorMeshAuthoringState before =
                        Iridium::captureEditorMeshAuthoringState(mesh);
                    mesh.requestedAssetGuid =
                        decoded->guid;
                    mesh.materialOverrides.clear();
                    mesh.requestedMaterialAssetRoots
                        .clear();
                    commitMeshAuthoringEdit(transactionService_, registry,
                        selection_->selected(), selectedEntityValue, mesh, before,
                        Iridium::captureEditorMeshAuthoringState(mesh),
                        "Assign Model");
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(
            "Current mesh");
        ImGui::TextWrapped(
            "ID: %s",
            displayedGuid
                .toString().c_str());
        ImGui::TextDisabled(
            "Drop onto the preview to swap");
        ImGui::EndGroup();
    }
    else {
        ImGui::TextDisabled("None");
        ImGui::Button(
            "No mesh",
            ImVec2(
                std::min(144.0f,
                    ImGui::GetContentRegionAvail().x),
                96.0f));
        if (ImGui::BeginDragDropTarget()) {
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
                        Iridium::AssetDragKind::
                            Model);
                if (decoded) {
                    const Iridium::EditorMeshAuthoringState before =
                        Iridium::captureEditorMeshAuthoringState(mesh);
                    mesh.requestedAssetGuid =
                        decoded->guid;
                    mesh.materialOverrides.clear();
                    mesh.requestedMaterialAssetRoots.clear();
                    commitMeshAuthoringEdit(transactionService_, registry,
                        selection_->selected(), selectedEntityValue, mesh, before,
                        Iridium::captureEditorMeshAuthoringState(mesh),
                        "Assign Model");
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    if (ImGui::Button(
            "Browse models...")) {
        openAssetPicker(
            AssetPickerKind::Model);
    }
    if (!mesh.assetResolutionDiagnostic
            .empty()) {
        ImGui::TextWrapped(
            "%s",
            mesh.assetResolutionDiagnostic
                .c_str());
    }

    ImGui::SeparatorText(
        "Materials");
    if (!mesh.model) {
        ImGui::TextDisabled(
            "Material slots appear after the model is ready.");
    }
    else {
        const Iridium::AssetGuid
            modelGuid =
                !mesh.model
                    ->assetGuid.isNil()
                ? mesh.model->assetGuid
                : displayedGuid;
        if (materialCatalogRoot_ !=
                modelGuid) {
            materialCatalogRoot_ =
                modelGuid;
            materialNames_.clear();
            if (assetCatalog_ &&
                !modelGuid.isNil()) {
                const auto records =
                    assetCatalog_
                        ->recordsForSourceRoot(
                            modelGuid);
                if (thumbnailService_) {
                    thumbnailService_
                        ->setPinnedDemand(
                            records);
                }
                for (const auto&
                        record : records) {
                    if (record.assetType ==
                        "iridium.material") {
                        materialNames_
                            .insert_or_assign(
                                record.guid,
                                record.displayName);
                    }
                }
            }
        }
        std::map<int,
            Iridium::AssetGuid> slots;
        for (const Iridium::SubMesh&
                primitive :
            mesh.model->subMeshes) {
            if (primitive.materialIndex >=
                0) {
                slots.emplace(
                    primitive.materialIndex,
                    primitive.materialGuid);
            }
        }
        if (materialPageEntity_ !=
                *selectedEntity ||
            materialPageModel_ !=
                modelGuid) {
            materialPageEntity_ =
                *selectedEntity;
            materialPageModel_ =
                modelGuid;
            materialPage_ = 0;
        }
        constexpr size_t
            materialsPerPage = 4;
        const size_t pageCount =
            std::max<size_t>(
                1,
                (slots.size() +
                    materialsPerPage -
                    1) /
                    materialsPerPage);
        materialPage_ =
            std::min(
                materialPage_,
                pageCount - 1);
        const size_t firstSlot =
            materialPage_ *
            materialsPerPage;
        const size_t lastSlot =
            std::min(
                slots.size(),
                firstSlot +
                    materialsPerPage);
        constexpr std::array<float, 4>
            materialThumbnailSizes{
                48.0f, 64.0f,
                80.0f, 104.0f,
            };
        Iridium::EditorComponentUI::
            drawCollectionControls(
                "mesh-materials",
                materialView_,
                materialThumbnailSizes);
        const bool materialGrid =
            materialView_.layout ==
            Iridium::EditorComponentUI::
                CollectionLayout::Grid;
        const float thumbnailExtent =
            Iridium::EditorComponentUI::
                thumbnailExtent(
                    materialView_,
                    materialThumbnailSizes);
        const float available =
            ImGui::GetContentRegionAvail().x;
        const int columns =
            materialGrid &&
                available >= 390.0f
            ? 2 : 1;
        const float cardHeight =
            materialGrid
            ? std::max(
                210.0f,
                thumbnailExtent +
                    138.0f)
            : std::max(
                82.0f,
                thumbnailExtent +
                    16.0f);
        if (ImGui::BeginTable(
                    "material-cards",
                    columns,
                    ImGuiTableFlags_SizingStretchSame |
                        ImGuiTableFlags_PadOuterX)) {
                size_t slotPosition = 0;
                size_t visibleIndex = 0;
                for (const auto&
                        [slotIndex,
                            sourceGuid] :
                    slots) {
                    if (slotPosition <
                            firstSlot ||
                        slotPosition >=
                            lastSlot) {
                        ++slotPosition;
                        continue;
                    }
                    if (visibleIndex %
                            columns == 0) {
                        ImGui::TableNextRow();
                    }
                    ImGui::TableSetColumnIndex(
                        static_cast<int>(
                            visibleIndex %
                            columns));
                    ImGui::PushID(
                        slotIndex);
                    ImGui::BeginChild(
                        "material-card",
                        ImVec2(0.0f,
                            cardHeight),
                        ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse);
                    const auto override =
                        std::ranges::find_if(
                            mesh.materialOverrides,
                            [sourceGuid](
                                const MeshComponent::
                                    MaterialOverride&
                                    candidate) {
                                return candidate
                                    .sourceMaterialGuid ==
                                    sourceGuid;
                            });
                    const bool overridden =
                        override !=
                        mesh.materialOverrides
                            .end();
                    const Iridium::AssetGuid
                        shownGuid =
                            overridden
                            ? override
                                ->materialGuid
                            : sourceGuid;
                    void* thumbnail =
                        assetManager
                        ? assetManager
                            ->getEditorThumbnail(
                                shownGuid)
                        : nullptr;
                    if (!materialGrid) {
                        if (ImGui::BeginTable(
                                "material-list-row",
                                4,
                                ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_NoPadOuterX)) {
                            ImGui::TableSetupColumn(
                                "Preview",
                                ImGuiTableColumnFlags_WidthFixed,
                                thumbnailExtent + 8.0f);
                            ImGui::TableSetupColumn(
                                "Details",
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                "Browse",
                                ImGuiTableColumnFlags_WidthFixed,
                                74.0f);
                            ImGui::TableSetupColumn(
                                "Reset",
                                ImGuiTableColumnFlags_WidthFixed,
                                52.0f);
                            ImGui::TableNextRow(
                                ImGuiTableRowFlags_None,
                                thumbnailExtent);
                            ImGui::TableSetColumnIndex(0);
                            if (thumbnail) {
                                ImGui::Image(
                                    ImTextureRef(
                                        reinterpret_cast<
                                            ImTextureID>(
                                            thumbnail)),
                                    ImVec2(
                                        thumbnailExtent,
                                        thumbnailExtent));
                            }
                            else {
                                ImGui::PushStyleColor(
                                    ImGuiCol_Button,
                                    ImVec4(
                                        0.45f, 0.28f,
                                        0.16f, 1.0f));
                                ImGui::Button(
                                    "Material",
                                    ImVec2(
                                        thumbnailExtent,
                                        thumbnailExtent));
                                ImGui::PopStyleColor();
                            }
                            if (ImGui::BeginDragDropTarget()) {
                                if (const ImGuiPayload* payload =
                                        ImGui::AcceptDragDropPayload(
                                            Iridium::
                                                kAssetBrowserDragPayloadType
                                                    .data())) {
                                    const auto decoded =
                                        Iridium::
                                            decodeAssetDragPayload(
                                                payload->DataType,
                                                std::span(
                                                    static_cast<
                                                        const std::byte*>(
                                                        payload->Data),
                                                    static_cast<size_t>(
                                                        payload->DataSize)),
                                                Iridium::
                                                    AssetDragKind::
                                                        Material);
                                    if (decoded) {
                                        const Iridium::EditorMeshAuthoringState before =
                                            Iridium::captureEditorMeshAuthoringState(mesh);
                                        assignMaterial(
                                            mesh,
                                            sourceGuid,
                                            decoded->guid);
                                        commitMeshAuthoringEdit(
                                            transactionService_, registry,
                                            selection_->selected(),
                                            selectedEntityValue, mesh, before,
                                            Iridium::captureEditorMeshAuthoringState(mesh),
                                            "Assign Material");
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            if (ImGui::IsItemHovered(
                                    ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip(
                                    "Drop a material onto this preview to replace the slot.");
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text(
                                "Slot %d",
                                slotIndex);
                            if (overridden) {
                                ImGui::SameLine();
                                const bool ready =
                                    assetManager &&
                                    assetManager
                                        ->findCookedMaterial(
                                            shownGuid)
                                        .has_value();
                                ImGui::TextColored(
                                    ready
                                        ? ImVec4(
                                            0.35f, 0.85f,
                                            0.45f, 1.0f)
                                        : ImVec4(
                                            0.95f, 0.65f,
                                            0.25f, 1.0f),
                                    "%s",
                                    ready
                                        ? "Override"
                                        : "Loading");
                            }
                            const std::string
                                shownName =
                                    materialName(
                                        shownGuid);
                            ImGui::TextUnformatted(
                                shownName.c_str());
                            if (ImGui::IsItemHovered(
                                    ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip(
                                    "%s",
                                    shownName.c_str());
                            }
                            const std::string
                                shownId =
                                    shownGuid
                                        .toString();
                            ImGui::TextDisabled(
                                "ID: %.8s...",
                                shownId.c_str());
                            if (ImGui::IsItemHovered(
                                    ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip(
                                    "%s",
                                    shownId.c_str());
                            }
                            ImGui::TableSetColumnIndex(2);
                            if (ImGui::Button(
                                    "Browse...")) {
                                openAssetPicker(
                                    AssetPickerKind::
                                        Material,
                                    sourceGuid);
                            }
                            ImGui::TableSetColumnIndex(3);
                            if (overridden) {
                                if (ImGui::Button(
                                        "Reset")) {
                                    const Iridium::EditorMeshAuthoringState before =
                                        Iridium::captureEditorMeshAuthoringState(mesh);
                                    mesh.materialOverrides
                                        .erase(
                                            override);
                                    mesh.requestedMaterialAssetRoots
                                        .clear();
                                    commitMeshAuthoringEdit(
                                        transactionService_, registry,
                                        selection_->selected(),
                                        selectedEntityValue, mesh, before,
                                        Iridium::captureEditorMeshAuthoringState(mesh),
                                        "Reset Material Override");
                                }
                            }
                            else {
                                ImGui::TextDisabled(
                                    "Source");
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndChild();
                        ImGui::PopID();
                        ++visibleIndex;
                        ++slotPosition;
                        continue;
                    }
                    if (thumbnail) {
                        ImGui::Image(
                            ImTextureRef(
                                reinterpret_cast<
                                    ImTextureID>(
                                    thumbnail)),
                            ImVec2(
                                thumbnailExtent,
                                thumbnailExtent));
                    }
                    else {
                        ImGui::PushStyleColor(
                            ImGuiCol_Button,
                            ImVec4(
                                0.45f,
                                0.28f,
                                0.16f,
                                1.0f));
                        ImGui::Button(
                            "Material",
                            ImVec2(
                                thumbnailExtent,
                                thumbnailExtent));
                        ImGui::PopStyleColor();
                    }
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text(
                        "Slot %d",
                        slotIndex);
                    ImGui::TextWrapped(
                        "%s",
                        materialName(
                            shownGuid)
                            .c_str());
                    if (overridden) {
                        const bool ready =
                            assetManager &&
                            assetManager
                                ->findCookedMaterial(
                                    shownGuid)
                                .has_value();
                        ImGui::TextColored(
                            ready
                                ? ImVec4(
                                    0.35f,
                                    0.85f,
                                    0.45f,
                                    1.0f)
                                : ImVec4(
                                    0.95f,
                                    0.65f,
                                    0.25f,
                                    1.0f),
                            "%s",
                            ready
                                ? "Override"
                                : "Loading");
                    }
                    ImGui::EndGroup();
                    ImGui::TextWrapped(
                        "ID: %s",
                        shownGuid
                            .toString()
                            .c_str());
                    if (materialGrid) {
                        ImGui::Button(
                            "Drop material here",
                            ImVec2(-1.0f,
                                0.0f));
                    }
                    else {
                        ImGui::SameLine();
                        ImGui::Button(
                            "Drop material here");
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload*
                                payload =
                            ImGui::
                                AcceptDragDropPayload(
                                    Iridium::
                                        kAssetBrowserDragPayloadType
                                            .data())) {
                            const auto decoded =
                                Iridium::
                                    decodeAssetDragPayload(
                                        payload->DataType,
                                        std::span(
                                            static_cast<
                                                const std::byte*>(
                                                payload->Data),
                                            static_cast<size_t>(
                                                payload->DataSize)),
                                        Iridium::AssetDragKind::
                                            Material);
                            if (decoded) {
                                const Iridium::EditorMeshAuthoringState before =
                                    Iridium::captureEditorMeshAuthoringState(mesh);
                                assignMaterial(
                                    mesh,
                                    sourceGuid,
                                    decoded->guid);
                                commitMeshAuthoringEdit(
                                    transactionService_, registry,
                                    selection_->selected(),
                                    selectedEntityValue, mesh, before,
                                    Iridium::captureEditorMeshAuthoringState(mesh),
                                    "Assign Material");
                            }
                        }
                        ImGui::
                            EndDragDropTarget();
                    }
                    if (ImGui::Button(
                            "Browse...")) {
                        openAssetPicker(
                            AssetPickerKind::
                                Material,
                            sourceGuid);
                    }
                    if (overridden) {
                        ImGui::SameLine();
                        if (ImGui::Button(
                                "Reset")) {
                            const Iridium::EditorMeshAuthoringState before =
                                Iridium::captureEditorMeshAuthoringState(mesh);
                            mesh.materialOverrides
                                .erase(
                                    override);
                            mesh.requestedMaterialAssetRoots
                                .clear();
                            commitMeshAuthoringEdit(
                                transactionService_, registry,
                                selection_->selected(),
                                selectedEntityValue, mesh, before,
                                Iridium::captureEditorMeshAuthoringState(mesh),
                                "Reset Material Override");
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopID();
                    ++visibleIndex;
                    ++slotPosition;
                }
                ImGui::EndTable();
        }
        ImGui::BeginDisabled(
            materialPage_ == 0);
        if (ImGui::Button(
                "Previous materials")) {
            --materialPage_;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%llu / %llu",
            static_cast<
                unsigned long long>(
                materialPage_ + 1),
            static_cast<
                unsigned long long>(
                pageCount));
        ImGui::SameLine();
        ImGui::BeginDisabled(
            materialPage_ + 1 >=
                pageCount);
        if (ImGui::Button(
                "Next materials")) {
            ++materialPage_;
        }
        ImGui::EndDisabled();
    }
}

void InspectorPanel::OnImGuiRender(
    Registry& registry,
    Iridium::AssetManager* assetManager) {
    ImGui::Begin("Inspector");

    if (*selectedEntity == NULL_ENTITY &&
        !materialCatalogRoot_.isNil()) {
        materialCatalogRoot_ = {};
        materialNames_.clear();
        if (thumbnailService_) {
            thumbnailService_
                ->setPinnedDemand(
                    std::span<const
                        Iridium::
                            AssetCatalogRecord>{});
        }
    }
    if (*selectedEntity != NULL_ENTITY) {
        auto* namePool =
            registry.getPool<NameComponent>();
        if (!namePool->has(*selectedEntity)) {
            namePool->add(*selectedEntity, {
                .name = "Entity " +
                    std::to_string(
                        selectedEntity->index()),
            });
        }
        if (nameEntity_ !=
            *selectedEntity) {
            nameEntity_ = *selectedEntity;
            observedName_ = namePool->get(
                *selectedEntity).name;
            nameBuffer_.fill('\0');
            const std::string& current = observedName_;
            std::memcpy(nameBuffer_.data(),
                current.data(),
                (std::min)(
                    current.size(),
                    nameBuffer_.size() - 1));
        }
        const std::string& currentName = namePool->get(
            *selectedEntity).name;
        if (currentName != observedName_) {
            observedName_ = currentName;
            nameBuffer_.fill('\0');
            std::memcpy(nameBuffer_.data(), observedName_.data(),
                (std::min)(observedName_.size(), nameBuffer_.size() - 1));
        }
        if (ImGui::InputText(
                "Name",
                nameBuffer_.data(),
                nameBuffer_.size(),
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string name(
                nameBuffer_.data());
            if (!name.empty()) {
                const std::string replacement =
                    Iridium::uniqueEntityName(
                        registry, name, *selectedEntity);
                const Entity entity = *selectedEntity;
                Iridium::EditorTransaction edit;
                edit.label = "Rename Entity";
                edit.operations.push_back(
                    Iridium::makeEditorValueOperation<std::string>(
                        "entity/name",
                        [&registry, entity]() -> std::string* {
                            auto* pool = registry.findPool<NameComponent>();
                            return pool && pool->has(entity)
                                ? &pool->get(entity).name : nullptr;
                        },
                        observedName_, replacement));
                const auto result = transactionService_->execute(
                    std::move(edit));
                if (result) observedName_ = replacement;
            }
        }
        ImGui::TextDisabled(
            "Entity %u",
            selectedEntity->index());
        ImGui::Separator();

        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.65f, 0.18f,
                0.18f, 1.0f));
        if (ImGui::Button(
                "DELETE ENTITY",
                ImVec2(-1, 0))) {
            if (sceneCommands_) {
                (void)sceneCommands_->deleteEntity(*selectedEntity);
            }
            nameEntity_ = NULL_ENTITY;
            ImGui::PopStyleColor();
            ImGui::End();
            return;
        }
        ImGui::PopStyleColor();
        ImGui::Separator();

        MeshComponent* selectedMesh = nullptr;
        auto* meshPool =
            registry.getPool<MeshComponent>();
        if (meshPool->has(*selectedEntity)) {
            selectedMesh =
                &meshPool->get(*selectedEntity);
        }
        const Iridium::AssetGuid
            selectedModelGuid =
                !selectedMesh
                ? Iridium::AssetGuid{}
                : !selectedMesh
                    ->requestedAssetGuid.isNil()
                    ? selectedMesh
                        ->requestedAssetGuid
                    : !selectedMesh
                        ->assetGuid.isNil()
                        ? selectedMesh->assetGuid
                        : selectedMesh->model
                            ? selectedMesh
                                ->model->assetGuid
                            : Iridium::AssetGuid{};
        if (pinnedModelGuid_ !=
                selectedModelGuid) {
            pinnedModelGuid_ =
                selectedModelGuid;
            if (thumbnailService_) {
                if (assetCatalog_ &&
                    !selectedModelGuid.isNil()) {
                    const auto records =
                        assetCatalog_
                            ->recordsForSourceRoot(
                                selectedModelGuid);
                    thumbnailService_
                        ->setPinnedDemand(
                            records);
                }
                else {
                    thumbnailService_
                        ->setPinnedDemand(
                            std::span<const
                                Iridium::
                                    AssetCatalogRecord>{});
                }
            }
            if (selectedModelGuid.isNil()) {
                materialCatalogRoot_ = {};
                materialNames_.clear();
            }
        }

        for (const auto& descriptor :
            componentRegistry_.descriptors()) {
            if (!descriptor.visible ||
                !descriptor.has(
                    registry, *selectedEntity)) {
                continue;
            }
            void* component = descriptor.getMutable(
                registry, *selectedEntity);
            if (!component) {
                continue;
            }
            const std::string& name =
                descriptor.displayName;

            if (!ImGui::CollapsingHeader(
                    name.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }
            ImGui::Indent();
            ImGui::PushID(
                descriptor.id.value().c_str());
            const bool drawComponent =
                Iridium::EditorComponentUI::
                    beginComponentBody(
                        "component-body",
                        descriptor.preferredBodyHeight);

            if (drawComponent) {
                Iridium::EditorComponentDrawContext context{
                    .component = component,
                    .registry = registry,
                    .entity = *selectedEntity,
                    .assetManager = assetManager
                };
                if (const auto* drawer =
                        drawerRegistry_.find(descriptor.id)) {
                    drawer->draw(context);
                }
                else {
                    (void)Iridium::drawGenericEditorProperties(
                        descriptor, component, {
                            .registry = &registry,
                            .entity = *selectedEntity,
                            .transactions = transactionService_,
                            .sessions = &genericPropertySessions_,
                            .entities = selection_->selected(),
                        });
                }
            }

            if (drawComponent) {
                ImGui::Spacing();
                if (!descriptor.required) {
                    if (ImGui::Button(
                            ("Remove " + name)
                                .c_str())) {
                        if (component == selectedMesh) {
                            selectedMesh = nullptr;
                        }
                        const Entity entity = *selectedEntity;
                        if (descriptor.capture) {
                            const auto has = descriptor.has;
                            const auto remove = descriptor.remove;
                            const auto restore = descriptor.restore;
                            Iridium::EditorTransaction edit;
                            edit.label = "Remove " + name;
                            const auto appendTarget = [&](Entity target) {
                                if (!has(registry, target)) return;
                                const auto snapshot = descriptor.capture(
                                    registry, target);
                                if (!snapshot) return;
                                edit.operations.push_back({
                                    .target = descriptor.id.value(),
                                    .apply = [&registry, target, has, remove] {
                                        if (!has(registry, target)) {
                                            return Iridium::EditorMutationResult::noChange();
                                        }
                                        return remove(registry, target)
                                            ? Iridium::EditorMutationResult::applied()
                                            : Iridium::EditorMutationResult::failure(
                                                "Could not remove component");
                                    },
                                    .revert = [&registry, target, has, restore,
                                        snapshot] {
                                        if (has(registry, target)) {
                                            return Iridium::EditorMutationResult::noChange();
                                        }
                                        return restore && restore(
                                                registry, target, snapshot)
                                            ? Iridium::EditorMutationResult::applied()
                                            : Iridium::EditorMutationResult::failure(
                                                "Could not restore component");
                                    },
                                    .estimatedPayloadBytes = sizeof(snapshot),
                                });
                            };
                            if (selection_->selected().empty()) appendTarget(entity);
                            else for (Entity target : selection_->selected()) {
                                appendTarget(target);
                            }
                            (void)transactionService_->execute(std::move(edit));
                        }
                    }
                }
                else {
                    ImGui::TextDisabled(
                        "(Required Component)");
                }
            }
            Iridium::EditorComponentUI::
                endComponentBody();
            ImGui::PopID();
            ImGui::Unindent();
            ImGui::Separator();
        }

        if (selectedMesh) {
            drawAssetPicker(registry, *selectedEntity, *selectedMesh);
        }

        ImGui::Spacing();
        if (ImGui::Button(
                "Add Component")) {
            ImGui::OpenPopup(
                "AddComponentPopup");
        }
        if (ImGui::BeginPopup(
                "AddComponentPopup")) {
            for (const auto& descriptor :
                componentRegistry_.descriptors()) {
                if (!descriptor.addable ||
                    descriptor.has(
                        registry, *selectedEntity)) {
                    continue;
                }
                if (ImGui::MenuItem(
                        descriptor.displayName.c_str())) {
                    const Entity entity = *selectedEntity;
                    const auto has = descriptor.has;
                    const auto add = descriptor.add;
                    const auto remove = descriptor.remove;
                    Iridium::EditorTransaction edit;
                    edit.label = "Add " + descriptor.displayName;
                    const auto appendTarget = [&](Entity target) {
                        if (has(registry, target)) return;
                        edit.operations.push_back({
                            .target = descriptor.id.value(),
                            .apply = [&registry, target, has, add] {
                                if (has(registry, target)) {
                                    return Iridium::EditorMutationResult::noChange();
                                }
                                return add(registry, target)
                                    ? Iridium::EditorMutationResult::applied()
                                    : Iridium::EditorMutationResult::failure(
                                        "Could not add component");
                            },
                            .revert = [&registry, target, has, remove] {
                                if (!has(registry, target)) {
                                    return Iridium::EditorMutationResult::noChange();
                                }
                                return remove(registry, target)
                                    ? Iridium::EditorMutationResult::applied()
                                    : Iridium::EditorMutationResult::failure(
                                        "Could not undo component addition");
                            },
                        });
                    };
                    if (selection_->selected().empty()) appendTarget(entity);
                    else for (Entity target : selection_->selected()) {
                        appendTarget(target);
                    }
                    (void)transactionService_->execute(std::move(edit));
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}
