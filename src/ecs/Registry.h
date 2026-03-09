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
    virtual void clear() = 0;
    // virtual bool has(Entity entity) = 0;
    virtual void* getVoid(Entity entity) = 0;
};

template<typename T>
class ComponentPool : public IComponentPool {
public:
    std::vector<T> components;     // The "Dense" array: Tightly packed data
    std::vector<Entity> entities;  // Map: Dense Index -> Entity ID
    std::unordered_map<Entity, size_t> sparseMap; // Map: Entity ID -> Dense Index

    T& add(Entity entity, T component) {
        // CRASH FIX: Check if it already exists! This is a second fallback. Handling is first done in inspector
        if (has(entity)) {
            // Just overwrite the existing data
            components[sparseMap[entity]] = component;
            return components[sparseMap[entity]];
        }

        // Standard Add Logic
        sparseMap[entity] = components.size();
        entities.push_back(entity);
        components.push_back(component);
        return components.back();
    }

    T& get(Entity entity) {
        return components[sparseMap[entity]];
    }

    bool has(Entity entity) {
        return sparseMap.find(entity) != sparseMap.end();
    }

    void remove(Entity entity) override {
        if (sparseMap.find(entity) == sparseMap.end()) return;

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

    void* getVoid(Entity entity) override {
        if (!has(entity)) return nullptr;
        return &components[sparseMap[entity]];
    }

    void clear() override {
        components.clear();
        entities.clear();
        sparseMap.clear();
    }
};

class Registry {
public:
    Entity createEntity() { return nextEntityID++; }
    auto& getPools() { return pools; }

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

    void destroyEntity(Entity entity) {
        // Iterate over all active pools
        for (auto& pair : pools) {
            // We need to check if the entity is actually IN this pool before removing
            // But since 'IComponentPool' is generic, we rely on the 'remove' implementation 
            // to be safe.
            pair.second->remove(entity);
        }
    }

    void clear() {
        // 1. Reset Entity ID Counter
        nextEntityID = 0;

        // 2. Clear every existing Component Pool
        for (auto& pair : pools) {
            // pair.second is the unique_ptr<IComponentPool>
            if (pair.second) {
                pair.second->clear();
            }
        }
    }

    uint32_t getMaxEntities() const { return nextEntityID; }

private:
    uint32_t nextEntityID = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools;
};