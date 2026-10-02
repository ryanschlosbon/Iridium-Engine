#include "scene/authoring/AtomicSourceSceneFile.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " \
    << __LINE__ << ": " #value "\n"; return false; } } while (false)

    struct TemporaryDirectory {
        std::filesystem::path path;

        TemporaryDirectory() {
            path = std::filesystem::temp_directory_path() /
                ("iridium-atomic-scene-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    void write(const std::filesystem::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(input), {} };
    }

    bool verifyScene(std::string_view bytes, std::string& diagnostic) {
        if (bytes.starts_with("{\"format\":\"iridium.scene\"")) return true;
        diagnostic = "not an Iridium source scene";
        return false;
    }

    bool firstSaveAndReplacementRetainBackup() {
        TemporaryDirectory temporary;
        const auto destination = temporary.path / "scene.iridium.json";
        constexpr std::string_view first =
            "{\"format\":\"iridium.scene\",\"revision\":1}";
        constexpr std::string_view second =
            "{\"format\":\"iridium.scene\",\"revision\":2}";

        const auto initial = Iridium::saveSourceSceneAtomically(
            destination, first, verifyScene);
        CHECK(initial);
        CHECK(read(destination) == first);
        CHECK(!std::filesystem::exists(initial.backup));

        const auto replacement = Iridium::saveSourceSceneAtomically(
            destination, second, verifyScene);
        CHECK(replacement);
        CHECK(read(destination) == second);
        CHECK(read(replacement.backup) == first);
        CHECK(!replacement.contentSha256.empty());
        return true;
    }

    bool everyPreReplaceFailureIsNonDestructive() {
        TemporaryDirectory temporary;
        const auto destination = temporary.path / "scene.iridium.json";
        const auto backup = std::filesystem::path(destination.string() + ".bak");
        constexpr std::string_view original =
            "{\"format\":\"iridium.scene\",\"revision\":1}";
        constexpr std::string_view oldBackup =
            "{\"format\":\"iridium.scene\",\"revision\":0}";
        constexpr std::string_view replacement =
            "{\"format\":\"iridium.scene\",\"revision\":2}";

        constexpr std::array phases{
            Iridium::AtomicSceneSavePhase::CreateTemporary,
            Iridium::AtomicSceneSavePhase::WriteTemporary,
            Iridium::AtomicSceneSavePhase::FlushTemporary,
            Iridium::AtomicSceneSavePhase::VerifyTemporary,
            Iridium::AtomicSceneSavePhase::ReplaceDestination,
        };
        for (const auto phase : phases) {
            write(destination, original);
            write(backup, oldBackup);
            const auto result = Iridium::saveSourceSceneAtomically(
                destination, replacement, verifyScene, { phase });
            CHECK(!result);
            CHECK(result.failedPhase == phase);
            CHECK(read(destination) == original);
            CHECK(read(backup) == oldBackup);
            CHECK(!result.retainedTemporary.has_value());
        }
        return true;
    }

    bool semanticVerificationFailureNeverAdopts() {
        TemporaryDirectory temporary;
        const auto destination = temporary.path / "scene.iridium.json";
        constexpr std::string_view original =
            "{\"format\":\"iridium.scene\",\"revision\":1}";
        write(destination, original);

        const auto result = Iridium::saveSourceSceneAtomically(
            destination, "{\"format\":\"other\"}", verifyScene);
        CHECK(!result);
        CHECK(result.failedPhase ==
            Iridium::AtomicSceneSavePhase::VerifyTemporary);
        CHECK(read(destination) == original);
        CHECK(!std::filesystem::exists(result.backup));
        return true;
    }

    bool orphanDiscoveryIsExactAndReadOnly() {
        TemporaryDirectory temporary;
        const auto destination = temporary.path / "scene.iridium.scene.json";
        const auto first = temporary.path /
            ".scene.iridium.scene.json.iridium-save-crash-2";
        const auto second = temporary.path /
            ".scene.iridium.scene.json.iridium-save-crash-1";
        const auto unrelated = temporary.path / ".other.iridium-save-crash";
        write(first, "candidate-two");
        write(second, "candidate-one");
        write(unrelated, "unrelated");

        const auto candidates =
            Iridium::findOrphanedSceneTemporaries(destination);
        CHECK(candidates.size() == 2);
        CHECK(candidates[0].path == second);
        CHECK(candidates[1].path == first);
        CHECK(candidates[0].sizeBytes == std::string_view("candidate-one").size());
        CHECK(candidates[0].contentSha256.size() == 64);
        CHECK(read(first) == "candidate-two");
        CHECK(read(second) == "candidate-one");
        CHECK(!std::filesystem::exists(destination));
        return true;
    }

} // namespace

int main() {
    const std::array tests{
        std::pair{ "first save and backup replacement",
            firstSaveAndReplacementRetainBackup },
        std::pair{ "pre-replace failures preserve destination",
            everyPreReplaceFailureIsNonDestructive },
        std::pair{ "semantic verification failure",
            semanticVerificationFailureNeverAdopts },
        std::pair{ "read-only orphan discovery",
            orphanDiscoveryIsExactAndReadOnly },
    };
    for (const auto& [name, run] : tests) {
        if (!run()) {
            std::cerr << "[FAIL] " << name << '\n';
            return 1;
        }
        std::cout << "[PASS] " << name << '\n';
    }
    return 0;
}
