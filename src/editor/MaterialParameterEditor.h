#pragma once
#include "material/MaterialAuthoringPatch.h"
#include <imgui.h>

namespace Iridium {
    // Shared drawer for Asset Details and future viewer/graph output controls.
    // This edits a sparse document, never a loaded renderer object directly.
    inline bool drawMaterialParameterEditor(nlohmann::json& settings,
        const std::string& guid, const nlohmann::json& inherited) {
        using Json = nlohmann::json;
        Json patch = settings.value("material_overrides", Json::object()).value(
            guid, Json{{"schema_version", 1}, {"values", Json::object()}});
        Json& values = patch["values"];
        bool changed = false;
        ImGui::SeparatorText("Material parameters (shared asset)");
        ImGui::TextWrapped("Overrides survive reimport. Unchecked fields inherit the source. "
            "Apply updates all users of this material, including material-slot overrides referencing it.");
        for (const auto& field : materialAuthoringFields) {
            const bool specGloss = inherited.contains("/extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor");
            if (std::string_view(field.path).starts_with("/extensions/KHR_materials_pbrSpecularGlossiness/") && !specGloss) continue;
            if (std::string_view(field.path).starts_with("/pbrMetallicRoughness/") && specGloss) continue;
            const bool textureScalar = std::string_view(field.path).starts_with("/normalTexture") ||
                std::string_view(field.path).starts_with("/occlusionTexture");
            if (textureScalar && !inherited.contains(field.path)) continue;
            ImGui::PushID(field.path);
            Json fallback = field.components == 1 ? Json(field.defaultValue) : Json::array();
            if (field.components > 1)
                for (unsigned i = 0; i < field.components; ++i) fallback.push_back(field.defaultValue);
            const Json source = inherited.value(field.path, fallback);
            const bool infinite = source == "infinity";
            bool overridden = values.contains(field.path);
            if (ImGui::Checkbox("##override", &overridden)) {
                if (overridden) values[field.path] = infinite ? fallback : source;
                else values.erase(field.path);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Override this field; uncheck to reset to imported value");
            ImGui::SameLine();
            const Json effective = values.value(field.path, infinite ? fallback : source);
            float v[4]{};
            for (unsigned i = 0; i < field.components; ++i)
                v[i] = field.components == 1 ? effective.get<float>() : effective.at(i).get<float>();
            ImGui::BeginDisabled(!overridden);
            bool edited = field.components == 4 ? ImGui::ColorEdit4(field.label, v, ImGuiColorEditFlags_Float) :
                field.components == 3 ? ImGui::ColorEdit3(field.label, v, ImGuiColorEditFlags_Float) :
                ImGui::DragFloat(field.label, v, .01f, field.minimum, field.maximum, "%.4g", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            if (!overridden && infinite) ImGui::TextDisabled("Inherited: infinite (no distance attenuation)");
            if (edited) {
                Json value = field.components == 1 ? Json(v[0]) : Json::array();
                if (field.components > 1)
                    for (unsigned i = 0; i < field.components; ++i) value.push_back(v[i]);
                values[field.path] = std::move(value);
                changed = true;
            }
            ImGui::PopID();
        }
        for (const char* path : {"/alphaMode", "/doubleSided"}) {
            ImGui::PushID(path);
            bool overridden = values.contains(path);
            const Json source = inherited.value(path, std::string_view(path) == "/alphaMode" ? Json("OPAQUE") : Json(false));
            if (ImGui::Checkbox("##override", &overridden)) {
                if (overridden) values[path] = source; else values.erase(path);
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!overridden);
            if (std::string_view(path) == "/doubleSided") {
                bool v = values.value(path, source).get<bool>();
                if (ImGui::Checkbox("Double sided", &v)) { values[path] = v; changed = true; }
            } else {
                const std::string mode = values.value(path, source).get<std::string>();
                int v = mode == "BLEND" ? 2 : mode == "MASK" ? 1 : 0;
                if (ImGui::Combo("Coverage", &v, "Opaque\0Alpha clip\0Blend\0")) {
                    values[path] = v == 2 ? "BLEND" : v == 1 ? "MASK" : "OPAQUE";
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (ImGui::Button("Reset all material parameters to source")) { values.clear(); changed = true; }
        if (changed) {
            if (!settings.contains("material_overrides")) settings["material_overrides"] = Json::object();
            if (values.empty()) settings["material_overrides"].erase(guid);
            else settings["material_overrides"][guid] = std::move(patch);
            if (settings["material_overrides"].empty()) settings.erase("material_overrides");
        }
        return changed;
    }
}
