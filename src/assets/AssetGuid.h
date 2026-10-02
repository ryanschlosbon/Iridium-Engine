#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Iridium {

    class AssetGuid {
    public:
        using Bytes = std::array<uint8_t, 16>;

        constexpr AssetGuid() noexcept = default;
        explicit constexpr AssetGuid(Bytes bytes) noexcept : m_bytes(bytes) {}

        [[nodiscard]] static std::optional<AssetGuid> parse(std::string_view text) noexcept;
        [[nodiscard]] static AssetGuid fromUuidV7Fields(
            uint64_t unixMilliseconds, std::span<const uint8_t, 10> randomBytes) noexcept;

        [[nodiscard]] std::string toString() const;
        [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return m_bytes; }
        [[nodiscard]] constexpr bool isNil() const noexcept {
            for (const uint8_t byte : m_bytes) {
                if (byte != 0) return false;
            }
            return true;
        }
        [[nodiscard]] constexpr uint8_t version() const noexcept {
            return static_cast<uint8_t>(m_bytes[6] >> 4);
        }
        [[nodiscard]] constexpr bool hasRfc4122Variant() const noexcept {
            return (m_bytes[8] & 0xc0u) == 0x80u;
        }

        auto operator<=>(const AssetGuid&) const = default;

    private:
        Bytes m_bytes{};
    };

    struct AssetGuidHash {
        [[nodiscard]] size_t operator()(const AssetGuid& guid) const noexcept;
    };

    [[nodiscard]] AssetGuid createAssetGuidV7();
    [[nodiscard]] AssetGuid createAssetGuidV7(
        std::chrono::system_clock::time_point timestamp);

    template <typename AssetTag>
    struct AssetRef {
        AssetGuid guid;

        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return !guid.isNil();
        }

        auto operator<=>(const AssetRef&) const = default;
    };

} // namespace Iridium
