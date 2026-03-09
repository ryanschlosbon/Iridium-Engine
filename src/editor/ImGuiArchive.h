#pragma once
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include "utils/FileDialogs.h"

class ImGuiArchive {
public:
    bool operator()(const char* name, float& val) {
        ImGui::PushID(name);
        bool changed = ImGui::DragFloat(name, &val, 0.1f);
        ImGui::PopID();
        return changed;
    }

    bool operator()(const char* name, int& val) {
        ImGui::PushID(name);
        bool changed = ImGui::DragInt(name, &val);
        ImGui::PopID();
        return changed;
    }

    bool operator()(const char* name, bool& val) {
        ImGui::PushID(name);
        bool changed = ImGui::Checkbox(name, &val);
        ImGui::PopID();
        return changed;
    }

    bool operator()(const char* name, std::string& val) {
        ImGui::PushID(name);
        char buffer[256];
        strncpy(buffer, val.c_str(), sizeof(buffer));
        bool changed = ImGui::InputText(name, buffer, sizeof(buffer));
        if (changed) val = std::string(buffer);
        ImGui::PopID();
        return changed;
    }

    bool operator()(const char* name, glm::vec3& val) {
        ImGui::PushID(name);
        bool changed = false;
        std::string strName(name);
        if (strName.find("color") != std::string::npos || strName.find("Color") != std::string::npos) {
            changed = ImGui::ColorEdit3(name, glm::value_ptr(val));
        }
        else {
            changed = ImGui::DragFloat3(name, glm::value_ptr(val), 0.1f);
        }
        ImGui::PopID();
        return changed;
    }

    bool operator()(const char* name, glm::vec4& val) {
        ImGui::PushID(name);
        bool changed = false;
        std::string strName(name);
        if (strName.find("color") != std::string::npos || strName.find("Color") != std::string::npos) {
            changed = ImGui::ColorEdit4(name, glm::value_ptr(val));
        }
        else {
            changed = ImGui::DragFloat4(name, glm::value_ptr(val), 0.1f);
        }
        ImGui::PopID();
        return changed;
    }

    bool filePath(const char* name, std::string& val) {
        ImGui::PushID(name);

        ImGui::Text("%s", name);

        float inputWidth = ImGui::GetContentRegionAvail().x - 35.0f;
        ImGui::PushItemWidth(inputWidth);

        char buffer[256];
        strncpy(buffer, val.c_str(), sizeof(buffer));
        bool changed = ImGui::InputText("##path_input", buffer, sizeof(buffer));
        if (changed) val = std::string(buffer);

        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::Button("...", ImVec2(30, 0))) {

            // Default to all files
            std::vector<std::string> filters = { "All Files", "*" };
            std::string strName(name);

            // Smart Filter Detection!
            if (strName.find("Mesh") != std::string::npos || strName.find("Model") != std::string::npos) {
                filters = { "3D Models", "*.gltf *.glb *.obj" };
            }
            else if (strName.find("Image") != std::string::npos || strName.find("Texture") != std::string::npos) {
                filters = { "Images", "*.png *.jpg *.hdr" };
            }

            std::string selectedPath = FileDialogs::OpenFile("Select File", filters);
            if (!selectedPath.empty()) {
                val = selectedPath;
                changed = true;
            }
        }

        ImGui::PopID();
        return changed;
    }

    bool readOnly(const char* name, const std::string& val) {
        ImGui::TextDisabled("%s: %s", name, val.empty() ? "None" : val.c_str());
        return false;
    }

    bool button(const char* name) {
        return ImGui::Button(name);
    }
};