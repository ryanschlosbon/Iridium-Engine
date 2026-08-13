#pragma once

#include "Entity.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

// A component's dense slot is indexed by entity index in demand-allocated 4 KiB
// pages. Optional components therefore do not pay for a flat maximum-entity array,
// while lookup avoids per-entry hash nodes and preserves O(1) generation-safe
// access through ComponentPool::entities.
class PagedSparseIndex {
public:
    static constexpr uint32_t Empty =
        (std::numeric_limits<uint32_t>::max)();

    [[nodiscard]] uint32_t get(uint32_t entityIndex) const noexcept {
        const uint32_t pageIndex = entityIndex >> PageShift;
        if (pageIndex >= pages_.size() || !pages_[pageIndex]) return Empty;
        return pages_[pageIndex]->slots[entityIndex & PageMask];
    }

    void set(uint32_t entityIndex, size_t denseIndex) {
        if (denseIndex >= Empty) {
            throw std::length_error("Component pool dense index space exhausted");
        }
        const uint32_t pageIndex = entityIndex >> PageShift;
        if (pageIndex >= pages_.size()) pages_.resize(pageIndex + 1);
        if (!pages_[pageIndex]) pages_[pageIndex] = std::make_unique<Page>();
        Page& page = *pages_[pageIndex];
        uint32_t& slot = page.slots[entityIndex & PageMask];
        if (slot == Empty) ++page.liveCount;
        slot = static_cast<uint32_t>(denseIndex);
    }

    void erase(uint32_t entityIndex) noexcept {
        const uint32_t pageIndex = entityIndex >> PageShift;
        if (pageIndex >= pages_.size() || !pages_[pageIndex]) return;
        Page& page = *pages_[pageIndex];
        uint32_t& slot = page.slots[entityIndex & PageMask];
        if (slot == Empty) return;
        slot = Empty;
        if (--page.liveCount == 0) pages_[pageIndex].reset();
    }

    void clear() noexcept { pages_.clear(); }

    [[nodiscard]] size_t allocatedPageCount() const noexcept {
        size_t count = 0;
        for (const auto& page : pages_) count += page != nullptr;
        return count;
    }

private:
    static constexpr uint32_t PageShift = 10;
    static constexpr uint32_t PageSize = 1u << PageShift;
    static constexpr uint32_t PageMask = PageSize - 1;

    struct Page {
        Page() { slots.fill(Empty); }
        std::array<uint32_t, PageSize> slots{};
        uint32_t liveCount = 0;
    };

    std::vector<std::unique_ptr<Page>> pages_;
};

class RegistryEntityObserver {
public:
    virtual ~RegistryEntityObserver() = default;
    virtual void onEntityCreated(Entity entity) = 0;
    virtual void onEntityDestroying(Entity entity) noexcept = 0;
};

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void remove(Entity entity) = 0;
    virtual void clear() = 0;
    virtual void* getVoid(Entity entity) = 0;
};

template<typename T>
class ComponentPool : public IComponentPool {
public:
    std::vector<T> components;
    std::vector<Entity> entities;
    // The entity index is sufficient for sparse lookup; the dense entity handle
    // at the resulting slot is the generation authority. Demand-allocated pages
    // avoid hash nodes without allowing a recycled index to alias stale data.
    PagedSparseIndex sparseIndex;

    T& add(Entity entity, T component) {
        const uint32_t existing = sparseIndex.get(entity.index());
        if (existing != PagedSparseIndex::Empty) {
            if (entities[existing] != entity) {
                throw std::logic_error(
                    "Component pool contains a stale generation for entity index");
            }
            components[existing] = std::move(component);
            return components[existing];
        }
        const size_t denseIndex = components.size();
        components.push_back(std::move(component));
        try {
            entities.push_back(entity);
            sparseIndex.set(entity.index(), denseIndex);
        }
        catch (...) {
            if (entities.size() > denseIndex) entities.pop_back();
            components.pop_back();
            throw;
        }
        return components.back();
    }

    T& get(Entity entity) {
        const uint32_t found = sparseIndex.get(entity.index());
        if (found == PagedSparseIndex::Empty || entities[found] != entity) {
            throw std::out_of_range("Component does not exist for entity handle");
        }
        return components[found];
    }

    const T& get(Entity entity) const {
        const uint32_t found = sparseIndex.get(entity.index());
        if (found == PagedSparseIndex::Empty || entities[found] != entity) {
            throw std::out_of_range("Component does not exist for entity handle");
        }
        return components[found];
    }

    [[nodiscard]] bool has(Entity entity) const noexcept {
        const uint32_t found = sparseIndex.get(entity.index());
        return found != PagedSparseIndex::Empty && entities[found] == entity;
    }

    void remove(Entity entity) override {
        const uint32_t found = sparseIndex.get(entity.index());
        if (found == PagedSparseIndex::Empty || entities[found] != entity) return;

        const size_t index = found;
        const size_t lastIndex = components.size() - 1;
        if (index != lastIndex) {
            components[index] = std::move(components[lastIndex]);
            entities[index] = entities[lastIndex];
            sparseIndex.set(entities[index].index(), index);
        }
        components.pop_back();
        entities.pop_back();
        sparseIndex.erase(entity.index());
    }

    void* getVoid(Entity entity) override {
        const uint32_t found = sparseIndex.get(entity.index());
        return found == PagedSparseIndex::Empty || entities[found] != entity
            ? nullptr
            : &components[found];
    }

    void clear() override {
        components.clear();
        entities.clear();
        sparseIndex.clear();
    }
};

class Registry {
public:
    explicit Registry(
        RegistryEntityObserver* observer = nullptr,
        uint32_t maximumGeneration =
            (std::numeric_limits<uint32_t>::max)())
        : observer_(observer), maximumGeneration_(maximumGeneration) {
        if (maximumGeneration_ == 0) {
            throw std::invalid_argument("Registry generation zero is reserved");
        }
    }

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = delete;
    Registry& operator=(Registry&&) = delete;

    Entity createEntity() {
        uint32_t index = 0;
        if (!freeIndices_.empty()) {
            index = freeIndices_.back();
            freeIndices_.pop_back();
        }
        else {
            if (generations_.size() >= Entity::NullPart) {
                throw std::overflow_error("Registry entity index space exhausted");
            }
            index = static_cast<uint32_t>(generations_.size());
            generations_.push_back(1);
            alive_.push_back(false);
        }

        alive_[index] = true;
        const Entity entity = Entity::fromParts(index, generations_[index]);
        try {
            if (observer_) observer_->onEntityCreated(entity);
        }
        catch (...) {
            alive_[index] = false;
            freeIndices_.push_back(index);
            throw;
        }
        ++aliveCount_;
        return entity;
    }

    [[nodiscard]] bool isAlive(Entity entity) const noexcept {
        return !entity.isNull() && entity.index() < generations_.size() &&
            alive_[entity.index()] &&
            generations_[entity.index()] == entity.generation();
    }

    [[nodiscard]] size_t aliveCount() const noexcept { return aliveCount_; }

    [[nodiscard]] std::vector<Entity> aliveEntities() const {
        std::vector<Entity> result;
        result.reserve(aliveCount_);
        for (uint32_t index = 0; index < generations_.size(); ++index) {
            if (alive_[index]) {
                result.push_back(Entity::fromParts(index, generations_[index]));
            }
        }
        return result;
    }

    [[nodiscard]] auto& getPools() noexcept { return pools_; }
    [[nodiscard]] const auto& getPools() const noexcept { return pools_; }

    template<typename T, typename... Args>
    T& addComponent(Entity entity, Args&&... args) {
        requireAlive(entity);
        return getPool<T>()->add(entity, T{ std::forward<Args>(args)... });
    }

    template<typename T>
    T& getComponent(Entity entity) {
        requireAlive(entity);
        ComponentPool<T>* pool = findPool<T>();
        if (!pool) throw std::out_of_range("Component pool does not exist");
        return pool->get(entity);
    }

    template<typename T>
    const T& getComponent(Entity entity) const {
        requireAlive(entity);
        const ComponentPool<T>* pool = findPool<T>();
        if (!pool) throw std::out_of_range("Component pool does not exist");
        return pool->get(entity);
    }

    template<typename T>
    ComponentPool<T>* getPool() {
        const auto type = std::type_index(typeid(T));
        const auto found = pools_.find(type);
        if (found != pools_.end()) {
            return static_cast<ComponentPool<T>*>(found->second.get());
        }
        auto pool = std::make_unique<ComponentPool<T>>();
        ComponentPool<T>* result = pool.get();
        pools_.emplace(type, std::move(pool));
        return result;
    }

    template<typename T>
    [[nodiscard]] ComponentPool<T>* findPool() noexcept {
        const auto found = pools_.find(std::type_index(typeid(T)));
        return found == pools_.end()
            ? nullptr
            : static_cast<ComponentPool<T>*>(found->second.get());
    }

    template<typename T>
    [[nodiscard]] const ComponentPool<T>* findPool() const noexcept {
        const auto found = pools_.find(std::type_index(typeid(T)));
        return found == pools_.end()
            ? nullptr
            : static_cast<const ComponentPool<T>*>(found->second.get());
    }

    [[nodiscard]] bool destroyEntity(Entity entity) {
        if (!isAlive(entity)) return false;
        if (generations_[entity.index()] == maximumGeneration_) {
            throw std::overflow_error("Registry entity generation exhausted");
        }
        if (observer_) observer_->onEntityDestroying(entity);
        for (auto& [type, pool] : pools_) {
            (void)type;
            pool->remove(entity);
        }
        alive_[entity.index()] = false;
        ++generations_[entity.index()];
        freeIndices_.push_back(entity.index());
        --aliveCount_;
        return true;
    }

    void clear() {
        for (Entity entity : aliveEntities()) {
            if (generations_[entity.index()] == maximumGeneration_) {
                throw std::overflow_error("Registry entity generation exhausted");
            }
        }
        for (Entity entity : aliveEntities()) {
            if (observer_) observer_->onEntityDestroying(entity);
            ++generations_[entity.index()];
            alive_[entity.index()] = false;
        }
        for (auto& [type, pool] : pools_) {
            (void)type;
            if (pool) pool->clear();
        }
        freeIndices_.clear();
        for (size_t index = generations_.size(); index > 0; --index) {
            freeIndices_.push_back(static_cast<uint32_t>(index - 1));
        }
        aliveCount_ = 0;
    }

    // Exchanges entity/component storage while preserving each Registry object's
    // observer address. SceneWorld uses this only after a staging world validates,
    // so long-lived references to the active Registry remain valid across commit.
    void swapStorage(Registry& other) {
        if (this == &other) return;
        if (maximumGeneration_ != other.maximumGeneration_) {
            throw std::invalid_argument(
                "Registry storage swap requires matching generation limits");
        }
        generations_.swap(other.generations_);
        alive_.swap(other.alive_);
        freeIndices_.swap(other.freeIndices_);
        std::swap(aliveCount_, other.aliveCount_);
        pools_.swap(other.pools_);
    }

private:
    void requireAlive(Entity entity) const {
        if (!isAlive(entity)) {
            throw std::out_of_range("Entity handle is null, dead, or stale");
        }
    }

    RegistryEntityObserver* observer_ = nullptr;
    uint32_t maximumGeneration_ =
        (std::numeric_limits<uint32_t>::max)();
    std::vector<uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<uint32_t> freeIndices_;
    size_t aliveCount_ = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools_;
};
