#pragma once

#include <filesystem>
#include <string_view>

namespace Iridium {

    // The per-user cache directory for `subdirectory` (M7R R4c.4), not created:
    //   Windows  %LOCALAPPDATA%/Iridium/<subdirectory>
    //   other    $XDG_CACHE_HOME/Iridium/<subdirectory>, else ~/.cache/Iridium/...
    // When none of those is set, the system temporary directory stands in
    // (<temp>/Iridium/<subdirectory>). Empty only if no temporary directory exists.
    [[nodiscard]] std::filesystem::path userCacheDirectory(std::string_view subdirectory);

} // namespace Iridium
