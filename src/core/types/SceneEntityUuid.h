#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Iridium {

    class SceneEntityUuid {
    public:
        using Bytes = std::array<uint8_t, 16>;

        constexpr SceneEntityUuid() noexcept = default;
        explicit constexpr SceneEntityUuid(Bytes bytes) noexcept : bytes_(bytes) {}

        [[nodiscard]] static std::optional<SceneEntityUuid> parse(
            std::string_view text) noexcept;
        [[nodiscard]] static SceneEntityUuid fromUuidV7Fields(
            uint64_t unixMilliseconds,
            std::span<const uint8_t, 10> randomBytes) noexcept;

        [[nodiscard]] std::string toString() const;
        [[nodiscard]] constexpr const Bytes& bytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] constexpr bool isNil() const noexcept {
            for (uint8_t byte : bytes_) {
                if (byte != 0) return false;
            }
            return true;
        }
        [[nodiscard]] constexpr uint8_t version() const noexcept {
            return static_cast<uint8_t>(bytes_[6] >> 4u);
        }
        [[nodiscard]] constexpr bool hasRfc4122Variant() const noexcept {
            return (bytes_[8] & 0xc0u) == 0x80u;
        }
        [[nodiscard]] constexpr bool isSupported() const noexcept {
            return !isNil() && hasRfc4122Variant() &&
                (version() == 5 || version() == 7);
        }

        auto operator<=>(const SceneEntityUuid&) const = default;

    private:
        Bytes bytes_{};
    };

    struct SceneEntityUuidHash {
        [[nodiscard]] size_t operator()(SceneEntityUuid uuid) const noexcept;
    };

    [[nodiscard]] SceneEntityUuid deriveSceneEntityUuidV5(
        std::span<const uint8_t, 16> namespaceBytes,
        std::string_view name);

    class SceneUuidGenerator {
    public:
        virtual ~SceneUuidGenerator() = default;
        [[nodiscard]] virtual SceneEntityUuid next() = 0;
    };

    class SystemSceneUuidGenerator final : public SceneUuidGenerator {
    public:
        [[nodiscard]] SceneEntityUuid next() override;
    };

} // namespace Iridium
