#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class AtomicSceneSavePhase {
        None,
        CreateTemporary,
        WriteTemporary,
        FlushTemporary,
        VerifyTemporary,
        ReplaceDestination,
    };

    struct AtomicSceneSaveOptions {
        // Test-only fault injection. Production callers leave this as None.
        AtomicSceneSavePhase failBefore = AtomicSceneSavePhase::None;
    };

    using SourceSceneFileVerifier = std::function<bool(
        std::string_view bytes, std::string& diagnostic)>;

    struct AtomicSceneSaveResult {
        bool saved = false;
        AtomicSceneSavePhase failedPhase = AtomicSceneSavePhase::None;
        std::filesystem::path destination;
        std::filesystem::path backup;
        std::optional<std::filesystem::path> retainedTemporary;
        std::string contentSha256;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept { return saved; }
    };

    struct OrphanedSceneTemporary {
        std::filesystem::path path;
        uintmax_t sizeBytes = 0;
        std::filesystem::file_time_type lastWriteTime{};
        std::string contentSha256;
    };

    // Writes a source scene through a unique sibling temporary, flushes it to
    // stable storage, reopens and verifies it, then atomically adopts it. An
    // existing destination becomes the single last-known-good .bak sibling.
    [[nodiscard]] AtomicSceneSaveResult saveSourceSceneAtomically(
        const std::filesystem::path& destination,
        std::string_view canonicalBytes,
        const SourceSceneFileVerifier& verifier,
        AtomicSceneSaveOptions options = {});

    // Reports only crash-left save temporaries for this exact destination. It
    // never deletes, adopts, parses, or otherwise mutates a candidate.
    [[nodiscard]] std::vector<OrphanedSceneTemporary>
        findOrphanedSceneTemporaries(
            const std::filesystem::path& destination);

} // namespace Iridium
