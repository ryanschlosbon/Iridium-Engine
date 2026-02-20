#pragma once
#include <vector>
#include "ecs/Entity.h"

struct RelationshipComponent {
    Entity parent = NULL_ENTITY;
    std::vector<Entity> children;
    int depth = 0;

    void OnInspector() {
        // Read-only display for now
        ImGui::Text("Depth: %d", depth);
        ImGui::Text("Children: %d", (int)children.size());

        if (parent != NULL_ENTITY) {
            ImGui::Text("Parent ID: %d", (int)parent);
        }
        else {
            ImGui::Text("Parent: None");
        }
    }
};