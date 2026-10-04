#include "platform/UserCacheDirectory.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <iterator>
#include <string>

namespace Iridium {

    namespace {

        std::filesystem::path environmentPath(const char* name) {
#if defined(_WIN32)
            // Wide API: %LOCALAPPDATA% may hold non-ASCII user names.
            wchar_t wideName[64]{};
            for (size_t i = 0; name[i] != '\0' && i + 1 < std::size(wideName); ++i)
                wideName[i] = static_cast<wchar_t>(name[i]);
            const DWORD length = GetEnvironmentVariableW(wideName, nullptr, 0);
            if (length == 0) return {};
            std::wstring value(length, L'\0');
            const DWORD written = GetEnvironmentVariableW(wideName, value.data(), length);
            if (written == 0 || written >= length) return {};
            value.resize(written);
            return std::filesystem::path(value);
#else
            const char* value = std::getenv(name);
            return value != nullptr && value[0] != '\0'
                ? std::filesystem::path(value) : std::filesystem::path{};
#endif
        }

    } // namespace

    std::filesystem::path userCacheDirectory(std::string_view subdirectory) {
        std::filesystem::path root;
#if defined(_WIN32)
        root = environmentPath("LOCALAPPDATA");
#else
        root = environmentPath("XDG_CACHE_HOME");
        if (root.empty()) {
            const std::filesystem::path home = environmentPath("HOME");
            if (!home.empty()) root = home / ".cache";
        }
#endif
        if (root.empty() || !root.is_absolute()) {
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error) return {};
        }
        return root / "Iridium" / std::filesystem::path(subdirectory);
    }

} // namespace Iridium
