#pragma once

#include "scene/SceneEntityUuid.h"
#include "scene/runtime/ComponentIdentity.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Iridium {

    enum class StableReferenceKind {
        Entity,
        Asset,
        Subasset,
    };

    enum class StableReferenceResolution {
        Unresolved,
        Resolved,
        Pending,
        Failed,
    };

    struct SceneReferenceKey {
        SceneEntityUuid owner;
        ComponentTypeId component;
        std::string propertyPath;

        bool operator==(const SceneReferenceKey&) const = default;
    };

    struct SceneReferenceKeyHash {
        [[nodiscard]] size_t operator()(const SceneReferenceKey& key) const noexcept;
    };

    struct SceneReferenceRecord {
        SceneReferenceKey key;
        StableReferenceKind kind = StableReferenceKind::Entity;
        std::array<uint8_t, 16> target{};
        bool required = false;
        StableReferenceResolution resolution =
            StableReferenceResolution::Unresolved;
    };

    class SceneReferenceState {
    public:
        [[nodiscard]] bool add(SceneReferenceRecord record);
        [[nodiscard]] SceneReferenceRecord* find(
            const SceneReferenceKey& key) noexcept;
        [[nodiscard]] const SceneReferenceRecord* find(
            const SceneReferenceKey& key) const noexcept;
        [[nodiscard]] std::span<const SceneReferenceRecord> records() const noexcept;
        [[nodiscard]] std::span<SceneReferenceRecord> records() noexcept;
        void clear() noexcept;
        void swap(SceneReferenceState& other) noexcept;

    private:
        std::vector<SceneReferenceRecord> records_;
        std::unordered_map<SceneReferenceKey, size_t, SceneReferenceKeyHash> indices_;
    };

} // namespace Iridium
