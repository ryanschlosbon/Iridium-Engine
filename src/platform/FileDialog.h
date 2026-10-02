#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

struct GLFWwindow;

namespace Iridium {

    struct FileDialogFilter {
        std::string_view name;
        std::string_view pattern;
    };

    // The editor owns the GLFW window; the dialog service only borrows it while
    // displaying a modal native dialog.
    void setFileDialogOwner(GLFWwindow* window) noexcept;

    std::optional<std::filesystem::path> openFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& initialDirectory = {});

    std::optional<std::filesystem::path> saveFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& suggestedPath = {},
        std::string_view defaultExtension = {});

} // namespace Iridium
