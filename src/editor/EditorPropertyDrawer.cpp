#include "editor/EditorPropertyDrawer.h"
#include "editor/EditorPropertyTransaction.h"
#include "editor/EditorTransactionService.h"

#include "assets/AssetGuid.h"
#include "ecs/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace Iridium {
    namespace {

        static_assert(sizeof(int) == sizeof(int32_t));

        int resizeString(ImGuiInputTextCallbackData* data) {
            if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) return 0;
            auto* value = static_cast<std::string*>(data->UserData);
            value->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = value->data();
            return 0;
        }

        bool drawString(const char* label, std::string& value) {
            if (value.capacity() == 0) value.reserve(16);
            return ImGui::InputText(label, value.data(), value.capacity() + 1,
                ImGuiInputTextFlags_CallbackResize, resizeString, &value);
        }

        bool drawEnum(const EditorPropertyDescriptor& property, void* value) {
            if (property.enumLabels.empty()) {
                ImGui::TextDisabled("%s: enum labels unavailable",
                    property.displayName.c_str());
                return false;
            }
            int32_t current = 0;
            std::memcpy(&current, value, sizeof(current));
            const bool valid = current >= 0 &&
                static_cast<size_t>(current) < property.enumLabels.size();
            const char* preview = valid
                ? property.enumLabels[static_cast<size_t>(current)].c_str()
                : "<invalid>";
            bool changed = false;
            if (ImGui::BeginCombo(property.displayName.c_str(), preview)) {
                for (size_t index = 0; index < property.enumLabels.size(); ++index) {
                    const bool selected = current == static_cast<int32_t>(index);
                    if (ImGui::Selectable(
                            property.enumLabels[index].c_str(), selected)) {
                        current = static_cast<int32_t>(index);
                        std::memcpy(value, &current, sizeof(current));
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            return changed;
        }

    } // namespace

    uint64_t EditorPropertyEditSessionState::observe(
        uint32_t itemId, bool activated) noexcept {
        if (itemId == 0) return 0;
        if (activated || itemId != activeItemId) {
            activeItemId = itemId;
            activeSession = nextSession++;
            if (nextSession == 0) nextSession = 1;
        }
        return activeSession;
    }

    bool drawGenericEditorProperties(
        const EditorComponentDescriptor& descriptor,
        void* component,
        GenericEditorPropertyDrawContext context) {
        if (!component) return false;
        bool changed = false;
        for (const auto& property : descriptor.properties) {
            ImGui::PushID(property.id.value().c_str());
            void* value = property.getMutable(component);
            if (!value) {
                ImGui::TextDisabled("%s: %s", property.displayName.c_str(),
                    property.nullable ? "None" : "Unavailable");
                ImGui::PopID();
                continue;
            }
            ImGui::BeginDisabled(property.readOnly);
            const auto before = captureEditorPropertyValue(
                property, component);
            bool propertyChanged = false;
            bool hasInteractiveItem = true;
            switch (property.valueType) {
            case PropertyValueType::Boolean:
                propertyChanged = ImGui::Checkbox(property.displayName.c_str(),
                    static_cast<bool*>(value));
                break;
            case PropertyValueType::Int32: {
                auto* integer = static_cast<int32_t*>(value);
                const int minimum = property.minimum
                    ? static_cast<int>(*property.minimum) : 0;
                const int maximum = property.maximum
                    ? static_cast<int>(*property.maximum) : 0;
                propertyChanged = ImGui::DragInt(property.displayName.c_str(),
                    reinterpret_cast<int*>(integer), 1.0f, minimum, maximum,
                    "%d", property.minimum
                        ? ImGuiSliderFlags_AlwaysClamp
                        : ImGuiSliderFlags_None);
                break;
            }
            case PropertyValueType::Float32: {
                const float minimum = property.minimum.value_or(0.0f);
                const float maximum = property.maximum.value_or(0.0f);
                propertyChanged = ImGui::DragFloat(property.displayName.c_str(),
                    static_cast<float*>(value), 0.1f, minimum, maximum,
                    "%.3f", property.minimum
                        ? ImGuiSliderFlags_AlwaysClamp
                        : ImGuiSliderFlags_None);
                break;
            }
            case PropertyValueType::String:
                propertyChanged = drawString(property.displayName.c_str(),
                    *static_cast<std::string*>(value));
                break;
            case PropertyValueType::Float32x3:
                propertyChanged = ImGui::DragFloat3(property.displayName.c_str(),
                    glm::value_ptr(*static_cast<glm::vec3*>(value)), 0.1f);
                break;
            case PropertyValueType::Enum:
                propertyChanged = drawEnum(property, value);
                break;
            case PropertyValueType::EntityReference: {
                const Entity entity = *static_cast<Entity*>(value);
                if (entity.isNull()) {
                    ImGui::TextDisabled("%s: None",
                        property.displayName.c_str());
                }
                else {
                    ImGui::Text("%s: %u:%u", property.displayName.c_str(),
                        entity.index(), entity.generation());
                }
                hasInteractiveItem = false;
                break;
            }
            case PropertyValueType::AssetReference:
            case PropertyValueType::SubassetReference: {
                const auto& guid = *static_cast<AssetGuid*>(value);
                const std::string text = guid.isNil() ? "None" : guid.toString();
                ImGui::TextWrapped("%s: %s", property.displayName.c_str(),
                    text.c_str());
                hasInteractiveItem = false;
                break;
            }
            case PropertyValueType::Collection:
                ImGui::TextDisabled("%s: custom collection drawer required",
                    property.displayName.c_str());
                hasInteractiveItem = false;
                break;
            }
            uint64_t session = 0;
            const uint32_t itemId = hasInteractiveItem
                ? ImGui::GetItemID() : 0;
            if (hasInteractiveItem && context.sessions) {
                session = context.sessions->observe(
                    itemId, ImGui::IsItemActivated());
            }
            if (propertyChanged && before) {
                const auto after = captureEditorPropertyValue(
                    property, component);
                const EditorMutationResult restored =
                    writeEditorPropertyValue(property, component, *before);
                if (after && restored && context.transactions &&
                    context.registry) {
                        EditorTransaction edit;
                        edit.label = "Edit " + property.displayName;
                        edit.coalescingKey = descriptor.id.value() + "/" +
                            property.id.value() + "/" +
                            std::to_string(context.entity.index()) + ":" +
                            std::to_string(context.entity.generation()) + "/" +
                            std::to_string(itemId);
                        edit.coalescingSession = session;
                        const auto appendTarget = [&](Entity entity) {
                            void* targetComponent = descriptor.getMutable(
                                *context.registry, entity);
                            if (!targetComponent) return;
                            const auto targetBefore =
                                captureEditorPropertyValue(
                                    property, targetComponent);
                            if (!targetBefore) return;
                            edit.operations.push_back(
                                makeEditorPropertyOperation(
                                    *context.registry, entity, descriptor,
                                    property, *targetBefore, *after));
                        };
                        if (context.entities.empty()) {
                            appendTarget(context.entity);
                        }
                        else {
                            for (Entity entity : context.entities) {
                                appendTarget(entity);
                            }
                        }
                        changed |= static_cast<bool>(
                            context.transactions->execute(std::move(edit)));
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        return changed;
    }

} // namespace Iridium
