// Unit tests for Iridium::ProjectAssetRoots (iridium_core): iridium.local.json
// parsing, the worktree fallback to the main checkout, the environment override,
// absence meaning the project root only, and resolution order.
#include "core/ProjectAssetRoots.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    struct TemporaryDirectory {
        TemporaryDirectory() {
            static std::atomic<uint32_t> counter{ 0 };
            path = std::filesystem::temp_directory_path() /
                ("iridium-asset-roots-" +
                    std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch().count()) +
                    "-" + std::to_string(counter++));
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            // Only paths this test created; no links are ever made here.
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
        std::filesystem::path path;
    };

    void writeText(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    std::string jsonPath(const std::filesystem::path& path) {
        return path.generic_string();
    }

    bool same(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
        return std::filesystem::weakly_canonical(lhs) ==
            std::filesystem::weakly_canonical(rhs);
    }

    bool testParse() {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "iridium-base";
        std::string diagnostic;
        auto root = parseLocalAssetRootConfig(
            R"({ "localAssetRoot": "D:/IridiumAssets" })", base, &diagnostic);
        CHECK(root && diagnostic.empty());
        CHECK(root->generic_string() == "D:/IridiumAssets");

        root = parseLocalAssetRootConfig(R"({ "localAssetRoot": "../library" })",
            base, &diagnostic);
        CHECK(root && *root == (base / ".." / "library").lexically_normal());

        diagnostic.clear();
        CHECK(!parseLocalAssetRootConfig(R"({ "other": 1 })", base, &diagnostic));
        CHECK(diagnostic.empty());
        CHECK(!parseLocalAssetRootConfig(R"({ "localAssetRoot": "  " })", base, &diagnostic));
        CHECK(!parseLocalAssetRootConfig(R"({ "localAssetRoot": null })", base, &diagnostic));
        CHECK(diagnostic.empty());

        CHECK(!parseLocalAssetRootConfig("{ not json", base, &diagnostic));
        CHECK(!diagnostic.empty());
        diagnostic.clear();
        CHECK(!parseLocalAssetRootConfig(R"({ "localAssetRoot": 4 })", base, &diagnostic));
        CHECK(!diagnostic.empty());
        diagnostic.clear();
        CHECK(!parseLocalAssetRootConfig("[]", base, &diagnostic));
        CHECK(!diagnostic.empty());
        return true;
    }

    bool testAbsenceIsProjectOnly() {
        TemporaryDirectory project;
        std::filesystem::create_directories(project.path / "assets" / "models");
        writeText(project.path / "assets" / "models" / "a.gltf", "{}");
        const ProjectAssetRoots roots =
            ProjectAssetRoots::fromProjectRoot(project.path, std::nullopt);
        CHECK(!roots.configuration().configuredRoot);
        CHECK(roots.configuration().origin.empty());
        CHECK(roots.configuration().diagnostics.empty());
        CHECK(!roots.localAssetRoot());
        const auto list = roots.roots();
        CHECK(list.size() == 1);
        CHECK(list[0].id == "project");
        CHECK(list[0].path == project.path / "assets");
        CHECK(roots.resolve("models/a.gltf") == project.path / "assets" / "models/a.gltf");
        CHECK(roots.resolve("models/missing.gltf") ==
            project.path / "assets" / "models/missing.gltf");
        CHECK(roots.resolveRoot("models/missing.gltf").id == "project");
        CHECK(!roots.rootPath("local"));
        CHECK(!roots.mapProjectPathToLocal(project.path / "assets" / "models"));

        // An empty environment value is "unset".
        const ProjectAssetRoots emptyEnvironment =
            ProjectAssetRoots::fromProjectRoot(project.path, std::string("  "));
        CHECK(!emptyEnvironment.localAssetRoot());
        CHECK(emptyEnvironment.roots().size() == 1);
        return true;
    }

    bool testOwnConfigFile() {
        TemporaryDirectory project;
        TemporaryDirectory library;
        std::filesystem::create_directories(project.path / "assets");
        writeText(project.path / "iridium.local.json",
            "{ \"localAssetRoot\": \"" + jsonPath(library.path) + "\" }");
        const ProjectAssetRoots roots =
            ProjectAssetRoots::fromProjectRoot(project.path, std::nullopt);
        CHECK(roots.localAssetRoot());
        CHECK(same(*roots.localAssetRoot(), library.path));
        CHECK(roots.configuration().origin.ends_with("iridium.local.json"));
        const auto list = roots.roots();
        CHECK(list.size() == 2);
        CHECK(list[0].id == "project");
        CHECK(list[1].id == "local");
        CHECK(roots.rootPath("local") && same(*roots.rootPath("local"), library.path));
        CHECK(!roots.rootPath("other"));
        return true;
    }

    bool testConfiguredButMissing() {
        TemporaryDirectory project;
        std::filesystem::create_directories(project.path / "assets");
        const std::filesystem::path missing = project.path / "no-such-library";
        writeText(project.path / "iridium.local.json",
            "{ \"localAssetRoot\": \"" + jsonPath(missing) + "\" }");
        const ProjectAssetRoots roots =
            ProjectAssetRoots::fromProjectRoot(project.path, std::nullopt);
        CHECK(roots.configuration().configuredRoot);
        CHECK(!roots.localAssetRoot());
        CHECK(roots.roots().size() == 1);
        CHECK(!roots.configuration().diagnostics.empty());
        return true;
    }

    // <main>/.git/worktrees/<name> with a commondir file (git's layout), and the
    // same without commondir (structural fallback).
    bool testWorktreeFallback() {
        for (const bool commondir : { true, false }) {
            TemporaryDirectory main;
            TemporaryDirectory library;
            TemporaryDirectory worktree;
            const std::filesystem::path gitDir = main.path / ".git" / "worktrees" / "lane";
            std::filesystem::create_directories(gitDir);
            if (commondir) writeText(gitDir / "commondir", "../..\n");
            writeText(worktree.path / ".git", "gitdir: " + jsonPath(gitDir) + "\n");
            writeText(main.path / "iridium.local.json",
                "{ \"localAssetRoot\": \"" + jsonPath(library.path) + "\" }");

            const auto resolvedMain = mainCheckoutForWorktree(worktree.path);
            CHECK(resolvedMain && same(*resolvedMain, main.path));
            CHECK(!mainCheckoutForWorktree(main.path));   // .git is a directory

            const ProjectAssetRoots roots =
                ProjectAssetRoots::fromProjectRoot(worktree.path, std::nullopt);
            CHECK(roots.localAssetRoot());
            CHECK(same(*roots.localAssetRoot(), library.path));
            CHECK(roots.configuration().origin.find(
                main.path.filename().generic_string()) != std::string::npos);

            // The worktree's own file wins over the main checkout's, even when
            // it names no root.
            writeText(worktree.path / "iridium.local.json", "{}");
            const ProjectAssetRoots own =
                ProjectAssetRoots::fromProjectRoot(worktree.path, std::nullopt);
            CHECK(!own.localAssetRoot());
            CHECK(own.roots().size() == 1);
        }

        // A relative gitdir resolves against the worktree.
        TemporaryDirectory parent;
        const std::filesystem::path mainPath = parent.path / "main";
        const std::filesystem::path worktreePath = parent.path / "wt";
        std::filesystem::create_directories(mainPath / ".git" / "worktrees" / "wt");
        std::filesystem::create_directories(worktreePath);
        writeText(mainPath / ".git" / "worktrees" / "wt" / "commondir", "../..");
        writeText(worktreePath / ".git", "gitdir: ../main/.git/worktrees/wt");
        const auto relativeMain = mainCheckoutForWorktree(worktreePath);
        CHECK(relativeMain && same(*relativeMain, mainPath));
        return true;
    }

    bool testEnvironmentOverride() {
        TemporaryDirectory project;
        TemporaryDirectory fileLibrary;
        TemporaryDirectory environmentLibrary;
        std::filesystem::create_directories(project.path / "assets");
        writeText(project.path / "iridium.local.json",
            "{ \"localAssetRoot\": \"" + jsonPath(fileLibrary.path) + "\" }");
        const ProjectAssetRoots roots = ProjectAssetRoots::fromProjectRoot(
            project.path, environmentLibrary.path.string());
        CHECK(roots.configuration().origin == "environment");
        CHECK(roots.localAssetRoot());
        CHECK(same(*roots.localAssetRoot(), environmentLibrary.path));
        return true;
    }

    bool testResolutionOrder() {
        TemporaryDirectory project;
        TemporaryDirectory library;
        const std::filesystem::path assets = project.path / "assets";
        writeText(assets / "shared.txt", "project");
        writeText(library.path / "shared.txt", "local");
        writeText(library.path / "models" / "car" / "car.gltf", "{}");
        std::filesystem::create_directories(assets / "benchmarks");
        writeText(project.path / "iridium.local.json",
            "{ \"localAssetRoot\": \"" + jsonPath(library.path) + "\" }");
        const ProjectAssetRoots roots =
            ProjectAssetRoots::fromProjectRoot(project.path, std::nullopt);
        CHECK(roots.localAssetRoot());

        CHECK(roots.resolve("shared.txt") == assets / "shared.txt");
        CHECK(roots.resolveRoot("shared.txt").id == "project");
        CHECK(same(roots.resolve("models/car/car.gltf"),
            library.path / "models" / "car" / "car.gltf"));
        CHECK(roots.resolveRoot("models/car/car.gltf").id == "local");
        CHECK(roots.resolve("models/none.gltf") == assets / "models/none.gltf");
        CHECK(roots.resolveRoot("models/none.gltf").id == "project");
        const std::filesystem::path absolute = library.path / "shared.txt";
        CHECK(roots.resolve(absolute) == absolute);
        CHECK(roots.resolveRoot(absolute).id == "local");
        CHECK(roots.resolveRoot(assets / "shared.txt").id == "project");
        CHECK(roots.resolveRoot(project.path / "elsewhere.txt").id == "project");

        const auto mapped = roots.mapProjectPathToLocal(assets / "benchmarks" / "m0");
        CHECK(mapped && same(*mapped, library.path / "benchmarks" / "m0"));
        const auto top = roots.mapProjectPathToLocal(assets);
        CHECK(top && same(*top, library.path));
        CHECK(!roots.mapProjectPathToLocal(project.path / "tests"));
        CHECK(!roots.mapProjectPathToLocal(project.path));
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "iridium.local.json parsing", testParse },
        { "No configuration means the project root only", testAbsenceIsProjectOnly },
        { "Own iridium.local.json adds the local root", testOwnConfigFile },
        { "Configured but missing root is ignored", testConfiguredButMissing },
        { "Worktree falls back to the main checkout", testWorktreeFallback },
        { "Environment variable overrides the file", testEnvironmentOverride },
        { "Resolution order project, local, project", testResolutionOrder },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
