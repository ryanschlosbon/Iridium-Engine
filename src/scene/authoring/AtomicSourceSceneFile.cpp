#include "scene/authoring/AtomicSourceSceneFile.h"

#include "utils/Sha256.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Iridium {
    namespace {

        std::atomic<uint64_t> temporarySequence = 0;

        [[nodiscard]] std::filesystem::path siblingPath(
            const std::filesystem::path& destination,
            std::string_view role) {
            const uint64_t sequence = temporarySequence.fetch_add(
                1, std::memory_order_relaxed);
            const uint64_t ticks = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            return destination.parent_path() /
                ("." + destination.filename().string() + ".iridium-" +
                    std::string(role) + "-" + std::to_string(ticks) + "-" +
                    std::to_string(sequence));
        }

        [[nodiscard]] std::vector<std::byte> asBytes(std::string_view text) {
            const auto* first = reinterpret_cast<const std::byte*>(text.data());
            return { first, first + text.size() };
        }

        [[nodiscard]] bool readAll(
            const std::filesystem::path& path,
            std::string& bytes,
            std::string& diagnostic) {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                diagnostic = "Could not reopen the scene temporary for verification";
                return false;
            }
            const std::streamoff size = input.tellg();
            if (size < 0) {
                diagnostic = "Could not determine the scene temporary size";
                return false;
            }
            bytes.resize(static_cast<size_t>(size));
            input.seekg(0, std::ios::beg);
            if (size != 0 && !input.read(bytes.data(), size)) {
                diagnostic = "Could not read the complete scene temporary";
                return false;
            }
            return true;
        }

#ifdef _WIN32
        [[nodiscard]] std::string windowsMessage(DWORD code) {
            return "Windows error " + std::to_string(code);
        }

        [[nodiscard]] bool writeAndFlush(
            const std::filesystem::path& path,
            std::string_view bytes,
            AtomicSceneSavePhase failBefore,
            AtomicSceneSavePhase& failedPhase,
            std::string& diagnostic) {
            if (failBefore == AtomicSceneSavePhase::CreateTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary creation failure";
                return false;
            }
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                failedPhase = AtomicSceneSavePhase::CreateTemporary;
                diagnostic = "Could not create a unique scene temporary: " +
                    windowsMessage(GetLastError());
                return false;
            }

            bool succeeded = true;
            if (failBefore == AtomicSceneSavePhase::WriteTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary write failure";
                succeeded = false;
            }
            size_t offset = 0;
            while (succeeded && offset < bytes.size()) {
                const DWORD request = static_cast<DWORD>((std::min)(
                    bytes.size() - offset,
                    static_cast<size_t>(UINT32_MAX)));
                DWORD written = 0;
                if (!WriteFile(file, bytes.data() + offset, request,
                        &written, nullptr) || written == 0) {
                    failedPhase = AtomicSceneSavePhase::WriteTemporary;
                    diagnostic = "Could not write the complete scene temporary: " +
                        windowsMessage(GetLastError());
                    succeeded = false;
                    break;
                }
                offset += written;
            }
            if (succeeded &&
                failBefore == AtomicSceneSavePhase::FlushTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary flush failure";
                succeeded = false;
            }
            if (succeeded && !FlushFileBuffers(file)) {
                failedPhase = AtomicSceneSavePhase::FlushTemporary;
                diagnostic = "Could not flush the scene temporary: " +
                    windowsMessage(GetLastError());
                succeeded = false;
            }
            if (!CloseHandle(file) && succeeded) {
                failedPhase = AtomicSceneSavePhase::FlushTemporary;
                diagnostic = "Could not close the scene temporary after flush: " +
                    windowsMessage(GetLastError());
                succeeded = false;
            }
            return succeeded;
        }

        [[nodiscard]] bool replaceFile(
            const std::filesystem::path& destination,
            const std::filesystem::path& temporary,
            const std::filesystem::path& backup,
            AtomicSceneSavePhase failBefore,
            std::string& diagnostic) {
            if (failBefore == AtomicSceneSavePhase::ReplaceDestination) {
                diagnostic = "Injected destination replacement failure";
                return false;
            }

            std::error_code existsError;
            const bool destinationExists = std::filesystem::exists(
                destination, existsError);
            if (existsError) {
                diagnostic = "Could not inspect the scene destination: " +
                    existsError.message();
                return false;
            }
            if (!destinationExists) {
                if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                    diagnostic = "Could not adopt the first scene save: " +
                        windowsMessage(GetLastError());
                    return false;
                }
                return true;
            }

            const std::filesystem::path retiredBackup = siblingPath(
                destination, "retired-backup");
            bool movedOldBackup = false;
            const bool backupExists = std::filesystem::exists(backup, existsError);
            if (existsError) {
                diagnostic = "Could not inspect the scene backup: " +
                    existsError.message();
                return false;
            }
            if (backupExists) {
                if (!MoveFileExW(backup.c_str(), retiredBackup.c_str(),
                        MOVEFILE_WRITE_THROUGH)) {
                    diagnostic = "Could not preserve the previous scene backup: " +
                        windowsMessage(GetLastError());
                    return false;
                }
                movedOldBackup = true;
            }

            if (!ReplaceFileW(destination.c_str(), temporary.c_str(),
                    backup.c_str(), REPLACEFILE_WRITE_THROUGH |
                        REPLACEFILE_IGNORE_MERGE_ERRORS,
                    nullptr, nullptr)) {
                const DWORD error = GetLastError();
                if (movedOldBackup) {
                    (void)MoveFileExW(retiredBackup.c_str(), backup.c_str(),
                        MOVEFILE_WRITE_THROUGH);
                }
                diagnostic = "Could not atomically replace the scene: " +
                    windowsMessage(error);
                return false;
            }
            if (movedOldBackup) {
                std::error_code removeError;
                std::filesystem::remove(retiredBackup, removeError);
            }
            return true;
        }
#else
        [[nodiscard]] bool writeAndFlush(
            const std::filesystem::path& path,
            std::string_view bytes,
            AtomicSceneSavePhase failBefore,
            AtomicSceneSavePhase& failedPhase,
            std::string& diagnostic) {
            if (failBefore == AtomicSceneSavePhase::CreateTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary creation failure";
                return false;
            }
            const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (file < 0) {
                failedPhase = AtomicSceneSavePhase::CreateTemporary;
                diagnostic = "Could not create a unique scene temporary";
                return false;
            }
            bool succeeded = true;
            if (failBefore == AtomicSceneSavePhase::WriteTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary write failure";
                succeeded = false;
            }
            size_t offset = 0;
            while (succeeded && offset < bytes.size()) {
                const ssize_t written = ::write(file, bytes.data() + offset,
                    bytes.size() - offset);
                if (written <= 0) {
                    failedPhase = AtomicSceneSavePhase::WriteTemporary;
                    diagnostic = "Could not write the complete scene temporary";
                    succeeded = false;
                    break;
                }
                offset += static_cast<size_t>(written);
            }
            if (succeeded && failBefore == AtomicSceneSavePhase::FlushTemporary) {
                failedPhase = failBefore;
                diagnostic = "Injected temporary flush failure";
                succeeded = false;
            }
            if (succeeded && ::fsync(file) != 0) {
                failedPhase = AtomicSceneSavePhase::FlushTemporary;
                diagnostic = "Could not flush the scene temporary";
                succeeded = false;
            }
            (void)::close(file);
            return succeeded;
        }

        [[nodiscard]] bool replaceFile(
            const std::filesystem::path& destination,
            const std::filesystem::path& temporary,
            const std::filesystem::path& backup,
            AtomicSceneSavePhase failBefore,
            std::string& diagnostic) {
            if (failBefore == AtomicSceneSavePhase::ReplaceDestination) {
                diagnostic = "Injected destination replacement failure";
                return false;
            }
            std::error_code error;
            if (std::filesystem::exists(destination, error)) {
                const std::filesystem::path stagedBackup = siblingPath(
                    destination, "new-backup");
                std::filesystem::copy_file(destination, stagedBackup,
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error) {
                    diagnostic = "Could not stage the scene backup: " + error.message();
                    return false;
                }
                std::filesystem::rename(temporary, destination, error);
                if (error) {
                    std::filesystem::remove(stagedBackup);
                    diagnostic = "Could not atomically replace the scene: " +
                        error.message();
                    return false;
                }
                std::filesystem::rename(stagedBackup, backup, error);
                if (error) {
                    diagnostic = "Scene replaced, but backup adoption failed: " +
                        error.message();
                    return false;
                }
                return true;
            }
            std::filesystem::rename(temporary, destination, error);
            if (error) {
                diagnostic = "Could not adopt the first scene save: " + error.message();
                return false;
            }
            return true;
        }
#endif

    } // namespace

    AtomicSceneSaveResult saveSourceSceneAtomically(
        const std::filesystem::path& requestedDestination,
        std::string_view canonicalBytes,
        const SourceSceneFileVerifier& verifier,
        AtomicSceneSaveOptions options) {
        AtomicSceneSaveResult result;
        result.destination = requestedDestination.lexically_normal();
        result.backup = result.destination;
        result.backup += ".bak";
        if (result.destination.empty() || !result.destination.has_filename()) {
            result.failedPhase = AtomicSceneSavePhase::CreateTemporary;
            result.diagnostic = "Scene destination must name a file";
            return result;
        }
        if (!verifier) {
            result.failedPhase = AtomicSceneSavePhase::VerifyTemporary;
            result.diagnostic = "Scene save requires a verification callback";
            return result;
        }

        std::error_code directoryError;
        const std::filesystem::path parent = result.destination.has_parent_path()
            ? result.destination.parent_path()
            : std::filesystem::current_path(directoryError);
        if (directoryError) {
            result.failedPhase = AtomicSceneSavePhase::CreateTemporary;
            result.diagnostic = "Could not resolve the scene directory: " +
                directoryError.message();
            return result;
        }
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            result.failedPhase = AtomicSceneSavePhase::CreateTemporary;
            result.diagnostic = "Could not create the scene directory: " +
                directoryError.message();
            return result;
        }

        const std::filesystem::path temporary = siblingPath(
            result.destination, "save");
        result.retainedTemporary = temporary;
        if (!writeAndFlush(temporary, canonicalBytes, options.failBefore,
                result.failedPhase, result.diagnostic)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }

        if (options.failBefore == AtomicSceneSavePhase::VerifyTemporary) {
            result.failedPhase = options.failBefore;
            result.diagnostic = "Injected temporary verification failure";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }
        std::string reopened;
        if (!readAll(temporary, reopened, result.diagnostic)) {
            result.failedPhase = AtomicSceneSavePhase::VerifyTemporary;
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }
        const std::vector<std::byte> expectedBytes = asBytes(canonicalBytes);
        const std::vector<std::byte> reopenedBytes = asBytes(reopened);
        result.contentSha256 = sha256(expectedBytes);
        if (sha256(reopenedBytes) != result.contentSha256 ||
            reopened != canonicalBytes) {
            result.failedPhase = AtomicSceneSavePhase::VerifyTemporary;
            result.diagnostic = "Scene temporary content hash did not match";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }
        if (!verifier(reopened, result.diagnostic)) {
            result.failedPhase = AtomicSceneSavePhase::VerifyTemporary;
            if (result.diagnostic.empty()) {
                result.diagnostic = "Scene temporary failed semantic verification";
            }
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }

        if (!replaceFile(result.destination, temporary, result.backup,
                options.failBefore, result.diagnostic)) {
            result.failedPhase = AtomicSceneSavePhase::ReplaceDestination;
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            result.retainedTemporary.reset();
            return result;
        }
        result.retainedTemporary.reset();
        result.saved = true;
        return result;
    }

    std::vector<OrphanedSceneTemporary> findOrphanedSceneTemporaries(
        const std::filesystem::path& requestedDestination) {
        std::vector<OrphanedSceneTemporary> result;
        const std::filesystem::path destination =
            requestedDestination.lexically_normal();
        if (destination.empty() || !destination.has_filename()) return result;
        std::error_code error;
        const std::filesystem::path parent = destination.has_parent_path()
            ? destination.parent_path()
            : std::filesystem::current_path(error);
        if (error) return result;
        const std::string prefix = "." + destination.filename().string() +
            ".iridium-save-";
        for (std::filesystem::directory_iterator iterator(parent, error), end;
            !error && iterator != end; iterator.increment(error)) {
            const std::filesystem::directory_entry& entry = *iterator;
            if (!entry.is_regular_file(error) || error ||
                !entry.path().filename().string().starts_with(prefix)) {
                error.clear();
                continue;
            }
            OrphanedSceneTemporary candidate;
            candidate.path = entry.path();
            candidate.sizeBytes = entry.file_size(error);
            if (error) { error.clear(); continue; }
            candidate.lastWriteTime = entry.last_write_time(error);
            if (error) { error.clear(); continue; }
            try {
                candidate.contentSha256 = sha256File(candidate.path);
            }
            catch (...) {
                continue;
            }
            result.push_back(std::move(candidate));
        }
        std::ranges::sort(result,
            [](const OrphanedSceneTemporary& lhs,
                const OrphanedSceneTemporary& rhs) {
                return lhs.path.filename().string() <
                    rhs.path.filename().string();
            });
        return result;
    }

} // namespace Iridium
