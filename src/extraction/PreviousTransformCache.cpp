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
        touched_.clear();
    }

    glm::mat4 PreviousTransformCache::resolve(const Key& key, const glm::mat4& current) {
        current_.push_back({ key, current });
        const auto found = std::lower_bound(previous_.begin(), previous_.end(), key,
            [](const Entry& entry, const Key& value) { return keyLess(entry.key, value); });
        if (found != previous_.end() && keyEqual(found->key, key)) return found->world;
        return current;
    }

    void PreviousTransformCache::touch(const Key& key, const glm::mat4& current) {
        touched_.push_back({ key, current });
    }

    void PreviousTransformCache::endFrame() {
        const auto entryLess = [](const Entry& lhs, const Entry& rhs) {
            return keyLess(lhs.key, rhs.key);
        };
        const auto entryEqual = [](const Entry& lhs, const Entry& rhs) {
            return keyEqual(lhs.key, rhs.key);
        };
        std::sort(current_.begin(), current_.end(), entryLess);
        // Duplicate keys (one primitive drawn twice) keep the first entry.
        current_.erase(std::unique(current_.begin(), current_.end(), entryEqual),
            current_.end());
        if (touched_.empty()) {
            previous_.swap(current_);
            current_.clear();
            return;
        }
        // M7.10.1: the resolved entries above, unchanged, plus the touched
        // keys no packet resolved (a resolved key keeps its resolved entry).
        std::sort(touched_.begin(), touched_.end(), entryLess);
        touched_.erase(std::unique(touched_.begin(), touched_.end(), entryEqual),
            touched_.end());
        merged_.clear();
        merged_.reserve(current_.size() + touched_.size());
        auto resolved = current_.begin();
        auto touched = touched_.begin();
        while (resolved != current_.end() && touched != touched_.end()) {
            if (keyLess(touched->key, resolved->key)) {
                merged_.push_back(*touched++);
                continue;
            }
            if (!keyLess(resolved->key, touched->key)) ++touched;
            merged_.push_back(*resolved++);
        }
        merged_.insert(merged_.end(), resolved, current_.end());
        merged_.insert(merged_.end(), touched, touched_.end());
        previous_.swap(merged_);
        merged_.clear();
        current_.clear();
        touched_.clear();
    }

    void PreviousTransformCache::clear() noexcept {
        previous_.clear();
        current_.clear();
        touched_.clear();
        merged_.clear();
    }

} // namespace Iridium
