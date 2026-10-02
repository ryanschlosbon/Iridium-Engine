#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "editor/EditorSceneActions.h"
#include "editor/ViewportLayout.h"
#include "editor/ViewportPlacement.h"
#include "editor/ViewportRenderExtent.h"
#include "editor/ViewportPicking.h"
#include "editor/EditorTransformSettings.h"
#include "editor/EditorShortcut.h"
#include "ecs/Registry.h"
#include "scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>
#include <memory>

namespace {

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " \
                    #condition << " (line " \
                    << __LINE__ << ")\n"; \
                return 1; \
            } \
        } while (false)

    bool close(float lhs, float rhs) {
        return std::abs(lhs - rhs) <
            0.001f;
    }

}

int main() {
    using Iridium::fitViewportAspect;

    const auto wide =
        fitViewportAspect(
            1200.0f, 600.0f,
            16.0f / 9.0f);
    CHECK(close(wide.width,
        1066.6667f));
    CHECK(close(wide.height, 600.0f));
    CHECK(close(wide.offsetX,
        66.6667f));
    CHECK(close(wide.offsetY, 0.0f));
    CHECK(close(wide.width / wide.height,
        16.0f / 9.0f));

    const auto tall =
        fitViewportAspect(
            600.0f, 900.0f,
            16.0f / 9.0f);
    CHECK(close(tall.width, 600.0f));
    CHECK(close(tall.height, 337.5f));
    CHECK(close(tall.offsetX, 0.0f));
    CHECK(close(tall.offsetY, 281.25f));
    CHECK(close(tall.width / tall.height,
        16.0f / 9.0f));

    CHECK(fitViewportAspect(
        0.0f, 100.0f, 1.0f) ==
        Iridium::ViewportFitRect{});

    const glm::mat4 view =
        glm::lookAt(
            glm::vec3(0.0f, 5.0f, 5.0f),
            glm::vec3(0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 projection =
        glm::perspective(
            glm::radians(60.0f),
            16.0f / 9.0f,
            0.1f, 100.0f);
    projection[1][1] *= -1.0f;
    const auto center =
        Iridium::viewportDropWorldPosition(
            800.0f, 450.0f,
            1600.0f, 900.0f,
            view, projection);
    CHECK(center.has_value());
    CHECK(close(center->x, 0.0f));
    CHECK(close(center->y, 0.0f));
    CHECK(close(center->z, 0.0f));
    const glm::mat4 horizontalView = glm::lookAt(
        glm::vec3(0.0f, 4.0f, 5.0f),
        glm::vec3(0.0f, 4.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));
    const auto horizontalFallback =
        Iridium::viewportDropWorldPosition(
            800.0f, 450.0f,
            1600.0f, 900.0f,
            horizontalView, projection);
    CHECK(horizontalFallback.has_value());
    CHECK(close(horizontalFallback->y, 0.0f));
    CHECK(!Iridium::viewportDropWorldPosition(
        0.0f, 0.0f, 0.0f, 900.0f,
        view, projection));

    CHECK(close(Iridium::metresPerUnit(
        Iridium::EditorLengthUnit::Millimetres), 0.001f));
    CHECK(close(Iridium::metresPerUnit(
        Iridium::EditorLengthUnit::Centimetres), 0.01f));
    Iridium::EditorTransformSettings transformSettings;
    CHECK(transformSettings.gridVisible);
    CHECK(transformSettings.gridFollowsTranslation);
    CHECK(Iridium::ToggleViewportGrid.group ==
        Iridium::EditorShortcutGroup::Viewport);
    CHECK(Iridium::ToggleViewportGrid.chord ==
        (ImGuiMod_Alt | ImGuiKey_G));
    transformSettings.worldUnit = Iridium::EditorLengthUnit::Centimetres;
    transformSettings.gridStepInWorldUnits = 10.0f;
    transformSettings.translationSnapInWorldUnits = 2.5f;
    CHECK(close(transformSettings.gridStepMeters(), 0.1f));
    CHECK(close(transformSettings.translationSnapMeters(), 0.025f));
    CHECK(close(transformSettings.effectiveTranslationSnapMeters(), 0.1f));
    transformSettings.translationSnapUsesGridStep = false;
    CHECK(close(transformSettings.effectiveTranslationSnapMeters(), 0.025f));
    CHECK(!Iridium::transformSnapActive(false, false, false));
    CHECK(Iridium::transformSnapActive(true, false, false));
    CHECK(Iridium::transformSnapActive(false, true, false));
    CHECK(!Iridium::transformSnapActive(true, true, true));
    CHECK(close(Iridium::adaptiveGridSpacingMeters(0.5f, 1.0f), 0.01f));
    CHECK(close(Iridium::adaptiveGridSpacingMeters(80.0f, 1.0f), 10.0f));

    Registry pickingRegistry;
    const Entity nearEntity = pickingRegistry.createEntity();
    const Entity farEntity = pickingRegistry.createEntity();
    auto modelAsset = std::make_shared<Iridium::ModelAsset>();
    modelAsset->subMeshes.push_back({
        .boundsMin = glm::vec3(-0.5f),
        .boundsMax = glm::vec3(0.5f),
    });
    pickingRegistry.addComponent<TransformComponent>(nearEntity);
    pickingRegistry.addComponent<MeshComponent>(nearEntity).model = modelAsset;
    auto& farTransform = pickingRegistry.addComponent<TransformComponent>(
        farEntity);
    farTransform.worldMatrix = glm::translate(glm::mat4(1.0f),
        glm::vec3(0.0f, 0.0f, -3.0f));
    pickingRegistry.addComponent<MeshComponent>(farEntity).model = modelAsset;
    const glm::mat4 pickView = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f),
        glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 pickProjection = glm::perspective(glm::radians(60.0f),
        16.0f / 9.0f, 0.1f, 100.0f);
    pickProjection[1][1] *= -1.0f;
    CHECK(Iridium::pickViewportEntity(pickingRegistry, 800.0f, 450.0f,
        1600.0f, 900.0f, pickView, pickProjection) == nearEntity);
    CHECK(Iridium::pickViewportEntity(pickingRegistry, 0.0f, 0.0f,
        1600.0f, 900.0f, pickView, pickProjection) == NULL_ENTITY);

    const Iridium::RenderExtent dpiExtent =
        Iridium::viewportPixelExtent(
            800.0f, 450.0f, 1.5f, 1.5f);
    CHECK(dpiExtent.width == 1200);
    CHECK(dpiExtent.height == 675);
    CHECK(Iridium::viewportPixelExtent(
        40.0f, 40.0f, 1.0f, 1.0f).width == 0);
    const Iridium::RenderExtent clampedExtent =
        Iridium::viewportPixelExtent(
            10000.0f, 9000.0f, 1.0f, 1.0f);
    CHECK(clampedExtent.width == 8192);
    CHECK(clampedExtent.height == 8192);

    const Iridium::RenderExtent committedExtent{ 1259, 648 };
    const float committedAspect = static_cast<float>(committedExtent.width) /
        static_cast<float>(committedExtent.height);
    const auto committedFit = fitViewportAspect(
        1259.0f, 648.0f, committedAspect);
    CHECK(close(committedFit.width, 1259.0f));
    CHECK(close(committedFit.height, 648.0f));
    CHECK(close(committedFit.offsetX, 0.0f));
    CHECK(close(committedFit.offsetY, 0.0f));
    const auto pendingFit = fitViewportAspect(
        1259.0f, 648.0f, 16.0f / 9.0f);
    CHECK(close(pendingFit.width / pendingFit.height, 16.0f / 9.0f));
    CHECK(pendingFit.width <= 1259.0f && pendingFit.height <= 648.0f);
    glm::mat4 committedProjection = glm::perspective(
        glm::radians(60.0f), committedAspect, 0.1f, 100.0f);
    committedProjection[1][1] *= -1.0f;
    CHECK(close(std::abs(committedProjection[1][1] /
        committedProjection[0][0]), committedAspect));

    Iridium::ViewportRenderExtentPolicy resizePolicy(100);
    const Iridium::RenderExtent activeExtent{ 1600, 900 };
    const Iridium::RenderExtent requestedExtent{ 1200, 675 };
    CHECK(!resizePolicy.observe(requestedExtent, activeExtent, 0));
    CHECK(!resizePolicy.observe(requestedExtent, activeExtent, 99));
    const auto debounced =
        resizePolicy.observe(requestedExtent, activeExtent, 100);
    CHECK(debounced);
    CHECK(debounced->width == requestedExtent.width);
    CHECK(debounced->height == requestedExtent.height);
    CHECK(!resizePolicy.observe(requestedExtent, activeExtent, 200));
    CHECK(!resizePolicy.observe(requestedExtent, requestedExtent, 201));

    const Iridium::RenderExtent rapidExtent{ 900, 1400 };
    CHECK(!resizePolicy.observe(requestedExtent, activeExtent, 300));
    CHECK(!resizePolicy.observe(rapidExtent, activeExtent, 350));
    CHECK(!resizePolicy.observe(rapidExtent, activeExtent, 449));
    const auto rapidDebounced =
        resizePolicy.observe(rapidExtent, activeExtent, 450);
    CHECK(rapidDebounced);
    CHECK(rapidDebounced->width == 900);
    CHECK(rapidDebounced->height == 1400);
    resizePolicy.reportFailure(450);
    CHECK(!resizePolicy.observe(rapidExtent, activeExtent, 549));
    CHECK(resizePolicy.observe(rapidExtent, activeExtent, 550));
    CHECK(!resizePolicy.observe({}, activeExtent, 551));
    CHECK(!resizePolicy.observe(rapidExtent, activeExtent, 552));

    Registry registry;
    const Entity first =
        Iridium::createEmptyEditorEntity(
            registry, "Model");
    const Entity second =
        Iridium::createEmptyEditorEntity(
            registry, "Model");
    CHECK(registry.getPool<NameComponent>()
        ->get(first).name == "Model");
    CHECK(registry.getPool<NameComponent>()
        ->get(second).name == "Model (2)");
    const Iridium::AssetGuid modelGuid =
        Iridium::createAssetGuidV7();
    const Entity model =
        Iridium::createModelEditorEntity(
            registry, modelGuid,
            "Model",
            glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(registry.getPool<NameComponent>()
        ->get(model).name == "Model (3)");
    CHECK(registry.getPool<TransformComponent>()
        ->get(model).position ==
            glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(registry.getPool<MeshComponent>()
        ->get(model).requestedAssetGuid ==
            modelGuid);
    return 0;
}
