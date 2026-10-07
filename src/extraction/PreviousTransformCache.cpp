#include "extraction/PreviousTransformCache.h"

#include <algorithm>
#include <cstring>

namespace Iridium {

    namespace {
        // Byte order on the 16-byte identities: a strict weak order that does
        // not depend on any comparison operator the identity types provide.
        bool keyLess(const PreviousTransformCache::Key& lhs,
            const PreviousTransformCache::Key& rhs) noexcept {
            const int owner = std::memcmp(lhs.owner.bytes().data(),
                rhs.owner.bytes().data(), lhs.owner.bytes().size());
            if (owner != 0) return owner < 0;
            return std::memcmp(lhs.primitiveGuid.bytes().data(),
                rhs.primitiveGuid.bytes().data(), lhs.primitiveGuid.bytes().size()) < 0;
        }
        bool keyEqual(const PreviousTransformCache::Key& lhs,
            const PreviousTransformCache::Key& rhs) noexcept {
            return !keyLess(lhs, rhs) && !keyLess(rhs, lhs);
        }
    }

    void PreviousTransformCache::beginFrame() noexcept {
        current_.clear();
    }

    glm::mat4 PreviousTransformCache::resolve(const Key& key, const glm::mat4& current) {
        current_.push_back({ key, current });
        const auto found = std::lower_bound(previous_.begin(), previous_.end(), key,
            [](const Entry& entry, const Key& value) { return keyLess(entry.key, value); });
        if (found != previous_.end() && keyEqual(found->key, key)) return found->world;
        return current;
    }

    void PreviousTransformCache::endFrame() {
        std::sort(current_.begin(), current_.end(),
            [](const Entry& lhs, const Entry& rhs) { return keyLess(lhs.key, rhs.key); });
        // Duplicate keys (one primitive drawn twice) keep the first entry.
        current_.erase(std::unique(current_.begin(), current_.end(),
            [](const Entry& lhs, const Entry& rhs) { return keyEqual(lhs.key, rhs.key); }),
            current_.end());
        previous_.swap(current_);
        current_.clear();
    }

    void PreviousTransformCache::clear() noexcept {
        previous_.clear();
        current_.clear();
    }

} // namespace Iridium
