#include "platform/FileDialog.h"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace Iridium {
    namespace {
        GLFWwindow* dialogOwner = nullptr;

#if defined(_WIN32)
        std::wstring widen(std::string_view value) {
            if (value.empty()) return {};

            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (length <= 0) return {};

            std::wstring result(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), result.data(), length);
            return result;
        }

        std::wstring buildFilter(std::span<const FileDialogFilter> filters) {
            std::wstring result;
            for (const FileDialogFilter& filter : filters) {
                const std::wstring name = widen(filter.name);
                const std::wstring pattern = widen(filter.pattern);
                result.append(name);
                result.push_back(L'\0');
                result.append(pattern);
                result.push_back(L'\0');
            }
            result.push_back(L'\0');
            return result;
        }

        HWND ownerHandle() noexcept {
            return dialogOwner ? glfwGetWin32Window(dialogOwner) : GetActiveWindow();
        }
#endif
    }

    void setFileDialogOwner(GLFWwindow* window) noexcept {
        dialogOwner = window;
    }

    std::optional<std::filesystem::path> openFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& initialDirectory) {
#if defined(_WIN32)
        std::wstring filename(32768, L'\0');
        const std::wstring filter = buildFilter(filters);
        const std::wstring initial = initialDirectory.wstring();

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = ownerHandle();
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrFilter = filter.c_str();
        dialog.nFilterIndex = 1;
        dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
            OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog)) return std::nullopt;
        return std::filesystem::path(filename.c_str());
#else
        (void)filters;
        (void)initialDirectory;
        return std::nullopt;
#endif
    }

    std::optional<std::filesystem::path> saveFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& suggestedPath,
        std::string_view defaultExtension) {
#if defined(_WIN32)
        std::wstring filename(32768, L'\0');
        const std::wstring suggested = suggestedPath.wstring();
        if (!suggested.empty()) {
            suggested.copy(filename.data(), (std::min)(suggested.size(), filename.size() - 1));
        }

        const std::wstring filter = buildFilter(filters);
        const std::wstring extension = widen(defaultExtension);

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = ownerHandle();
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrFilter = filter.c_str();
        dialog.nFilterIndex = 1;
        dialog.lpstrDefExt = extension.empty() ? nullptr : extension.c_str();
        dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT |
            OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

        if (!GetSaveFileNameW(&dialog)) return std::nullopt;
        return std::filesystem::path(filename.c_str());
#else
        (void)filters;
        (void)suggestedPath;
        (void)defaultExtension;
        return std::nullopt;
#endif
    }

} // namespace Iridium
