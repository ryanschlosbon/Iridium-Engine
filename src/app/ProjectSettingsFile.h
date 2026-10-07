#pragma once

#include "app/ApplicationConfig.h"

#include <filesystem>
#include <string>

namespace Iridium {

    // M9.8c: the Project Settings window persisted per project
    // (`<repo>/project.settings.json`). Interactive runs load it before the
    // command line (so flags still win) and the editor saves it after edits;
    // benchmark and capture runs never read it, so measurement routes keep
    // their pinned configuration. The display transport stays per machine.
    //
    // Loading is tolerant: missing or unknown fields keep their current
    // values, a malformed file is reported and ignored.
    [[nodiscard]] std::filesystem::path defaultProjectSettingsPath();

    // Applies the file's values to `config`. Returns false (and a diagnostic)
    // when the file exists but cannot be read; a missing file is not an error.
    bool loadProjectSettings(const std::filesystem::path& path, ApplicationConfig& config,
        std::string& diagnostic);

    // Writes the persisted subset of `config` atomically (temporary file, then
    // rename). Returns false with a diagnostic on failure.
    bool saveProjectSettings(const std::filesystem::path& path, const ApplicationConfig& config,
        std::string& diagnostic);

} // namespace Iridium
