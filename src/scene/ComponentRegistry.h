#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>
#include "ecs/Registry.h"
#include "scene/JsonArchive.h"
#include "editor/ImGuiArchive.h"

using SerializeFn = std::function<void(Registry&, Entity, nlohmann::json&)>;
using DeserializeFn = std::function<void(Registry&, Entity, const nlohmann::json&)>;
using DrawFn = std::function<void(Registry&, Entity)>;

using HasFn = std::function<bool(Registry&, Entity)>;
using AddFn = std::function<void(Registry&, Entity)>;
using RemoveFn = std::function<void(Registry&, Entity)>;

struct ComponentFunctions {
    SerializeFn serialize;
    DeserializeFn deserialize;
    DrawFn drawInspector;

    HasFn hasComponent;
    AddFn addComponent;
    RemoveFn removeComponent;
};

class ComponentRegistry {
public:
    static inline std::unordered_map<std::string, ComponentFunctions> RegistryMap;

    // This is the magic template! It generates the functions for ANY component.
    template<typename T>
    static void Register(const std::string& name) {
        ComponentFunctions funcs;

        // 1. Generate the Serialization Lambda
        funcs.serialize = [name](Registry& reg, Entity e, nlohmann::json& entityJson) {
            if (reg.getPool<T>()->has(e)) {
                auto& comp = reg.getComponent<T>(e);
                JsonWriteArchive archive;
                comp.reflect(archive); // The Visitor does the work!
                entityJson[name] = archive.j;
            }
            };

        // 2. Generate the Deserialization Lambda
        funcs.deserialize = [name](Registry& reg, Entity e, const nlohmann::json& entityJson) {
            if (entityJson.contains(name)) {
                T comp; // Create a blank component
                JsonReadArchive archive(entityJson[name]);
                comp.reflect(archive); // Fill it with JSON data
                reg.addComponent<T>(e, comp); // Add it to the ECS!
            }
            };

        // 3. The UI Drawer
        funcs.drawInspector = [name](Registry& reg, Entity e) {
            if (reg.getPool<T>()->has(e)) {
                if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto& comp = reg.getComponent<T>(e);
                    ImGuiArchive archive;
                    comp.reflect(archive); // Magic: Generates the sliders!
                    ImGui::Spacing();
                }
            }
        };

        // 4. The UI Utilities
        funcs.hasComponent = [](Registry& reg, Entity e) { return reg.getPool<T>()->has(e); };
        funcs.addComponent = [](Registry& reg, Entity e) { reg.addComponent<T>(e); };
        funcs.removeComponent = [](Registry& reg, Entity e) { reg.getPool<T>()->remove(e); };

        RegistryMap[name] = funcs;
    }
};