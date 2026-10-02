#include "assets/AssetGuid.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace Iridium {

    namespace {

        constexpr char kHex[] = "0123456789abcdef";

        int hexValue(char value) noexcept {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

    } // namespace

    std::optional<AssetGuid> AssetGuid::parse(std::string_view text) noexcept {
        if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
            text[18] != '-' || text[23] != '-') {
            return std::nullopt;
        }

        Bytes bytes{};
        size_t byteIndex = 0;
        for (size_t index = 0; index < text.size();) {
            if (text[index] == '-') {
                ++index;
                continue;
            }
            if (index + 1 >= text.size() || byteIndex >= bytes.size()) return std::nullopt;
            const int high = hexValue(text[index]);
            const int low = hexValue(text[index + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            bytes[byteIndex++] = static_cast<uint8_t>((high << 4) | low);
            index += 2;
        }
        if (byteIndex != bytes.size()) return std::nullopt;
        return AssetGuid(bytes);
    }

    AssetGuid AssetGuid::fromUuidV7Fields(
        uint64_t unixMilliseconds, std::span<const uint8_t, 10> randomBytes) noexcept {
        Bytes bytes{};
        const uint64_t timestamp = unixMilliseconds & 0x0000ffffffffffffull;
        for (size_t index = 0; index < 6; ++index) {
            bytes[index] = static_cast<uint8_t>(timestamp >> ((5 - index) * 8));
        }
        bytes[6] = static_cast<uint8_t>(0x70u | (randomBytes[0] & 0x0fu));
        bytes[7] = randomBytes[1];
        bytes[8] = static_cast<uint8_t>(0x80u | (randomBytes[2] & 0x3fu));
        std::copy(randomBytes.begin() + 3, randomBytes.end(), bytes.begin() + 9);
        return AssetGuid(bytes);
    }

    std::string AssetGuid::toString() const {
        std::string result(36, '-');
        size_t outputIndex = 0;
        for (size_t byteIndex = 0; byteIndex < m_bytes.size(); ++byteIndex) {
            if (outputIndex == 8 || outputIndex == 13 ||
                outputIndex == 18 || outputIndex == 23) {
                ++outputIndex;
            }
            result[outputIndex++] = kHex[m_bytes[byteIndex] >> 4];
            result[outputIndex++] = kHex[m_bytes[byteIndex] & 0x0f];
        }
        return result;
    }

    size_t AssetGuidHash::operator()(const AssetGuid& guid) const noexcept {
        uint64_t high = 0;
        uint64_t low = 0;
        for (size_t index = 0; index < 8; ++index) {
            high = (high << 8) | guid.bytes()[index];
            low = (low << 8) | guid.bytes()[index + 8];
        }
        const size_t first = std::hash<uint64_t>{}(high);
        const size_t second = std::hash<uint64_t>{}(low);
        return first ^ (second + 0x9e3779b97f4a7c15ull + (first << 6) + (first >> 2));
    }

    AssetGuid createAssetGuidV7() {
        return createAssetGuidV7(std::chrono::system_clock::now());
    }

    AssetGuid createAssetGuidV7(std::chrono::system_clock::time_point timestamp) {
        std::array<uint8_t, 10> randomBytes{};
#if defined(_WIN32)
        const NTSTATUS randomStatus = BCryptGenRandom(nullptr, randomBytes.data(),
            static_cast<ULONG>(randomBytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (randomStatus < 0)
#endif
        {
            std::random_device source;
            for (uint8_t& byte : randomBytes) {
                byte = static_cast<uint8_t>(source());
            }
        }
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count();
        return AssetGuid::fromUuidV7Fields(
            static_cast<uint64_t>(milliseconds), randomBytes);
    }

} // namespace Iridium
