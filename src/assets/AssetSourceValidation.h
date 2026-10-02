#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace Iridium {

    inline constexpr std::string_view
        kGitLfsPointerPrefix =
            "version https://git-lfs.github.com/spec/v1";

    [[nodiscard]] inline bool isGitLfsPointer(
        std::span<const std::byte> bytes) noexcept {
        if (bytes.size() <
            kGitLfsPointerPrefix.size()) {
            return false;
        }
        for (size_t index = 0;
            index < kGitLfsPointerPrefix.size();
            ++index) {
            if (std::to_integer<unsigned char>(
                    bytes[index]) !=
                static_cast<unsigned char>(
                    kGitLfsPointerPrefix[index])) {
                return false;
            }
        }
        return true;
    }

} // namespace Iridium
