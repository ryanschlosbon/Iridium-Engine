#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

class EntityHandle {
public:
    static constexpr uint32_t NullPart =
        (std::numeric_limits<uint32_t>::max)();

    constexpr EntityHandle() noexcept = default;

    [[nodiscard]] static constexpr EntityHandle fromParts(
        uint32_t index, uint32_t generation) noexcept {
        return EntityHandle(index, generation);
    }

    // Explicit adapter for the logical-v0 serializer and frozen fixtures only.
    [[nodiscard]] static constexpr EntityHandle fromLegacyIndex(
        uint32_t index) noexcept {
        return EntityHandle(index, 1);
    }

    [[nodiscard]] constexpr uint32_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr uint32_t generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] constexpr uint64_t packed() const noexcept {
        return static_cast<uint64_t>(generation_) << 32u |
            static_cast<uint64_t>(index_);
    }
    [[nodiscard]] constexpr bool isNull() const noexcept {
        return index_ == NullPart && generation_ == NullPart;
    }

    auto operator<=>(const EntityHandle&) const = default;

private:
    constexpr EntityHandle(uint32_t index, uint32_t generation) noexcept
        : index_(index), generation_(generation) {}

    uint32_t index_ = NullPart;
    uint32_t generation_ = NullPart;
};

using Entity = EntityHandle;
inline constexpr Entity NULL_ENTITY{};

template<>
struct std::hash<EntityHandle> {
    [[nodiscard]] size_t operator()(EntityHandle entity) const noexcept {
        return std::hash<uint64_t>{}(entity.packed());
    }
};
