#include "scene/runtime/ComponentIdentity.h"

#include <functional>

namespace Iridium {
    namespace {

        [[nodiscard]] bool isLowerAscii(char value) noexcept {
            return value >= 'a' && value <= 'z';
        }

        [[nodiscard]] bool isDigitAscii(char value) noexcept {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]] bool isSectionCharacter(char value) noexcept {
            return (value >= 'A' && value <= 'Z') || isDigitAscii(value);
        }

    } // namespace

    bool ComponentTypeId::isValid(std::string_view text) noexcept {
        if (text.empty()) return false;

        size_t segmentStart = 0;
        size_t segmentCount = 0;
        while (segmentStart < text.size()) {
            const size_t segmentEnd = text.find('.', segmentStart);
            const size_t end = segmentEnd == std::string_view::npos
                ? text.size()
                : segmentEnd;
            if (end == segmentStart || !isLowerAscii(text[segmentStart])) {
                return false;
            }
            for (size_t index = segmentStart + 1; index < end; ++index) {
                const char value = text[index];
                if (!isLowerAscii(value) && !isDigitAscii(value) &&
                    (segmentCount == 0 || (value != '_' && value != '-'))) {
                    return false;
                }
            }
            ++segmentCount;
            if (segmentEnd == std::string_view::npos) break;
            segmentStart = segmentEnd + 1;
        }
        return segmentCount >= 2 && text.back() != '.';
    }

    std::optional<ComponentTypeId> ComponentTypeId::parse(
        std::string_view text) {
        if (!isValid(text)) return std::nullopt;
        return ComponentTypeId(std::string(text));
    }

    size_t ComponentTypeIdHash::operator()(
        const ComponentTypeId& id) const noexcept {
        return std::hash<std::string_view>{}(id.value());
    }

    bool PropertyId::isValid(std::string_view text) noexcept {
        if (text.empty() || !isLowerAscii(text.front())) return false;
        for (char value : text.substr(1)) {
            if (!isLowerAscii(value) && !isDigitAscii(value) && value != '_') {
                return false;
            }
        }
        return true;
    }

    std::optional<PropertyId> PropertyId::parse(std::string_view text) {
        if (!isValid(text)) return std::nullopt;
        return PropertyId(std::string(text));
    }

    size_t PropertyIdHash::operator()(const PropertyId& id) const noexcept {
        return std::hash<std::string_view>{}(id.value());
    }

    std::optional<CookedSectionId> CookedSectionId::parse(
        std::string_view text) noexcept {
        if (text.size() != 4) return std::nullopt;
        uint32_t value = 0;
        for (size_t index = 0; index < text.size(); ++index) {
            if (!isSectionCharacter(text[index])) return std::nullopt;
            value |= static_cast<uint32_t>(
                static_cast<unsigned char>(text[index])) << (index * 8u);
        }
        return CookedSectionId(value);
    }

    std::string CookedSectionId::toString() const {
        if (empty()) return {};
        std::string result(4, '\0');
        for (size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<char>(value_ >> (index * 8u));
        }
        return result;
    }

} // namespace Iridium
