#pragma once
#include <vector>
#include <cstdint>

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

    public:
        // Pre-allocate to prevent vector resizing during gameplay
        ResourcePool(size_t reserveSize = 4096) {
            pool.resize(reserveSize);
            // Populate free list backwards so we pull from index 0 first
            for (int i = reserveSize - 1; i >= 0; --i) {
                freeIndices.push_back(i);
            }
        }

        HandleType allocate(const T& data) {
            if (freeIndices.empty()) {
                // Expand the pool if we run out of space
                size_t oldSize = pool.size();
                pool.resize(oldSize * 2);
                for (int i = pool.size() - 1; i >= oldSize; --i) {
                    freeIndices.push_back(i);
                }
            }

            uint32_t index = freeIndices.back();
            freeIndices.pop_back();

            pool[index].payload = data;
            pool[index].isFree = false;

            // Combine the generation and index into a single 32-bit integer
            HandleType handle;
            handle.id = (pool[index].generation << 20) | index;
            return handle;
        }

        void free(HandleType handle) {
            if (!handle.isValid()) return;

            uint32_t index = handle.getIndex();
            if (index >= pool.size() || pool[index].isFree) return;

            // Safety check: Only free if the generation matches!
            if (pool[index].generation != handle.getGeneration()) return;

            pool[index].isFree = true;
            pool[index].generation++; // Increment to invalidate old handles
            freeIndices.push_back(index);
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

        T* get(HandleType handle) {
            if (!handle.isValid()) return nullptr;

            uint32_t index = handle.getIndex();
            if (index >= pool.size() || pool[index].isFree) return nullptr;

            // Safety check: Detect stale pointers
            if (pool[index].generation != handle.getGeneration()) return nullptr;

            return &pool[index].payload;
        }
    };

} // namespace Iridium