#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include <typeindex>
#include "Entity.h"

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void remove(Entity entity) = 0;
};

template<typename T>
class ComponentPool : public IComponentPool {
public:
    std::vector<T> components;     // The "Dense" array: Tightly packed data
    std::vector<Entity> entities;  // Map: Dense Index -> Entity ID
    std::unordered_map<Entity, size_t> sparseMap; // Map: Entity ID -> Dense Index

    T& add(Entity entity, T component) {
        sparseMap[entity] = components.size();
        entities.push_back(entity);
        components.push_back(component);
        return components.back();
    }

    T& get(Entity entity) {
        return components[sparseMap[entity]];
    }

    void remove(Entity entity) override {
        size_t index = sparseMap[entity];
        size_t lastIndex = components.size() - 1;

        // Swap with last element to keep dense array contiguous
        components[index] = components[lastIndex];
        entities[index] = entities[lastIndex];

        // Update sparse map for the moved element
        sparseMap[entities[index]] = index;

        components.pop_back();
        entities.pop_back();
        sparseMap.erase(entity);
    }
};

class Registry {
public:
    Entity createEntity() { return nextEntityID++; }

    template<typename T, typename... Args>
    T& addComponent(Entity entity, Args&&... args) {
        // This allows registry.addComponent<TransformComponent>(entity, pos, rot, scale);
        return getPool<T>()->add(entity, T{ std::forward<Args>(args)... });
    }

    template<typename T>
    T& getComponent(Entity entity) {
        return getPool<T>()->get(entity);
    }

    template<typename T>
    ComponentPool<T>* getPool() {
        auto typeIdx = std::type_index(typeid(T));
        if (pools.find(typeIdx) == pools.end()) {
            pools[typeIdx] = std::make_unique<ComponentPool<T>>();
        }
        return static_cast<ComponentPool<T>*>(pools[typeIdx].get());
    }

private:
    uint32_t nextEntityID = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools;
};