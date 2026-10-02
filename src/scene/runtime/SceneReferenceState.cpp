#include "scene/runtime/SceneReferenceState.h"

#include <functional>

namespace Iridium {

    size_t SceneReferenceKeyHash::operator()(
        const SceneReferenceKey& key) const noexcept {
        size_t value = SceneEntityUuidHash{}(key.owner);
        const auto combine = [&value](size_t next) {
            value ^= next + 0x9e3779b97f4a7c15ull +
                (value << 6u) + (value >> 2u);
        };
        combine(ComponentTypeIdHash{}(key.component));
        combine(std::hash<std::string_view>{}(key.propertyPath));
        return value;
    }

    bool SceneReferenceState::add(SceneReferenceRecord record) {
        if (record.key.owner.isNil() || record.key.component.empty() ||
            record.key.propertyPath.empty() || indices_.contains(record.key)) {
            return false;
        }
        const size_t index = records_.size();
        records_.push_back(std::move(record));
        indices_.emplace(records_.back().key, index);
        return true;
    }

    SceneReferenceRecord* SceneReferenceState::find(
        const SceneReferenceKey& key) noexcept {
        const auto found = indices_.find(key);
        return found == indices_.end() ? nullptr : &records_[found->second];
    }

    const SceneReferenceRecord* SceneReferenceState::find(
        const SceneReferenceKey& key) const noexcept {
        const auto found = indices_.find(key);
        return found == indices_.end() ? nullptr : &records_[found->second];
    }

    std::span<const SceneReferenceRecord>
    SceneReferenceState::records() const noexcept {
        return records_;
    }

    std::span<SceneReferenceRecord> SceneReferenceState::records() noexcept {
        return records_;
    }

    void SceneReferenceState::clear() noexcept {
        records_.clear();
        indices_.clear();
    }

    void SceneReferenceState::swap(SceneReferenceState& other) noexcept {
        records_.swap(other.records_);
        indices_.swap(other.indices_);
    }

} // namespace Iridium
