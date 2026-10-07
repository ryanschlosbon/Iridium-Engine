#include "extraction/PreviousTransformCache.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Iridium {

    namespace {
        bool keyEqual(const PreviousTransformCache::Key& lhs,
            const PreviousTransformCache::Key& rhs) noexcept {
            return std::memcmp(lhs.owner.bytes().data(), rhs.owner.bytes().data(),
                       lhs.owner.bytes().size()) == 0 &&
                std::memcmp(lhs.primitiveGuid.bytes().data(),
                    rhs.primitiveGuid.bytes().data(), lhs.primitiveGuid.bytes().size()) == 0;
        }

        // Byte order on owner identities: a strict weak order that does not
        // depend on any comparison operator the identity type provides.
        bool ownerLess(const SceneEntityUuid& lhs, const SceneEntityUuid& rhs) noexcept {
            return std::memcmp(lhs.bytes().data(), rhs.bytes().data(),
                lhs.bytes().size()) < 0;
        }

        uint64_t load64(const uint8_t* bytes) noexcept {
            uint64_t value;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        // A 64-bit mix of the two 16-byte identities. Only the table position
        // depends on it; equality always compares the full key.
        uint64_t keyHash(const PreviousTransformCache::Key& key) noexcept {
            const uint8_t* owner = key.owner.bytes().data();
            const uint8_t* primitive = key.primitiveGuid.bytes().data();
            uint64_t hash = load64(owner) * 0x9E3779B97F4A7C15ull;
            hash ^= std::rotl(load64(owner + 8) * 0xC2B2AE3D27D4EB4Full, 31);
            hash ^= std::rotl(load64(primitive) * 0x165667B19E3779F9ull, 17);
            hash ^= std::rotl(load64(primitive + 8) * 0xD6E8FEB86659FD93ull, 47);
            hash ^= hash >> 32;
            hash *= 0x9E3779B97F4A7C15ull;
            return hash ^ (hash >> 29);
        }
    }

    void PreviousTransformCache::Frame::reset(size_t expected) {
        entries.clear();
        const size_t capacity = std::max<size_t>(64, std::bit_ceil(expected * 2 + 1));
        if (table.size() < capacity) table.assign(capacity, 0u);
        else std::fill(table.begin(), table.end(), 0u);
    }

    const PreviousTransformCache::Entry* PreviousTransformCache::Frame::find(
        const Key& key) const noexcept {
        if (table.empty()) return nullptr;
        const size_t mask = table.size() - 1;
        for (size_t index = keyHash(key) & mask;; index = (index + 1) & mask) {
            const uint32_t value = table[index];
            if (value == 0u) return nullptr;
            if (keyEqual(entries[value - 1u].key, key)) return &entries[value - 1u];
        }
    }

    uint32_t& PreviousTransformCache::Frame::slot(const Key& key) {
        if ((entries.size() + 1) * 2 > table.size()) {
            // Grow (only when the packet count grows) and reindex.
            table.assign(std::max<size_t>(64, table.size() * 2), 0u);
            const size_t mask = table.size() - 1;
            for (uint32_t entry = 0; entry < entries.size(); ++entry) {
                size_t index = keyHash(entries[entry].key) & mask;
                while (table[index] != 0u) index = (index + 1) & mask;
                table[index] = entry + 1u;
            }
        }
        const size_t mask = table.size() - 1;
        for (size_t index = keyHash(key) & mask;; index = (index + 1) & mask) {
            uint32_t& value = table[index];
            if (value == 0u || keyEqual(entries[value - 1u].key, key)) return value;
        }
    }

    void PreviousTransformCache::beginFrame() noexcept {
        // Sized for last frame's population, so steady frames never grow it.
        current_.reset(previous_.entries.size());
        touchedOwners_.clear();
    }

    glm::mat4 PreviousTransformCache::resolve(const Key& key, const glm::mat4& current) {
        uint32_t& value = current_.slot(key);
        if (value == 0u) {
            current_.entries.push_back({ key, current, false });
            value = static_cast<uint32_t>(current_.entries.size());
        }
        else if (current_.entries[value - 1u].touched) {
            // A resolved entry wins over a touch of the same key.
            current_.entries[value - 1u] = { key, current, false };
        }
        // A duplicate resolve keeps the first entry.

        if (const Entry* found = previous_.find(key)) return found->world;
        // M7.10.1: a key of an owner rejected as a whole last frame.
        const auto owner = std::lower_bound(previousOwners_.begin(), previousOwners_.end(),
            key.owner, [](const OwnerEntry& entry, const SceneEntityUuid& value) {
                return ownerLess(entry.owner, value);
            });
        if (owner != previousOwners_.end() && !ownerLess(key.owner, owner->owner))
            return owner->world;
        return current;
    }

    void PreviousTransformCache::touch(const Key& key, const glm::mat4& current) {
        uint32_t& value = current_.slot(key);
        if (value != 0u) return;   // resolved, or already touched, this frame
        current_.entries.push_back({ key, current, true });
        value = static_cast<uint32_t>(current_.entries.size());
    }

    void PreviousTransformCache::touchOwner(SceneEntityUuid owner,
        const glm::mat4& current) {
        touchedOwners_.push_back({ owner, current });
    }

    void PreviousTransformCache::endFrame() {
        // Owners rejected as a whole this frame (a duplicate keeps the first).
        std::stable_sort(touchedOwners_.begin(), touchedOwners_.end(),
            [](const OwnerEntry& lhs, const OwnerEntry& rhs) {
                return ownerLess(lhs.owner, rhs.owner);
            });
        touchedOwners_.erase(std::unique(touchedOwners_.begin(), touchedOwners_.end(),
            [](const OwnerEntry& lhs, const OwnerEntry& rhs) {
                return !ownerLess(lhs.owner, rhs.owner) && !ownerLess(rhs.owner, lhs.owner);
            }), touchedOwners_.end());
        previousOwners_.swap(touchedOwners_);
        touchedOwners_.clear();
        std::swap(previous_, current_);
    }

    void PreviousTransformCache::clear() noexcept {
        previous_.entries.clear();
        std::fill(previous_.table.begin(), previous_.table.end(), 0u);
        current_.entries.clear();
        std::fill(current_.table.begin(), current_.table.end(), 0u);
        previousOwners_.clear();
        touchedOwners_.clear();
    }

} // namespace Iridium
