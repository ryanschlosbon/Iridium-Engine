#pragma once

// Project asset roots: the repository's own assets/ directory ("project") plus an
// optional local asset library ("local") outside the repository that holds
// licensed third-party test content (models, HDRIs, scenes). Local content is
// never committed; it is read in place by every checkout and worktree.
//
// The local root comes from, in order:
//   1. the IRIDIUM_LOCAL_ASSET_ROOT environment variable (non-empty);
//   2. <project>/iridium.local.json   { "localAssetRoot": "D:/IridiumAssets" };
//   3. for a git worktree (<project>/.git is a "gitdir:" file), the main
//      checkout's iridium.local.json.
// Relative paths resolve against the directory of the file that named them (the
// project root for the environment variable). A configured root that is not an
// existing directory is ignored with a diagnostic. With nothing configured only
// the project root exists and every resolution is exactly <project>/assets/<path>.
//
// Lives in iridium_core so the benchmark-manifest library (which depends only on
// core) can resolve content through it.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr std::string_view kProjectAssetRootId = "project";
    inline constexpr std::string_view kLocalAssetRootId = "local";
    inline constexpr std::string_view kLocalAssetConfigFileName = "iridium.local.json";
    inline constexpr std::string_view kLocalAssetRootEnvironmentVariable =
        "IRIDIUM_LOCAL_ASSET_ROOT";

    struct ProjectAssetRootEntry {
        std::string id;
        std::filesystem::path path;

        auto operator<=>(const ProjectAssetRootEntry&) const = default;
    };

    // Where the local root came from: "environment", the config file path, or
    // empty when nothing was configured.
    struct LocalAssetRootConfig {
        std::optional<std::filesystem::path> configuredRoot;
        std::string origin;
        std::vector<std::string> diagnostics;
    };

    // Parses an iridium.local.json document. Returns the configured root
    // (resolved against baseDirectory when relative) or nullopt when the document
    // names none; malformed documents return nullopt and set *diagnostic.
    [[nodiscard]] std::optional<std::filesystem::path> parseLocalAssetRootConfig(
        std::string_view jsonText,
        const std::filesystem::path& baseDirectory,
        std::string* diagnostic = nullptr);

    // For a linked git worktree (<checkout>/.git is a "gitdir: ..." file), the
    // main checkout's directory; nullopt for a main checkout or a non-git tree.
    [[nodiscard]] std::optional<std::filesystem::path> mainCheckoutForWorktree(
        const std::filesystem::path& checkout);

    // Reads the configuration for projectRoot. environmentValue stands in for
    // IRIDIUM_LOCAL_ASSET_ROOT (nullopt or empty: unset).
    [[nodiscard]] LocalAssetRootConfig readLocalAssetRootConfig(
        const std::filesystem::path& projectRoot,
        const std::optional<std::string>& environmentValue);

    class ProjectAssetRoots {
    public:
        // The roots of projectRoot under the given configuration.
        [[nodiscard]] static ProjectAssetRoots fromProjectRoot(
            const std::filesystem::path& projectRoot,
            const std::optional<std::string>& environmentValue);
        // PROJECT_ROOT_DIR and the process environment, read once.
        [[nodiscard]] static const ProjectAssetRoots& current();

        [[nodiscard]] const std::filesystem::path& projectRoot() const noexcept {
            return projectRoot_;
        }
        [[nodiscard]] const std::filesystem::path& projectAssetRoot() const noexcept {
            return projectAssetRoot_;
        }
        // Present only when configured and an existing directory.
        [[nodiscard]] const std::optional<std::filesystem::path>&
            localAssetRoot() const noexcept {
            return localAssetRoot_;
        }
        [[nodiscard]] const LocalAssetRootConfig& configuration() const noexcept {
            return configuration_;
        }

        // "project", then "local" when present.
        [[nodiscard]] std::vector<ProjectAssetRootEntry> roots() const;
        [[nodiscard]] std::optional<std::filesystem::path> rootPath(
            std::string_view id) const;

        // A root-relative path: the project copy when it exists, else the local
        // copy when it exists, else the project path (so a missing-file error
        // names the same path as before). Absolute paths are returned unchanged.
        [[nodiscard]] std::filesystem::path resolve(
            const std::filesystem::path& rootRelative) const;
        // The root a root-relative path resolves under (same order as resolve).
        [[nodiscard]] ProjectAssetRootEntry resolveRoot(
            const std::filesystem::path& rootRelative) const;

        // For a path inside the project asset root, the same relative location
        // under the local root; nullopt without a local root or for other paths.
        [[nodiscard]] std::optional<std::filesystem::path> mapProjectPathToLocal(
            const std::filesystem::path& path) const;

    private:
        std::filesystem::path projectRoot_;
        std::filesystem::path projectAssetRoot_;
        std::optional<std::filesystem::path> localAssetRoot_;
        LocalAssetRootConfig configuration_;
    };

    // Shorthands over ProjectAssetRoots::current().
    [[nodiscard]] std::filesystem::path resolveProjectAssetPath(
        const std::filesystem::path& rootRelative);

} // namespace Iridium
