#include "ViewportPanel.h"
#include "imgui.h"
#include "vendor/imguizmo/ImGuizmo.h"

// Bring in the RHI and Component definitions to fix "incomplete type" errors
#include "renderer/rhi/Mesh.h" 
#include "renderer/rhi/RenderDebugView.h"
#include "scene/components/LightComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/TransformComponent.h"
#include "editor/ViewportLayout.h"
#include "editor/ViewportPlacement.h"
#include "editor/ViewportRenderExtent.h"
#include "editor/EditorSceneActions.h"
#include "editor/EditorSceneCommandService.h"
#include "editor/EditorSelectionState.h"
#include "editor/EditorShortcut.h"
#include "editor/EditorTransformSettings.h"
#include "editor/EditorTransactionService.h"
#include "editor/ViewportPicking.h"
#include "profiling/CpuProfiler.h"
#include "assets/AssetBrowserModel.h"
#include "assets/AssetManager.h"
#include "ecs/Registry.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <memory>

namespace {

    template<typename Snapshot>
    [[nodiscard]] bool sameTransform(
        const Snapshot& lhs, const Snapshot& rhs) {
        return glm::all(glm::equal(lhs.position, rhs.position)) &&
            glm::all(glm::equal(lhs.rotation, rhs.rotation)) &&
            glm::all(glm::equal(lhs.scale, rhs.scale));
    }

    struct ViewportProjection {
        glm::mat4 viewProjection{ 1.0f };
        ImVec2 minimum{};
        ImVec2 size{};

        [[nodiscard]] bool project(glm::vec3 world, ImVec2& screen) const {
            const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
            if (!std::isfinite(clip.w) || clip.w <= 0.0001f) return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) ||
                !std::isfinite(ndc.z) || std::abs(ndc.x) > 32.0f ||
                std::abs(ndc.y) > 32.0f) {
                return false;
            }
            // Iridium's Vulkan projection already contains the framebuffer Y
            // orientation. Do not apply the OpenGL/ImGuizmo correction here.
            screen = {
                minimum.x + (ndc.x * 0.5f + 0.5f) * size.x,
                minimum.y + (ndc.y * 0.5f + 0.5f) * size.y,
            };
            return true;
        }

        [[nodiscard]] bool projectSegment(glm::vec3 worldA,
            glm::vec3 worldB, ImVec2& screenA, ImVec2& screenB) const {
            const glm::vec4 clipA = viewProjection * glm::vec4(worldA, 1.0f);
            const glm::vec4 clipB = viewProjection * glm::vec4(worldB, 1.0f);
            float minimumT = 0.0f;
            float maximumT = 1.0f;
            constexpr std::array<glm::vec4, 6> ClipPlanes{{
                { 1.0f, 0.0f, 0.0f, 1.0f },
                { -1.0f, 0.0f, 0.0f, 1.0f },
                { 0.0f, 1.0f, 0.0f, 1.0f },
                { 0.0f, -1.0f, 0.0f, 1.0f },
                { 0.0f, 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f, 1.0f },
            }};
            for (const glm::vec4& plane : ClipPlanes) {
                const float first = glm::dot(clipA, plane);
                const float second = glm::dot(clipB, plane);
                if (first < 0.0f && second < 0.0f) return false;
                if ((first < 0.0f) != (second < 0.0f)) {
                    const float crossing = first / (first - second);
                    if (first < 0.0f) minimumT = (std::max)(minimumT, crossing);
                    else maximumT = (std::min)(maximumT, crossing);
                    if (minimumT > maximumT) return false;
                }
            }
            const glm::vec4 clippedA = glm::mix(clipA, clipB, minimumT);
            const glm::vec4 clippedB = glm::mix(clipA, clipB, maximumT);
            if (clippedA.w <= 1.0e-5f || clippedB.w <= 1.0e-5f) return false;
            const glm::vec2 ndcA = glm::vec2(clippedA) / clippedA.w;
            const glm::vec2 ndcB = glm::vec2(clippedB) / clippedB.w;
            screenA = {
                minimum.x + (ndcA.x * 0.5f + 0.5f) * size.x,
                minimum.y + (ndcA.y * 0.5f + 0.5f) * size.y,
            };
            screenB = {
                minimum.x + (ndcB.x * 0.5f + 0.5f) * size.x,
                minimum.y + (ndcB.y * 0.5f + 0.5f) * size.y,
            };
            return true;
        }
    };

    struct WorldGridPlane {
        glm::vec3 origin{};
        glm::vec3 axisU{ 1.0f, 0.0f, 0.0f };
        glm::vec3 axisV{ 0.0f, 0.0f, 1.0f };
    };

    [[nodiscard]] glm::vec3 normalizedAxis(const glm::mat4& transform,
        size_t column, glm::vec3 fallback) {
        const glm::vec3 axis(transform[column]);
        const float lengthSquared = glm::dot(axis, axis);
        return std::isfinite(lengthSquared) && lengthSquared > 1.0e-8f
            ? axis / std::sqrt(lengthSquared) : fallback;
    }

    [[nodiscard]] WorldGridPlane translationGridPlane(
        const glm::mat4& transform, glm::vec3 worldDelta,
        glm::vec3 cameraForward, bool localSpace) {
        std::array<glm::vec3, 3> axes{
            glm::vec3(1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f),
        };
        if (localSpace) {
            axes[0] = normalizedAxis(transform, 0, axes[0]);
            axes[1] = normalizedAxis(transform, 1, axes[1]);
            axes[2] = normalizedAxis(transform, 2, axes[2]);
        }
        std::array<float, 3> contribution{
            std::abs(glm::dot(worldDelta, axes[0])),
            std::abs(glm::dot(worldDelta, axes[1])),
            std::abs(glm::dot(worldDelta, axes[2])),
        };
        size_t primary = 0;
        if (contribution[1] > contribution[primary]) primary = 1;
        if (contribution[2] > contribution[primary]) primary = 2;
        size_t secondary = primary == 0 ? 1 : 0;
        for (size_t axis = 0; axis < axes.size(); ++axis) {
            if (axis != primary && contribution[axis] >
                    contribution[secondary]) {
                secondary = axis;
            }
        }
        if (contribution[secondary] <= contribution[primary] * 0.18f) {
            float bestFacing = -1.0f;
            for (size_t candidate = 0; candidate < axes.size(); ++candidate) {
                if (candidate == primary) continue;
                const float facing = std::abs(glm::dot(glm::normalize(
                    glm::cross(axes[primary], axes[candidate])),
                    cameraForward));
                if (facing > bestFacing) {
                    bestFacing = facing;
                    secondary = candidate;
                }
            }
        }
        return {
            .origin = glm::vec3(transform[3]),
            .axisU = axes[primary],
            .axisV = axes[secondary],
        };
    }

    void drawWorldLine(ImDrawList& drawList,
        const ViewportProjection& projection, glm::vec3 first,
        glm::vec3 second, ImU32 color, float thickness = 1.5f) {
        ImVec2 a{}, b{};
        if (projection.projectSegment(first, second, a, b)) {
            drawList.AddLine(a, b, color, thickness);
        }
    }

    void drawWorldRing(ImDrawList& drawList,
        const ViewportProjection& projection, glm::vec3 center,
        glm::vec3 axisA, glm::vec3 axisB, ImU32 color,
        float thickness = 1.5f) {
        constexpr uint32_t Segments = 48;
        glm::vec3 previous = center + axisA;
        for (uint32_t segment = 1; segment <= Segments; ++segment) {
            const float angle = 2.0f * std::numbers::pi_v<float> *
                static_cast<float>(segment) / static_cast<float>(Segments);
            const glm::vec3 current = center + axisA * std::cos(angle) +
                axisB * std::sin(angle);
            drawWorldLine(drawList, projection, previous, current, color,
                thickness);
            previous = current;
        }
    }

    void drawWorldBox(ImDrawList& drawList,
        const ViewportProjection& projection, glm::vec3 center,
        glm::vec3 axisX, glm::vec3 axisY, glm::vec3 axisZ,
        glm::vec3 extents, ImU32 color, float thickness = 1.5f) {
        std::array<glm::vec3, 8> corners{};
        for (uint32_t corner = 0; corner < corners.size(); ++corner) {
            corners[corner] = center +
                axisX * extents.x * ((corner & 1u) ? 1.0f : -1.0f) +
                axisY * extents.y * ((corner & 2u) ? 1.0f : -1.0f) +
                axisZ * extents.z * ((corner & 4u) ? 1.0f : -1.0f);
        }
        constexpr std::array<std::array<uint32_t, 2>, 12> Edges{{
            {{0, 1}}, {{2, 3}}, {{4, 5}}, {{6, 7}},
            {{0, 2}}, {{1, 3}}, {{4, 6}}, {{5, 7}},
            {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
        }};
        for (const auto edge : Edges) {
            drawWorldLine(drawList, projection, corners[edge[0]],
                corners[edge[1]], color, thickness);
        }
    }

    void drawDirectionArrow(ImDrawList& drawList,
        const ViewportProjection& projection, glm::vec3 origin,
        glm::vec3 direction, glm::vec3 side, glm::vec3 up, float length,
        ImU32 color) {
        const glm::vec3 tip = origin + direction * length;
        drawWorldLine(drawList, projection, origin, tip, color, 2.25f);
        const float headLength = length * 0.18f;
        const float headWidth = length * 0.08f;
        const glm::vec3 base = tip - direction * headLength;
        drawWorldLine(drawList, projection, tip, base + side * headWidth,
            color, 2.0f);
        drawWorldLine(drawList, projection, tip, base - side * headWidth,
            color, 2.0f);
        drawWorldLine(drawList, projection, tip, base + up * headWidth,
            color, 2.0f);
        drawWorldLine(drawList, projection, tip, base - up * headWidth,
            color, 2.0f);
    }

    void drawSelectedComponentBounds(Registry& registry, Entity entity,
        const glm::mat4& transform, const glm::mat4& view,
        const glm::mat4& projectionMatrix, ImVec2 imageMinimum,
        ImVec2 imageSize) {
        if (entity == NULL_ENTITY || imageSize.x <= 0.0f ||
            imageSize.y <= 0.0f) {
            return;
        }
        ImDrawList& drawList = *ImGui::GetWindowDrawList();
        drawList.PushClipRect(imageMinimum,
            ImVec2(imageMinimum.x + imageSize.x,
                imageMinimum.y + imageSize.y), true);
        const ViewportProjection projection{
            .viewProjection = projectionMatrix * view,
            .minimum = imageMinimum,
            .size = imageSize,
        };
        const glm::vec3 center(transform[3]);
        const glm::vec3 axisX = normalizedAxis(transform, 0,
            glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 axisY = normalizedAxis(transform, 1,
            glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 axisZ = normalizedAxis(transform, 2,
            glm::vec3(0.0f, 0.0f, 1.0f));

        if (auto* lights = registry.findPool<LightComponent>();
            lights && lights->has(entity)) {
            const LightComponent& light = lights->get(entity);
            constexpr ImU32 Outer = IM_COL32(255, 190, 54, 235);
            constexpr ImU32 Inner = IM_COL32(255, 238, 150, 220);
            constexpr ImU32 Source = IM_COL32(255, 246, 205, 210);
            const float range = (std::max)(light.rangeMeters, 0.01f);
            if (light.type == LightType::Point ||
                light.type == LightType::Area) {
                drawWorldRing(drawList, projection, center, axisX * range,
                    axisY * range, Outer);
                drawWorldRing(drawList, projection, center, axisX * range,
                    axisZ * range, Outer);
                drawWorldRing(drawList, projection, center, axisY * range,
                    axisZ * range, Outer);
                const float sourceRadius = std::clamp(
                    light.sourceRadiusMeters, 0.0f, range);
                if (sourceRadius > 0.0f) {
                    drawWorldRing(drawList, projection, center,
                        axisX * sourceRadius, axisY * sourceRadius, Source);
                }
            }
            if (light.type == LightType::Spot) {
                const auto drawCone = [&](float angleDegrees, ImU32 color,
                    bool drawRays) {
                    const float angle = glm::radians(std::clamp(
                        angleDegrees, 0.01f, 89.0f));
                    const float radius = std::tan(angle) * range;
                    const glm::vec3 rimCenter = center + axisZ * range;
                    drawWorldRing(drawList, projection, rimCenter,
                        axisX * radius, axisY * radius, color,
                        drawRays ? 1.8f : 1.4f);
                    if (drawRays) {
                        constexpr uint32_t Rays = 8;
                        for (uint32_t ray = 0; ray < Rays; ++ray) {
                            const float a = 2.0f * std::numbers::pi_v<float> *
                                static_cast<float>(ray) / Rays;
                            const glm::vec3 rim = rimCenter +
                                axisX * (std::cos(a) * radius) +
                                axisY * (std::sin(a) * radius);
                            drawWorldLine(drawList, projection, center, rim,
                                color, 1.4f);
                        }
                    }
                };
                drawCone(light.outerConeDegrees, Outer, true);
                drawCone((std::min)(light.innerConeDegrees,
                    light.outerConeDegrees), Inner, false);
                drawDirectionArrow(drawList, projection, center, axisZ,
                    axisX, axisY, range, Inner);
            }
            if (light.type == LightType::Directional) {
                const glm::vec3 cameraPosition = glm::vec3(
                    glm::inverse(view)[3]);
                const float cameraDistance = glm::length(
                    cameraPosition - center);
                const float arrowLength = std::clamp(
                    cameraDistance * 0.18f, 0.75f, 50.0f);
                drawDirectionArrow(drawList, projection, center, axisZ,
                    axisX, axisY, arrowLength, Outer);
                const float diskRadius = arrowLength * 0.18f;
                drawWorldRing(drawList, projection, center,
                    axisX * diskRadius, axisY * diskRadius, Source);
            }
        }

        if (auto* probes = registry.findPool<
                Iridium::ReflectionProbeComponent>();
            probes && probes->has(entity)) {
            const Iridium::ReflectionProbeComponent& probe =
                probes->get(entity);
            constexpr ImU32 Influence = IM_COL32(45, 218, 255, 235);
            constexpr ImU32 Blend = IM_COL32(132, 238, 255, 150);
            if (probe.shape == Iridium::ReflectionProbeShape::Sphere) {
                const float radius = (std::max)(
                    probe.sphereRadiusMeters, 0.01f);
                drawWorldRing(drawList, projection, center, axisX * radius,
                    axisY * radius, Influence);
                drawWorldRing(drawList, projection, center, axisX * radius,
                    axisZ * radius, Influence);
                drawWorldRing(drawList, projection, center, axisY * radius,
                    axisZ * radius, Influence);
                const float innerRadius = (std::max)(
                    radius - probe.blendDistanceMeters, 0.0f);
                if (innerRadius > 0.0f && innerRadius < radius) {
                    drawWorldRing(drawList, projection, center,
                        axisX * innerRadius, axisY * innerRadius, Blend, 1.2f);
                    drawWorldRing(drawList, projection, center,
                        axisX * innerRadius, axisZ * innerRadius, Blend, 1.2f);
                }
            }
            else {
                const glm::vec3 extents = glm::max(
                    probe.boxExtentsMeters, glm::vec3(0.01f));
                drawWorldBox(drawList, projection, center, axisX, axisY,
                    axisZ, extents, Influence);
                const glm::vec3 inner = glm::max(extents -
                    glm::vec3((std::max)(probe.blendDistanceMeters, 0.0f)),
                    glm::vec3(0.0f));
                if (glm::all(glm::greaterThan(inner, glm::vec3(0.0f))) &&
                    glm::any(glm::notEqual(inner, extents))) {
                    drawWorldBox(drawList, projection, center, axisX, axisY,
                        axisZ, inner, Blend, 1.2f);
                }
            }
            drawDirectionArrow(drawList, projection, center, axisZ,
                axisX, axisY, (std::max)(probe.captureNearMeters * 4.0f,
                    0.35f), Influence);
        }
        drawList.PopClipRect();
    }

}

bool ViewportPanel::drawTransformSettingsPanel(
    const ImVec2& imageMin, const ImVec2& imageSize,
    ImGuizmo::OPERATION currentGizmoOperation,
    Iridium::EditorTransformSettings& settings) {
    if (!transformSettingsPanelOpen_ || imageSize.x < 260.0f ||
        imageSize.y < 300.0f) {
        return false;
    }

    const ImVec2 panelSize{
        (std::min)(330.0f, imageSize.x - 24.0f),
        (std::min)(440.0f, imageSize.y - 24.0f),
    };
    ImGui::SetCursorScreenPos(ImVec2(
        imageMin.x + imageSize.x - panelSize.x - 12.0f,
        imageMin.y + imageSize.y - panelSize.y - 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
        ImVec4(0.055f, 0.065f, 0.080f, 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    const bool visible = ImGui::BeginChild("viewport-transform-settings",
        panelSize, ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoSavedSettings);
    const bool hovered = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows);
    if (visible) {
        ImGui::TextUnformatted("Grid / Snap Settings");
        ImGui::SameLine();
        const float closeWidth = ImGui::CalcTextSize("Close").x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),
            ImGui::GetWindowWidth() - closeWidth - 10.0f));
        if (ImGui::SmallButton("Close")) {
            transformSettingsPanelOpen_ = false;
        }
        ImGui::Separator();

        const int activeSection = currentGizmoOperation == ImGuizmo::ROTATE
            ? 1 : currentGizmoOperation == ImGuizmo::SCALE ? 2 : 0;
        const bool scrollToActive =
            transformSettingsPanelSection_ != activeSection;
        const float globalRegionHeight = 178.0f;
        const float toolRegionHeight = (std::max)(100.0f,
            panelSize.y - globalRegionHeight - 52.0f);
        if (ImGui::BeginChild("transform-tool-options",
                ImVec2(0.0f, toolRegionHeight),
                ImGuiChildFlags_None)) {
            ImGui::SeparatorText("Move");
            if (scrollToActive && activeSection == 0) {
                ImGui::SetScrollHereY(0.0f);
            }
            ImGui::Checkbox("Automatic translation snap",
                &settings.translationSnapEnabled);
            ImGui::Checkbox("Use world grid step",
                &settings.translationSnapUsesGridStep);
            ImGui::BeginDisabled(settings.translationSnapUsesGridStep);
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat("Translation step",
                &settings.translationSnapInWorldUnits,
                0.01f, 0.000001f, FLT_MAX, "%.6g",
                ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine();
            ImGui::TextDisabled("%s",
                Iridium::lengthUnitSymbol(settings.worldUnit).data());
            ImGui::EndDisabled();

            ImGui::SeparatorText("Rotate");
            if (scrollToActive && activeSection == 1) {
                ImGui::SetScrollHereY(0.0f);
            }
            ImGui::Checkbox("Automatic rotation snap",
                &settings.rotationSnapEnabled);
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat("Rotation step (degrees)",
                &settings.rotationSnapDegrees,
                0.25f, 0.001f, 360.0f, "%.4g",
                ImGuiSliderFlags_AlwaysClamp);

            ImGui::SeparatorText("Scale");
            if (scrollToActive && activeSection == 2) {
                ImGui::SetScrollHereY(0.0f);
            }
            ImGui::Checkbox("Automatic scale snap",
                &settings.scaleSnapEnabled);
            ImGui::SetNextItemWidth(150.0f);
            ImGui::DragFloat("Scale step", &settings.scaleSnapIncrement,
                0.01f, 0.000001f, FLT_MAX, "%.6g",
                ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::EndChild();
        transformSettingsPanelSection_ = activeSection;

        ImGui::SeparatorText("Global Grid / Snap");
        ImGui::Checkbox("Show world grid", &settings.gridVisible);
        ImGui::SameLine();
        ImGui::Checkbox("Follow move plane", &settings.gridFollowsTranslation);
        const char* unitLabels[] = { "mm", "cm", "m", "km" };
        int unit = static_cast<int>(settings.worldUnit);
        ImGui::SetNextItemWidth(72.0f);
        if (ImGui::Combo("World unit", &unit, unitLabels,
                IM_ARRAYSIZE(unitLabels))) {
            settings.worldUnit =
                static_cast<Iridium::EditorLengthUnit>(unit);
        }
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat("World grid step",
            &settings.gridStepInWorldUnits,
            0.01f, 0.000001f, FLT_MAX, "%.6g",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
            Iridium::lengthUnitSymbol(settings.worldUnit).data());
        ImGui::TextDisabled("Shift: temporary snap   Ctrl: free transform");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return hovered;
}

void ViewportPanel::render(void* sceneTextureID, void* glassDepthTextureID,
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
    Iridium::EditorTransformSettings* transformSettings) {

    Entity* selectedEntity = selection ? &selection->primary : nullptr;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 });

    // Using "Scene Viewport" as the consistent window name
    isVisible = ImGui::Begin("Scene Viewport");
    if (!isVisible) {
        isHovered = isFocused = false;
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::SetWindowFocus();
    }

    isFocused = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);

    if (transformSettings && isFocused &&
        !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGuizmo::IsUsing() &&
        ImGui::Shortcut(Iridium::ToggleViewportGrid.chord,
            ImGuiInputFlags_RouteFocused)) {
        transformSettings->gridVisible = !transformSettings->gridVisible;
    }

    // ========================================================
    // 1. THE INTEGRATED TOOLBAR
    // ========================================================
    ImGui::SetCursorPos(ImVec2(10, 30));

    // Move Button logic
    bool isMoveActive = (currentGizmoOperation == ImGuizmo::TRANSLATE);
    if (isMoveActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.7f, 0.7f));
    if (ImGui::Button("Move")) currentGizmoOperation = ImGuizmo::TRANSLATE;
    if (isMoveActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Rotate Button logic
    bool isRotateActive = (currentGizmoOperation == ImGuizmo::ROTATE);
    if (isRotateActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.33f, 0.7f, 0.7f));
    if (ImGui::Button("Rotate")) currentGizmoOperation = ImGuizmo::ROTATE;
    if (isRotateActive) ImGui::PopStyleColor();

    ImGui::SameLine();

    // Scale Button logic
    bool isScaleActive = (currentGizmoOperation == ImGuizmo::SCALE);
    if (isScaleActive) ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.66f, 0.7f, 0.7f));
    if (ImGui::Button("Scale")) currentGizmoOperation = ImGuizmo::SCALE;
    if (isScaleActive) ImGui::PopStyleColor();

    if (transformSettings) {
        ImGui::SameLine();
        if (ImGui::Button(transformSettings->transformSpace ==
                Iridium::EditorTransformSpace::World ? "World" : "Local")) {
            transformSettings->transformSpace =
                transformSettings->transformSpace ==
                    Iridium::EditorTransformSpace::World
                ? Iridium::EditorTransformSpace::Local
                : Iridium::EditorTransformSpace::World;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("World axes stay fixed. Local axes follow the selected entity's pivot orientation.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Grid", &transformSettings->gridVisible);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Toggle the world grid (%s).",
                Iridium::ToggleViewportGrid.label.data());
        }
        ImGui::SameLine();
        const bool settingsPanelWasOpen = transformSettingsPanelOpen_;
        if (settingsPanelWasOpen) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.18f, 0.42f, 0.56f, 1.0f));
        }
        if (ImGui::Button("Grid / Snap")) {
            transformSettingsPanelOpen_ = !transformSettingsPanelOpen_;
        }
        if (settingsPanelWasOpen) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Open or close the viewport grid and snapping settings panel.");
        }
    }

    ImGui::SameLine();
    ImGui::Text("  |  View Mode:");
    ImGui::SameLine();

    // Render Mode Selection
    const char* items[] = {
        "Standard", "Wireframe", "Glass Depth", "Diffuse / Base Color",
        "Normals", "Roughness", "Metallic (forward source)",
        "Emissive", "Depth", "AO (canonical)",
        "RGB F0 (canonical)", "F90 (canonical)", "Material ID (canonical)",
        "Material Flags (canonical)", "Closure Class (canonical)",
        "Cluster Occupancy", "Cluster Overflow", "Direct Lighting",
        "Shadow Cascade", "Shadow Visibility", "Transparency Class",
        "Transparency Fallback", "Transparency Interval",
        "Transparency Pyramid Mip", "Transparency Layers",
        "Transparency Overflow"
    };
    ImGui::SetNextItemWidth(180);
    ImGui::Combo("##renderMode", &currentRenderMode, items, IM_ARRAYSIZE(items));
    if (ImGui::IsItemHovered()) {
        const Iridium::RenderDebugView debugView = currentRenderMode < 3
            ? Iridium::RenderDebugView::Final
            : static_cast<Iridium::RenderDebugView>(currentRenderMode - 2);
        ImGui::SetTooltip(
            "%s\nCanonical-only fields require the reference/quality/compact GBuffer.\n"
            "Forward materials now provide matching diagnostics where the source "
            "closure carries the requested field.",
            Iridium::renderDebugViewDescription(debugView).data());
    }

    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    // ========================================================
    // 2. MOUSE MATH & IMAGE DRAWING
    // ========================================================
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    requestedRenderExtent = Iridium::viewportPixelExtent(
        availSize.x, availSize.y,
        framebufferScale.x, framebufferScale.y);
    const Iridium::ViewportFitRect fitted =
        Iridium::fitViewportAspect(
            availSize.x, availSize.y,
            sceneAspect);
    screenPos.x += fitted.offsetX;
    screenPos.y += fitted.offsetY;
    const ImVec2 imageSize{
        fitted.width,
        fitted.height,
    };

    viewportWidth = imageSize.x;
    viewportHeight = imageSize.y;
    screenPosX = screenPos.x;
    screenPosY = screenPos.y;

    ImVec2 absoluteMousePos = ImGui::GetMousePos();
    mouseX = absoluteMousePos.x - screenPos.x;
    mouseY = absoluteMousePos.y - screenPos.y;
    isHovered =
        ImGui::IsMouseHoveringRect(
            screenPos,
            ImVec2(screenPos.x + imageSize.x,
                screenPos.y + imageSize.y));

    // Fix: Using the void* handles passed from the backend
    void* textureToDraw = sceneTextureID;
    if (currentRenderMode == 2) { // 2 matches "Glass Depth"
        textureToDraw = glassDepthTextureID;
    }


    bool viewportClicked = false;
    bool transformSettingsPanelHovered = false;
    // Drawing the viewport image using the API-agnostic handle
    if (viewportWidth > 0.0f && viewportHeight > 0.0f) {
        ImGui::SetCursorScreenPos(screenPos);
        ImGui::Image((ImTextureID)textureToDraw,
            imageSize);
        viewportClicked = ImGui::IsItemClicked(
            ImGuiMouseButton_Left);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(
                        Iridium::
                            kAssetBrowserDragPayloadType
                                .data(),
                        ImGuiDragDropFlags_AcceptBeforeDelivery |
                        ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
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
                    const ImVec2 mouse =
                        ImGui::GetMousePos();
                    ImDrawList* drawList =
                        ImGui::GetWindowDrawList();
                    drawList->AddCircle(
                        mouse, 18.0f,
                        IM_COL32(80, 220, 255, 240),
                        24, 2.0f);
                    drawList->AddLine(
                        ImVec2(mouse.x - 24.0f,
                            mouse.y),
                        ImVec2(mouse.x + 24.0f,
                            mouse.y),
                        IM_COL32(80, 220, 255, 220),
                        1.5f);
                    drawList->AddLine(
                        ImVec2(mouse.x,
                            mouse.y - 24.0f),
                        ImVec2(mouse.x,
                            mouse.y + 24.0f),
                        IM_COL32(80, 220, 255, 220),
                        1.5f);
                    if (assetManager) {
                        if (void* preview =
                                assetManager
                                    ->getEditorThumbnail(
                                        decoded->guid)) {
                            drawList->AddImage(
                                reinterpret_cast<ImTextureID>(
                                    preview),
                                ImVec2(mouse.x + 18.0f,
                                    mouse.y + 18.0f),
                                ImVec2(mouse.x + 114.0f,
                                    mouse.y + 114.0f),
                                ImVec2(0.0f, 0.0f),
                                ImVec2(1.0f, 1.0f),
                                IM_COL32(255, 255, 255,
                                    190));
                        }
                    }
                    if (payload->IsDelivery()) {
                        const auto position =
                            Iridium::
                                viewportDropWorldPosition(
                                    mouseX, mouseY,
                                    viewportWidth,
                                    viewportHeight,
                                    view, proj);
                        if (sceneCommands) {
                            glm::vec3 groundedPosition =
                                position.value_or(glm::vec3{});
                            // Iridium is Y-up; viewport model drops always land
                            // on the world XZ ground plane.
                            groundedPosition.y = 0.0f;
                            (void)sceneCommands->createModel(
                                decoded->guid, "Model",
                                groundedPosition);
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (transformSettings) {
        transformSettingsPanelHovered = drawTransformSettingsPanel(
            screenPos, imageSize, currentGizmoOperation,
            *transformSettings);
    }

    // ========================================================
    // 3. DRAW GIZMOS
    // ========================================================
    {
        Iridium::CpuScope gizmoScope(cpuProfiler, "cpu.editor.gizmo");
        if (gizmoEditActive_ && (!selectedEntity ||
                *selectedEntity != gizmoEntity_ || !selectedTransform)) {
            commitGizmoEdit(registry, transactionService);
        }
        if (selectedTransform) {
            if (selectedEntity) {
                drawSelectedComponentBounds(registry, *selectedEntity,
                    selectedTransform->worldMatrix, view, proj, screenPos,
                    imageSize);
            }
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();

            const ImVec2 imgMin = screenPos;
            const ImVec2 imgMax{
                screenPos.x + imageSize.x,
                screenPos.y + imageSize.y,
            };
            ImGuizmo::SetRect(imgMin.x, imgMin.y,
                imgMax.x - imgMin.x, imgMax.y - imgMin.y);

            // Fix: Use the actual worldMatrix from your component
            glm::mat4 transformMatrix = selectedTransform->worldMatrix;
            const glm::mat4 gridReferenceTransform = transformMatrix;

            // Vulkan flip for ImGuizmo compatibility
            glm::mat4 correctedProj = proj;
            correctedProj[1][1] *= -1;

            float snap[3]{};
            const float* activeSnap = nullptr;
            const ImGuiIO& io = ImGui::GetIO();
            if (transformSettings && !io.KeyCtrl) {
                if ((currentGizmoOperation == ImGuizmo::TRANSLATE ||
                        currentGizmoOperation == ImGuizmo::UNIVERSAL) &&
                    Iridium::transformSnapActive(
                        transformSettings->translationSnapEnabled,
                        io.KeyShift, io.KeyCtrl)) {
                    snap[0] = snap[1] = snap[2] =
                        transformSettings->effectiveTranslationSnapMeters();
                    activeSnap = snap;
                }
                else if (currentGizmoOperation == ImGuizmo::ROTATE &&
                    Iridium::transformSnapActive(
                        transformSettings->rotationSnapEnabled,
                        io.KeyShift, io.KeyCtrl)) {
                    snap[0] = transformSettings->rotationSnapDegrees;
                    activeSnap = snap;
                }
                else if (currentGizmoOperation == ImGuizmo::SCALE &&
                    Iridium::transformSnapActive(
                        transformSettings->scaleSnapEnabled,
                        io.KeyShift, io.KeyCtrl)) {
                    snap[0] = transformSettings->scaleSnapIncrement;
                    activeSnap = snap;
                }
            }
            const bool manipulated = ImGuizmo::Manipulate(
                glm::value_ptr(view), glm::value_ptr(correctedProj),
                currentGizmoOperation,
                transformSettings && transformSettings->transformSpace ==
                    Iridium::EditorTransformSpace::World
                    ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                glm::value_ptr(transformMatrix), nullptr, activeSnap);
            if (manipulated) {
                if (transformSettings &&
                    transformSettings->gridFollowsTranslation &&
                    (currentGizmoOperation == ImGuizmo::TRANSLATE ||
                        currentGizmoOperation == ImGuizmo::UNIVERSAL) &&
                    !translationGridActive_) {
                    const glm::vec3 worldDelta = glm::vec3(transformMatrix[3]) -
                        glm::vec3(gridReferenceTransform[3]);
                    if (glm::dot(worldDelta, worldDelta) > 1.0e-12f) {
                        const glm::vec3 cameraForward = -glm::normalize(
                            glm::vec3(glm::inverse(view)[2]));
                        const WorldGridPlane activePlane =
                            translationGridPlane(gridReferenceTransform,
                                worldDelta, cameraForward,
                                transformSettings->transformSpace ==
                                    Iridium::EditorTransformSpace::Local);
                        translationGridOrigin_ = activePlane.origin;
                        translationGridAxisU_ = activePlane.axisU;
                        translationGridAxisV_ = activePlane.axisV;
                        translationGridActive_ = true;
                    }
                }
                if (!gizmoEditActive_) {
                    gizmoEditActive_ = true;
                    gizmoEntity_ = selectedEntity
                        ? *selectedEntity : NULL_ENTITY;
                    gizmoOperation_ = currentGizmoOperation;
                    gizmoBefore_ = {
                        .position = selectedTransform->position,
                        .rotation = selectedTransform->rotation,
                        .scale = selectedTransform->scale,
                    };
                }

                float translation[3], rotation[3], scale[3];
                ImGuizmo::DecomposeMatrixToComponents(
                    glm::value_ptr(transformMatrix),
                    translation, rotation, scale);

                // ImGuizmo decomposes Euler rotation in degrees, which is also
                // the engine/editor-facing unit used by TransformComponent.
                // Matrix decomposition cannot uniquely recover the signs of a
                // mirrored scale. Preserve channels the active gizmo is not
                // editing so a rotation never resets or moves an existing scale.
                if (currentGizmoOperation == ImGuizmo::TRANSLATE ||
                    currentGizmoOperation == ImGuizmo::UNIVERSAL) {
                    selectedTransform->position = glm::vec3(
                        translation[0], translation[1], translation[2]);
                }
                if (currentGizmoOperation == ImGuizmo::ROTATE ||
                    currentGizmoOperation == ImGuizmo::UNIVERSAL) {
                    selectedTransform->rotation = glm::vec3(
                        rotation[0], rotation[1], rotation[2]);
                }
                if (currentGizmoOperation == ImGuizmo::SCALE ||
                    currentGizmoOperation == ImGuizmo::UNIVERSAL) {
                    selectedTransform->scale = glm::vec3(
                        scale[0], scale[1], scale[2]);
                }

                // Flag for TransformSystem to update the world matrix next frame.
                selectedTransform->isDirty = true;
                gizmoAfter_ = {
                    .position = selectedTransform->position,
                    .rotation = selectedTransform->rotation,
                    .scale = selectedTransform->scale,
                };
            }
            if (gizmoEditActive_ && !ImGuizmo::IsUsing()) {
                commitGizmoEdit(registry, transactionService);
            }
        }
    }

    if (viewportClicked && !transformSettingsPanelHovered &&
        !ImGuizmo::IsOver() &&
        !ImGuizmo::IsUsing() && selection) {
        selection->selectExclusive(Iridium::pickViewportEntity(registry,
            mouseX, mouseY, viewportWidth, viewportHeight, view, proj));
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

Iridium::ViewportGridOverlay ViewportPanel::gridOverlay(
    const glm::mat4& view, const glm::mat4& projection,
    const Iridium::EditorTransformSettings& settings) const noexcept {
    WorldGridPlane plane;
    if (settings.gridFollowsTranslation && translationGridActive_) {
        plane = {
            .origin = translationGridOrigin_,
            .axisU = translationGridAxisU_,
            .axisV = translationGridAxisV_,
        };
    }
    plane.axisU = glm::normalize(plane.axisU);
    plane.axisV = glm::normalize(plane.axisV - plane.axisU *
        glm::dot(plane.axisV, plane.axisU));
    return {
        .inverseViewProjection = glm::inverse(projection * view),
        .origin = plane.origin,
        .axisU = plane.axisU,
        .axisV = plane.axisV,
        .baseSpacingMeters = settings.gridStepMeters(),
        .opacity = 1.0f,
        .visible = settings.gridVisible,
    };
}

void ViewportPanel::commitGizmoEdit(Registry& registry,
    Iridium::EditorTransactionService* transactionService) {
    translationGridActive_ = false;
    if (!gizmoEditActive_) return;
    const Entity entity = gizmoEntity_;
    const GizmoTransformSnapshot before = gizmoBefore_;
    const GizmoTransformSnapshot after = gizmoAfter_;
    gizmoEditActive_ = false;
    gizmoEntity_ = NULL_ENTITY;
    if (entity == NULL_ENTITY || sameTransform(before, after)) {
        return;
    }

    auto* pool = registry.findPool<TransformComponent>();
    if (!pool || !pool->has(entity)) return;
    TransformComponent& transform = pool->get(entity);
    transform.position = before.position;
    transform.rotation = before.rotation;
    transform.scale = before.scale;
    transform.isDirty = true;
    if (!transactionService) return;

    struct State {
        Registry* registry = nullptr;
        Entity entity = NULL_ENTITY;
        GizmoTransformSnapshot before;
        GizmoTransformSnapshot after;

        [[nodiscard]] Iridium::EditorMutationResult write(
            const GizmoTransformSnapshot& expected,
            const GizmoTransformSnapshot& replacement) {
            auto* transforms = registry->findPool<TransformComponent>();
            if (!transforms || !transforms->has(entity)) {
                return Iridium::EditorMutationResult::failure(
                    "Gizmo target is no longer available");
            }
            TransformComponent& current = transforms->get(entity);
            const GizmoTransformSnapshot value{
                .position = current.position,
                .rotation = current.rotation,
                .scale = current.scale,
            };
            if (sameTransform(value, replacement)) {
                return Iridium::EditorMutationResult::noChange();
            }
            if (!sameTransform(value, expected)) {
                return Iridium::EditorMutationResult::failure(
                    "Gizmo target changed outside transaction history");
            }
            current.position = replacement.position;
            current.rotation = replacement.rotation;
            current.scale = replacement.scale;
            current.isDirty = true;
            return Iridium::EditorMutationResult::applied();
        }
    };
    auto state = std::make_shared<State>(State{
        .registry = &registry,
        .entity = entity,
        .before = before,
        .after = after,
    });
    Iridium::EditorTransaction edit;
    switch (gizmoOperation_) {
    case ImGuizmo::ROTATE: edit.label = "Rotate Entity"; break;
    case ImGuizmo::SCALE: edit.label = "Scale Entity"; break;
    default: edit.label = "Move Entity"; break;
    }
    edit.operations.push_back({
        .target = "entity/transform/gizmo",
        .apply = [state] { return state->write(state->before, state->after); },
        .revert = [state] { return state->write(state->after, state->before); },
        .estimatedPayloadBytes = sizeof(State),
    });
    (void)transactionService->execute(std::move(edit));
}
