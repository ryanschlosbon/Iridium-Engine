#include "core/ProjectAssetRoots.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace Iridium {

    namespace {

        std::optional<std::string> readTextFile(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (!input) return std::nullopt;
            std::ostringstream text;
            text << input.rdbuf();
            return text.str();
        }

        std::string trim(std::string value) {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }

        std::optional<std::string> environmentValue(std::string_view name) {
#if defined(_WIN32)
            char* value = nullptr;
            size_t length = 0;
            if (_dupenv_s(&value, &length, std::string(name).c_str()) != 0 ||
                value == nullptr) {
                return std::nullopt;
            }
            std::string result(value);
            std::free(value);
            return result;
#else
            const char* value = std::getenv(std::string(name).c_str());
            if (value == nullptr) return std::nullopt;
            return std::string(value);
#endif
        }

        std::filesystem::path normalizedAbsolute(const std::filesystem::path& path,
            const std::filesystem::path& base) {
            std::filesystem::path result =
                (path.is_absolute() ? path : base / path).lexically_normal();
            // "a/b/.." normalizes to "a/" (empty filename); drop the separator.
            if (result.has_relative_path() && result.filename().empty()) {
                result = result.parent_path();
            }
            return result;
        }

        // `path` relative to `root` when it lies inside it (both canonicalized).
        std::optional<std::filesystem::path> relativeInside(
            const std::filesystem::path& path, const std::filesystem::path& root) {
            std::error_code error;
            const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
            if (error) return std::nullopt;
            const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
            if (error) return std::nullopt;
            const auto relative = canonicalPath.lexically_relative(canonicalRoot);
            if (relative.empty()) return std::nullopt;
            const auto first = relative.begin();
            if (first != relative.end() && *first == "..") return std::nullopt;
            return relative;
        }

        // One config source: the root it names, or nullopt (diagnostics appended).
        std::optional<std::filesystem::path> readConfigFile(
            const std::filesystem::path& file, LocalAssetRootConfig& config,
            bool& present) {
            std::error_code error;
            present = std::filesystem::is_regular_file(file, error);
            if (!present) return std::nullopt;
            const std::optional<std::string> text = readTextFile(file);
            if (!text) {
                config.diagnostics.push_back("Could not read " + file.generic_string());
                return std::nullopt;
            }
            std::string diagnostic;
            auto root = parseLocalAssetRootConfig(*text, file.parent_path(), &diagnostic);
            if (!diagnostic.empty()) {
                config.diagnostics.push_back(file.generic_string() + ": " + diagnostic);
            }
            return root;
        }

    } // namespace

    std::optional<std::filesystem::path> parseLocalAssetRootConfig(
        std::string_view jsonText, const std::filesystem::path& baseDirectory,
        std::string* diagnostic) {
        const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            if (diagnostic) *diagnostic = "not a JSON object";
            return std::nullopt;
        }
        const auto found = document.find("localAssetRoot");
        if (found == document.end() || found->is_null()) return std::nullopt;
        if (!found->is_string()) {
            if (diagnostic) *diagnostic = "localAssetRoot must be a string";
            return std::nullopt;
        }
        const std::string value = trim(found->get<std::string>());
        if (value.empty()) return std::nullopt;
        return normalizedAbsolute(std::filesystem::path(value), baseDirectory);
    }

    std::optional<std::filesystem::path> mainCheckoutForWorktree(
        const std::filesystem::path& checkout) {
        const std::filesystem::path dotGit = checkout / ".git";
        std::error_code error;
        if (!std::filesystem::is_regular_file(dotGit, error)) return std::nullopt;
        const std::optional<std::string> text = readTextFile(dotGit);
        if (!text) return std::nullopt;
        std::string line = trim(*text);
        constexpr std::string_view prefix = "gitdir:";
        if (!line.starts_with(prefix)) return std::nullopt;
        const std::filesystem::path gitDir = normalizedAbsolute(
            std::filesystem::path(trim(line.substr(prefix.size()))), checkout);

        // <main>/.git/worktrees/<name>; "commondir" names <main>/.git relative to it.
        std::filesystem::path commonDir;
        if (const auto common = readTextFile(gitDir / "commondir")) {
            commonDir = normalizedAbsolute(std::filesystem::path(trim(*common)), gitDir);
        }
        else if (gitDir.parent_path().filename() == "worktrees") {
            commonDir = gitDir.parent_path().parent_path();
        }
        else {
            return std::nullopt;
        }
        // A bare repository has no main checkout.
        if (commonDir.filename() != ".git") return std::nullopt;
        std::filesystem::path main = commonDir.parent_path();
        if (main.empty()) return std::nullopt;
        if (!std::filesystem::is_directory(main, error)) return std::nullopt;
        return main;
    }

    LocalAssetRootConfig readLocalAssetRootConfig(
        const std::filesystem::path& projectRoot,
        const std::optional<std::string>& environment) {
        LocalAssetRootConfig config;
        if (environment) {
            const std::string value = trim(*environment);
            if (!value.empty()) {
                config.configuredRoot = normalizedAbsolute(
                    std::filesystem::path(value), projectRoot);
                config.origin = "environment";
                return config;
            }
        }
        bool present = false;
        const std::filesystem::path own =
            projectRoot / std::string(kLocalAssetConfigFileName);
        config.configuredRoot = readConfigFile(own, config, present);
        if (present) {
            // The checkout's own file decides, even when it names no root.
            config.origin = own.lexically_normal().generic_string();
            return config;
        }
        if (const auto main = mainCheckoutForWorktree(projectRoot)) {
            const std::filesystem::path shared =
                *main / std::string(kLocalAssetConfigFileName);
            config.configuredRoot = readConfigFile(shared, config, present);
            if (present) config.origin = shared.lexically_normal().generic_string();
        }
        if (!config.configuredRoot) config.origin.clear();
        return config;
    }

    ProjectAssetRoots ProjectAssetRoots::fromProjectRoot(
        const std::filesystem::path& projectRoot,
        const std::optional<std::string>& environment) {
        ProjectAssetRoots roots;
        roots.projectRoot_ = projectRoot;
        roots.projectAssetRoot_ = projectRoot / "assets";
        roots.configuration_ = readLocalAssetRootConfig(projectRoot, environment);
        if (const auto& configured = roots.configuration_.configuredRoot) {
            std::error_code error;
            if (std::filesystem::is_directory(*configured, error)) {
                roots.localAssetRoot_ = *configured;
            }
            else {
                roots.configuration_.diagnostics.push_back(
                    "Local asset root " + configured->generic_string() + " (from " +
                    roots.configuration_.origin +
                    ") is not an existing directory; using the project root only.");
            }
        }
        return roots;
    }

    const ProjectAssetRoots& ProjectAssetRoots::current() {
        static const ProjectAssetRoots roots = fromProjectRoot(
            std::filesystem::path(PROJECT_ROOT_DIR),
            environmentValue(kLocalAssetRootEnvironmentVariable));
        return roots;
    }

    std::vector<ProjectAssetRootEntry> ProjectAssetRoots::roots() const {
        std::vector<ProjectAssetRootEntry> result{
            { std::string(kProjectAssetRootId), projectAssetRoot_ },
        };
        if (localAssetRoot_) {
            result.push_back({ std::string(kLocalAssetRootId), *localAssetRoot_ });
        }
        return result;
    }

    std::optional<std::filesystem::path> ProjectAssetRoots::rootPath(
        std::string_view id) const {
        if (id == kProjectAssetRootId) return projectAssetRoot_;
        if (id == kLocalAssetRootId && localAssetRoot_) return *localAssetRoot_;
        return std::nullopt;
    }

    ProjectAssetRootEntry ProjectAssetRoots::resolveRoot(
        const std::filesystem::path& rootRelative) const {
        ProjectAssetRootEntry project{ std::string(kProjectAssetRootId), projectAssetRoot_ };
        if (rootRelative.is_absolute() || !localAssetRoot_) return project;
        std::error_code error;
        if (std::filesystem::exists(projectAssetRoot_ / rootRelative, error)) return project;
        error.clear();
        if (std::filesystem::exists(*localAssetRoot_ / rootRelative, error)) {
            return { std::string(kLocalAssetRootId), *localAssetRoot_ };
        }
        return project;
    }

    std::filesystem::path ProjectAssetRoots::resolve(
        const std::filesystem::path& rootRelative) const {
        if (rootRelative.is_absolute()) return rootRelative;
        return resolveRoot(rootRelative).path / rootRelative;
    }

    std::optional<std::filesystem::path> ProjectAssetRoots::mapProjectPathToLocal(
        const std::filesystem::path& path) const {
        if (!localAssetRoot_) return std::nullopt;
        std::error_code error;
        const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
        if (error) return std::nullopt;
        const auto canonicalRoot = std::filesystem::weakly_canonical(projectAssetRoot_, error);
        if (error) return std::nullopt;
        if (canonicalPath == canonicalRoot) return *localAssetRoot_;
        const auto relative = relativeInside(canonicalPath, canonicalRoot);
        if (!relative) return std::nullopt;
        return (*localAssetRoot_ / *relative).lexically_normal();
    }

    std::filesystem::path resolveProjectAssetPath(
        const std::filesystem::path& rootRelative) {
        return ProjectAssetRoots::current().resolve(rootRelative);
    }

} // namespace Iridium
