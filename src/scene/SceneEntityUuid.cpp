#include "scene/SceneEntityUuid.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <functional>
#include <random>
#include <vector>

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

        std::array<uint8_t, 20> sha1(std::span<const uint8_t> input) {
            std::vector<uint8_t> message(input.begin(), input.end());
            const uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8u;
            message.push_back(0x80u);
            while ((message.size() % 64u) != 56u) message.push_back(0u);
            for (int shift = 56; shift >= 0; shift -= 8) {
                message.push_back(static_cast<uint8_t>(bitLength >> shift));
            }

            uint32_t h0 = 0x67452301u;
            uint32_t h1 = 0xefcdab89u;
            uint32_t h2 = 0x98badcfeu;
            uint32_t h3 = 0x10325476u;
            uint32_t h4 = 0xc3d2e1f0u;
            for (size_t offset = 0; offset < message.size(); offset += 64u) {
                std::array<uint32_t, 80> words{};
                for (size_t index = 0; index < 16; ++index) {
                    const size_t source = offset + index * 4u;
                    words[index] = static_cast<uint32_t>(message[source]) << 24u |
                        static_cast<uint32_t>(message[source + 1]) << 16u |
                        static_cast<uint32_t>(message[source + 2]) << 8u |
                        static_cast<uint32_t>(message[source + 3]);
                }
                for (size_t index = 16; index < words.size(); ++index) {
                    words[index] = std::rotl(words[index - 3] ^ words[index - 8] ^
                        words[index - 14] ^ words[index - 16], 1);
                }

                uint32_t a = h0;
                uint32_t b = h1;
                uint32_t c = h2;
                uint32_t d = h3;
                uint32_t e = h4;
                for (size_t index = 0; index < words.size(); ++index) {
                    uint32_t function = 0;
                    uint32_t constant = 0;
                    if (index < 20) {
                        function = (b & c) | ((~b) & d);
                        constant = 0x5a827999u;
                    }
                    else if (index < 40) {
                        function = b ^ c ^ d;
                        constant = 0x6ed9eba1u;
                    }
                    else if (index < 60) {
                        function = (b & c) | (b & d) | (c & d);
                        constant = 0x8f1bbcdcu;
                    }
                    else {
                        function = b ^ c ^ d;
                        constant = 0xca62c1d6u;
                    }
                    const uint32_t temporary = std::rotl(a, 5) + function + e +
                        constant + words[index];
                    e = d;
                    d = c;
                    c = std::rotl(b, 30);
                    b = a;
                    a = temporary;
                }
                h0 += a;
                h1 += b;
                h2 += c;
                h3 += d;
                h4 += e;
            }

            const std::array<uint32_t, 5> hashes{ h0, h1, h2, h3, h4 };
            std::array<uint8_t, 20> result{};
            for (size_t word = 0; word < hashes.size(); ++word) {
                for (size_t byte = 0; byte < 4; ++byte) {
                    result[word * 4 + byte] = static_cast<uint8_t>(
                        hashes[word] >> ((3 - byte) * 8u));
                }
            }
            return result;
        }

    } // namespace

    std::optional<SceneEntityUuid> SceneEntityUuid::parse(
        std::string_view text) noexcept {
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
            if (index + 1 >= text.size() || byteIndex >= bytes.size()) {
                return std::nullopt;
            }
            const int high = hexValue(text[index]);
            const int low = hexValue(text[index + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            bytes[byteIndex++] = static_cast<uint8_t>((high << 4) | low);
            index += 2;
        }
        if (byteIndex != bytes.size()) return std::nullopt;

        SceneEntityUuid result(bytes);
        return result.isSupported()
            ? std::optional<SceneEntityUuid>(result)
            : std::nullopt;
    }

    SceneEntityUuid SceneEntityUuid::fromUuidV7Fields(
        uint64_t unixMilliseconds,
        std::span<const uint8_t, 10> randomBytes) noexcept {
        Bytes bytes{};
        const uint64_t timestamp = unixMilliseconds & 0x0000ffffffffffffull;
        for (size_t index = 0; index < 6; ++index) {
            bytes[index] = static_cast<uint8_t>(
                timestamp >> ((5 - index) * 8u));
        }
        bytes[6] = static_cast<uint8_t>(0x70u | (randomBytes[0] & 0x0fu));
        bytes[7] = randomBytes[1];
        bytes[8] = static_cast<uint8_t>(0x80u | (randomBytes[2] & 0x3fu));
        std::copy(randomBytes.begin() + 3, randomBytes.end(), bytes.begin() + 9);
        return SceneEntityUuid(bytes);
    }

    std::string SceneEntityUuid::toString() const {
        std::string result(36, '-');
        size_t outputIndex = 0;
        for (uint8_t byte : bytes_) {
            if (outputIndex == 8 || outputIndex == 13 ||
                outputIndex == 18 || outputIndex == 23) {
                ++outputIndex;
            }
            result[outputIndex++] = kHex[byte >> 4u];
            result[outputIndex++] = kHex[byte & 0x0fu];
        }
        return result;
    }

    size_t SceneEntityUuidHash::operator()(SceneEntityUuid uuid) const noexcept {
        uint64_t high = 0;
        uint64_t low = 0;
        for (size_t index = 0; index < 8; ++index) {
            high = (high << 8u) | uuid.bytes()[index];
            low = (low << 8u) | uuid.bytes()[index + 8];
        }
        const size_t first = std::hash<uint64_t>{}(high);
        const size_t second = std::hash<uint64_t>{}(low);
        return first ^ (second + 0x9e3779b97f4a7c15ull +
            (first << 6u) + (first >> 2u));
    }

    SceneEntityUuid deriveSceneEntityUuidV5(
        std::span<const uint8_t, 16> namespaceBytes,
        std::string_view name) {
        std::vector<uint8_t> input;
        input.reserve(namespaceBytes.size() + name.size());
        input.insert(input.end(), namespaceBytes.begin(), namespaceBytes.end());
        input.insert(input.end(), name.begin(), name.end());
        const std::array<uint8_t, 20> hash = sha1(input);
        SceneEntityUuid::Bytes bytes{};
        std::copy_n(hash.begin(), bytes.size(), bytes.begin());
        bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x50u);
        bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
        return SceneEntityUuid(bytes);
    }

    SceneEntityUuid SystemSceneUuidGenerator::next() {
        std::array<uint8_t, 10> randomBytes{};
#if defined(_WIN32)
        const NTSTATUS randomStatus = BCryptGenRandom(nullptr,
            randomBytes.data(), static_cast<ULONG>(randomBytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (randomStatus < 0)
#endif
        {
            std::random_device source;
            for (uint8_t& byte : randomBytes) {
                byte = static_cast<uint8_t>(source());
            }
        }
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        return SceneEntityUuid::fromUuidV7Fields(
            static_cast<uint64_t>(milliseconds), randomBytes);
    }

} // namespace Iridium
