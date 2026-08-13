#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Iridium {

    class ComponentTypeId {
    public:
        ComponentTypeId() = default;

        [[nodiscard]] static std::optional<ComponentTypeId> parse(
            std::string_view text);
        [[nodiscard]] static bool isValid(std::string_view text) noexcept;

        [[nodiscard]] const std::string& value() const noexcept { return value_; }
        [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

        auto operator<=>(const ComponentTypeId&) const = default;

    private:
        explicit ComponentTypeId(std::string value) : value_(std::move(value)) {}
        std::string value_;
    };

    struct ComponentTypeIdHash {
        [[nodiscard]] size_t operator()(const ComponentTypeId& id) const noexcept;
    };

    class PropertyId {
    public:
        PropertyId() = default;

        [[nodiscard]] static std::optional<PropertyId> parse(
            std::string_view text);
        [[nodiscard]] static bool isValid(std::string_view text) noexcept;

        [[nodiscard]] const std::string& value() const noexcept { return value_; }
        [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

        auto operator<=>(const PropertyId&) const = default;

    private:
        explicit PropertyId(std::string value) : value_(std::move(value)) {}
        std::string value_;
    };

    struct PropertyIdHash {
        [[nodiscard]] size_t operator()(const PropertyId& id) const noexcept;
    };

    class CookedSectionId {
    public:
        CookedSectionId() = default;

        [[nodiscard]] static std::optional<CookedSectionId> parse(
            std::string_view text) noexcept;
        [[nodiscard]] std::string toString() const;
        [[nodiscard]] uint32_t value() const noexcept { return value_; }
        [[nodiscard]] bool empty() const noexcept { return value_ == 0; }

        auto operator<=>(const CookedSectionId&) const = default;

    private:
        explicit constexpr CookedSectionId(uint32_t value) noexcept : value_(value) {}
        uint32_t value_ = 0;
    };

} // namespace Iridium
