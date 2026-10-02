#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace Iridium {

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
    [[nodiscard]] std::string sha256File(const std::filesystem::path& path);

} // namespace Iridium
