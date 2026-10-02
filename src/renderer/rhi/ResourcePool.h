#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Iridium {

    template <typename T, typename HandleType>
    class ResourcePool {
    private:
        struct Slot {
            T payload;
            uint32_t generation = 1;
            bool isFree = true;
        };

        std::vector<Slot> pool;
        std::vector<uint32_t> freeIndices;

        static constexpr size_t MaxCapacity = static_cast<size_t>(HandleType::MaxIndex) + 1;

        void appendFreeIndices(size_t firstIndex, size_t endIndex) {
            for (size_t index = endIndex; index-- > firstIndex;) {
                freeIndices.push_back(static_cast<uint32_t>(index));
            }
        }

        void grow() {
            const size_t oldSize = pool.size();
            if (oldSize >= MaxCapacity) {
                throw std::overflow_error("ResourcePool exhausted the 20-bit handle index capacity");
            }

            size_t newSize = oldSize == 0 ? 1 : oldSize * 2;
            if (newSize > MaxCapacity) {
                newSize = MaxCapacity;
            }

            pool.resize(newSize);
            appendFreeIndices(oldSize, newSize);
        }

        uint32_t nextFreeIndex() {
            if (freeIndices.empty()) {
                grow();
            }

            return freeIndices.back();
        }

    public:
        // Pre-allocate to prevent vector resizing during gameplay
        ResourcePool(size_t reserveSize = 4096) {
            if (reserveSize > MaxCapacity) {
                throw std::invalid_argument("ResourcePool initial capacity exceeds the 20-bit handle index capacity");
            }

            pool.resize(reserveSize);
            // Populate free list backwards so we pull from index 0 first
            appendFreeIndices(0, reserveSize);
        }

        HandleType allocate(const T& data) {
            return emplace(data);
        }

        HandleType allocate(T&& data) {
            return emplace(std::move(data));
        }

        template <typename... Args>
        HandleType emplace(Args&&... args) {
            const uint32_t index = nextFreeIndex();
            Slot& slot = pool[index];

            slot.payload = T(std::forward<Args>(args)...);
            freeIndices.pop_back();
            slot.isFree = false;
            return HandleType::fromParts(index, slot.generation);
        }

        void free(HandleType handle) {
            if (!handle.isValid()) return;

            uint32_t index = handle.getIndex();
            if (index >= pool.size() || pool[index].isFree) return;

            // Safety check: Only free if the generation matches!
            if (pool[index].generation != handle.getGeneration()) return;

            Slot& slot = pool[index];
            slot.payload = T{};
            slot.isFree = true;
            slot.generation = slot.generation == HandleType::MaxGeneration ? 1 : slot.generation + 1;
            freeIndices.push_back(index);
        }

        [[nodiscard]] size_t activeCount() const noexcept {
            return pool.size() - freeIndices.size();
        }

        [[nodiscard]] size_t capacity() const noexcept {
            return pool.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return activeCount() == 0;
        }

        // Safely iterates over all active (non-freed) items in the pool
        template <typename Func>
        void forEach(Func callback) {
            for (auto& slot : pool) {
                if (!slot.isFree) {
                    callback(slot.payload);
                }
            }
        }

        template <typename Func>
        void forEachIndexed(Func callback) {
            for (uint32_t index = 0; index < pool.size(); ++index) {
                Slot& slot = pool[index];
                if (!slot.isFree)
                    callback(HandleType::fromParts(index, slot.generation), slot.payload);
            }
        }

        T* get(HandleType handle) {
            if (!handle.isValid()) return nullptr;

            uint32_t index = handle.getIndex();
            if (index >= pool.size() || pool[index].isFree) return nullptr;

            // Safety check: Detect stale pointers
            if (pool[index].generation != handle.getGeneration()) return nullptr;

            return &pool[index].payload;
        }

        template <typename Func>
        void forEach(Func callback) const {
            for (const auto& slot : pool) {
                if (!slot.isFree) {
                    callback(slot.payload);
                }
            }
        }

        const T* get(HandleType handle) const {
            if (!handle.isValid()) return nullptr;

            uint32_t index = handle.getIndex();
            if (index >= pool.size() || pool[index].isFree) return nullptr;

            if (pool[index].generation != handle.getGeneration()) return nullptr;

            return &pool[index].payload;
        }
    };

} // namespace Iridium
