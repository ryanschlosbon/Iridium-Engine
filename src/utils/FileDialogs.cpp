#include "FileDialogs.h"
#include "vendor/portable-file-dialogs.h"

std::string FileDialogs::OpenFile(const char* title, const std::vector<std::string>& filters) {
    auto selection = pfd::open_file(title, ".", filters).result();
    if (!selection.empty()) {
        return selection[0];
    }
    return std::string();
}

std::string FileDialogs::SaveFile(const char* title, const std::vector<std::string>& filters) {
    std::string defaultName = "NewFile";
    std::string ext = "";

    // Automatically extract the extension (e.g., "*.iridium" -> ".iridium")
    if (filters.size() >= 2) {
        ext = filters[1];
        if (ext.find('*') != std::string::npos) {
            ext.erase(0, ext.find('*') + 1);
        }
        defaultName += ext;
    }

    auto destination = pfd::save_file(title, defaultName, filters).result();

    // Force the extension back on if the user deleted it
    if (!destination.empty() && !ext.empty()) {
        if (destination.find(ext) == std::string::npos) {
            destination += ext;
        }
    }

    return destination;
}