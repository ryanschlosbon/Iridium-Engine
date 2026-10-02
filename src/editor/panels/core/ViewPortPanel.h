#pragma once
#include <imgui.h>
#include "vendor/imguizmo/ImGuizmo.h"
#include "ecs/Entity.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/ViewportGridOverlay.h"
#include <glm/glm.hpp>

struct TransformComponent;
class Registry;
namespace Iridium {
    class AssetManager;
    class CpuProfiler;
    class EditorSceneCommandService;
    struct EditorSelectionState;
    struct EditorTransformSettings;
    class EditorTransactionService;
}

class ViewportPanel {
public:
    // We pass in the texture and render mode so the panel can draw them
    void render(void* sceneTextureID, void* glassDepthTextureID,
        int& currentRenderMode, ImGuizmo::OPERATION& currentGizmoOperation,
        const glm::mat4& view, const glm::mat4& proj,
        TransformComponent* selectedTransform,
        float sceneAspect,
        Registry& registry,
        Iridium::EditorSelectionState* selection,
        Iridium::AssetManager* assetManager,
        Iridium::EditorTransactionService* transactionService,
        Iridium::EditorSceneCommandService* sceneCommands,
        Iridium::CpuProfiler* cpuProfiler,
        Iridium::EditorTransformSettings* transformSettings);

    // Application.cpp will read these to figure out what the mouse is doing!
    bool isHovered = false;
    bool isVisible = true;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float screenPosX = 0.0f;
    float screenPosY = 0.0f;
    bool isFocused = false; 
    Iridium::RenderExtent requestedRenderExtent{};

    [[nodiscard]] Iridium::ViewportGridOverlay gridOverlay(
        const glm::mat4& view, const glm::mat4& projection,
        const Iridium::EditorTransformSettings& settings) const noexcept;

private:
    struct GizmoTransformSnapshot {
        glm::vec3 position{};
        glm::vec3 rotation{};
        glm::vec3 scale{1.0f};
    };

    void commitGizmoEdit(Registry& registry,
        Iridium::EditorTransactionService* transactionService);
    [[nodiscard]] bool drawTransformSettingsPanel(
        const ImVec2& imageMin, const ImVec2& imageSize,
        ImGuizmo::OPERATION currentGizmoOperation,
        Iridium::EditorTransformSettings& settings);

    bool gizmoEditActive_ = false;
    Entity gizmoEntity_ = NULL_ENTITY;
    ImGuizmo::OPERATION gizmoOperation_ = ImGuizmo::TRANSLATE;
    GizmoTransformSnapshot gizmoBefore_;
    GizmoTransformSnapshot gizmoAfter_;
    bool translationGridActive_ = false;
    glm::vec3 translationGridOrigin_{};
    glm::vec3 translationGridAxisU_{ 1.0f, 0.0f, 0.0f };
    glm::vec3 translationGridAxisV_{ 0.0f, 0.0f, 1.0f };
    bool transformSettingsPanelOpen_ = true;
    int transformSettingsPanelSection_ = -1;
};
