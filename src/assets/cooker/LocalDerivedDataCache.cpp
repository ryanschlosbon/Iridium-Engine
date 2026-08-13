#include "assets/cooker/LocalDerivedDataCache.h"

#include "assets/AssetGuid.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Iridium {

    namespace {

        bool flushFile(const std::filesystem::path& path) {
#if defined(_WIN32)
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            const bool flushed = FlushFileBuffers(file) != 0;
            CloseHandle(file);
            return flushed;
#else
            const int file = open(path.c_str(), O_RDONLY);
            if (file < 0) return false;
            const bool flushed = fsync(file) == 0;
            close(file);
            return flushed;
#endif
        }

        bool atomicPublish(const std::filesystem::path& temporary,
            const std::filesystem::path& destination) {
#if defined(_WIN32)
            return MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_WRITE_THROUGH) != 0;
#else
            return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
        }

        CookDiagnostic error(std::string code, std::string message) {
            return {
                .code = std::move(code),
                .message = std::move(message),
            };
        }

        void downgradeForRebuild(std::vector<CookDiagnostic>& diagnostics) {
            for (CookDiagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    diagnostic.severity = CookDiagnosticSeverity::Warning;
                }
            }
        }

    } // namespace

    LocalDerivedDataCache::LocalDerivedDataCache(std::filesystem::path root)
        : m_root(std::move(root)),
          m_worker([this](std::stop_token stopToken) { workerLoop(stopToken); }) {
        std::error_code filesystemError;
        std::filesystem::create_directories(m_root, filesystemError);
        if (filesystemError) {
            m_worker.request_stop();
            throw std::runtime_error("Could not create local DDC root: " +
                filesystemError.message());
        }
    }

    LocalDerivedDataCache::~LocalDerivedDataCache() {
        m_worker.request_stop();
        m_condition.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    bool LocalDerivedDataCache::validCookKey(
        std::string_view cookKey) const noexcept {
        return cookKey.size() == 64 &&
            std::all_of(cookKey.begin(), cookKey.end(), [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
    }

    std::filesystem::path LocalDerivedDataCache::entryPath(
        std::string_view cookKey) const {
        if (!validCookKey(cookKey)) {
            throw std::invalid_argument("DDC cook key must be lower-case SHA-256 text.");
        }
        return m_root / std::string(cookKey.substr(0, 2)) /
            (std::string(cookKey.substr(2)) + ".irartifact");
    }

    DdcProbeResult LocalDerivedDataCache::probe(std::string_view cookKey) const {
        DdcProbeResult result;
        if (!validCookKey(cookKey)) {
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics.push_back(error("DDC_KEY_INVALID",
                "DDC cook key is not canonical lower-case SHA-256 text."));
            return result;
        }
        const std::filesystem::path path = entryPath(cookKey);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return result;
        const std::streamsize size = input.tellg();
        if (size < static_cast<std::streamsize>(kCookedArtifactHeaderSize)) {
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics.push_back(error("DDC_ENTRY_TRUNCATED",
                "DDC entry is smaller than the cooked artifact header."));
            return result;
        }
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> header(kCookedArtifactHeaderSize);
        if (!input.read(reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()))) {
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics.push_back(error("DDC_HEADER_READ",
                "DDC entry header could not be read."));
            return result;
        }
        const CookedArtifactHeaderProbe artifactProbe =
            probeCookedArtifactHeader(header, static_cast<uint64_t>(size), cookKey);
        if (!artifactProbe.valid) {
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics = artifactProbe.diagnostics;
            return result;
        }
        result.status = DdcLookupStatus::Hit;
        result.artifactSize = static_cast<uint64_t>(size);
        return result;
    }

    void LocalDerivedDataCache::quarantine(const std::filesystem::path& path,
        std::string_view cookKey, std::vector<CookDiagnostic>& diagnostics) {
        std::error_code filesystemError;
        const std::filesystem::path quarantineRoot = m_root / "quarantine";
        std::filesystem::create_directories(quarantineRoot, filesystemError);
        if (filesystemError) {
            diagnostics.push_back(error("DDC_QUARANTINE_DIRECTORY",
                "Could not create DDC quarantine directory: " +
                    filesystemError.message()));
            return;
        }
        const std::filesystem::path destination = quarantineRoot /
            (std::string(cookKey) + "." + createAssetGuidV7().toString() + ".corrupt");
        std::filesystem::rename(path, destination, filesystemError);
        if (filesystemError) {
            diagnostics.push_back(error("DDC_QUARANTINE_MOVE",
                "Could not quarantine corrupt DDC entry: " +
                    filesystemError.message()));
        } else {
            diagnostics.push_back({
                .severity = CookDiagnosticSeverity::Warning,
                .code = "DDC_ENTRY_QUARANTINED",
                .message = "Corrupt DDC entry was quarantined for diagnosis.",
            });
        }
    }

    DdcReadResult LocalDerivedDataCache::read(
        std::string_view cookKey, bool quarantineCorrupt) {
        DdcReadResult result;
        const DdcProbeResult header = probe(cookKey);
        result.status = header.status;
        result.diagnostics = header.diagnostics;
        if (header.status == DdcLookupStatus::Miss) return result;
        const std::filesystem::path path = entryPath(cookKey);
        if (header.status == DdcLookupStatus::Corrupt) {
            if (quarantineCorrupt && std::filesystem::exists(path)) {
                quarantine(path, cookKey, result.diagnostics);
            }
            return result;
        }

        std::ifstream input(path, std::ios::binary);
        std::vector<std::byte> bytes(
            static_cast<size_t>(header.artifactSize));
        if (!input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
            input.close();
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics.push_back(error("DDC_ENTRY_READ",
                "DDC entry payload could not be read."));
            if (quarantineCorrupt) quarantine(path, cookKey, result.diagnostics);
            return result;
        }
        input.close();
        const CookedArtifactReadResult artifact = readCookedArtifact(bytes);
        if (!artifact.valid() || artifact.artifact->cookKey != cookKey) {
            result.status = DdcLookupStatus::Corrupt;
            result.diagnostics.insert(result.diagnostics.end(),
                artifact.diagnostics.begin(), artifact.diagnostics.end());
            if (artifact.valid() && artifact.artifact->cookKey != cookKey) {
                result.diagnostics.push_back(error("DDC_COOK_KEY_MISMATCH",
                    "DDC entry contains a different cook key."));
            }
            if (quarantineCorrupt) quarantine(path, cookKey, result.diagnostics);
            return result;
        }
        result.blob = CookedArtifactBlob{
            .bytes = std::move(bytes),
            .artifactHash = artifact.artifactHash,
        };
        return result;
    }

    std::vector<CookDiagnostic> LocalDerivedDataCache::storeAtomic(
        std::string_view cookKey, const CookedArtifactBlob& blob) {
        std::vector<CookDiagnostic> diagnostics;
        if (!validCookKey(cookKey)) {
            diagnostics.push_back(error("DDC_KEY_INVALID",
                "DDC cook key is not canonical lower-case SHA-256 text."));
            return diagnostics;
        }
        const CookedArtifactReadResult decoded =
            readCookedArtifact(blob.bytes, blob.artifactHash);
        if (!decoded.valid() || decoded.artifact->cookKey != cookKey) {
            diagnostics = decoded.diagnostics;
            diagnostics.push_back(error("DDC_PUBLISH_REJECTED",
                "DDC refused to publish an invalid or mismatched artifact."));
            return diagnostics;
        }

        const std::filesystem::path destination = entryPath(cookKey);
        std::error_code filesystemError;
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
        if (filesystemError) {
            diagnostics.push_back(error("DDC_DIRECTORY_CREATE",
                "Could not create DDC entry directory: " +
                    filesystemError.message()));
            return diagnostics;
        }

        if (std::filesystem::exists(destination)) {
            DdcReadResult existing = read(cookKey);
            if (existing.status == DdcLookupStatus::Corrupt) {
                downgradeForRebuild(existing.diagnostics);
            }
            diagnostics.insert(diagnostics.end(), existing.diagnostics.begin(),
                existing.diagnostics.end());
            if (existing.status == DdcLookupStatus::Hit && existing.blob) {
                if (existing.blob->artifactHash == blob.artifactHash) return diagnostics;
                diagnostics.push_back(error("DDC_KEY_COLLISION",
                    "A valid entry with the same cook key has different bytes."));
                return diagnostics;
            }
        }

        const std::filesystem::path temporary = destination.string() + "." +
            createAssetGuidV7().toString() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                diagnostics.push_back(error("DDC_TEMP_OPEN",
                    "Could not open temporary DDC entry."));
                return diagnostics;
            }
            output.write(reinterpret_cast<const char*>(blob.bytes.data()),
                static_cast<std::streamsize>(blob.bytes.size()));
            output.flush();
            if (!output) {
                diagnostics.push_back(error("DDC_TEMP_WRITE",
                    "Could not write temporary DDC entry."));
            }
        }
        const bool flushed = !hasCookErrors(diagnostics) && flushFile(temporary);
        const bool published = flushed && atomicPublish(temporary, destination);
        if (!published && flushed) {
            DdcReadResult raced = read(cookKey);
            if (raced.status == DdcLookupStatus::Hit && raced.blob &&
                raced.blob->artifactHash == blob.artifactHash) {
                std::filesystem::remove(temporary, filesystemError);
                return diagnostics;
            }
        }
        if (hasCookErrors(diagnostics) || !published) {
            if (!hasCookErrors(diagnostics)) {
                diagnostics.push_back(error("DDC_ATOMIC_PUBLISH",
                    "Could not flush and atomically publish DDC entry."));
            }
            std::filesystem::remove(temporary, filesystemError);
        }
        return diagnostics;
    }

    std::shared_future<DdcRequestResult> LocalDerivedDataCache::request(
        std::string cookKey, std::stop_token stopToken, DdcBuilder builder) {
        std::lock_guard lock(m_mutex);
        const auto existing = m_inFlight.find(cookKey);
        if (existing != m_inFlight.end()) return existing->second;

        auto promise = std::make_shared<std::promise<DdcRequestResult>>();
        std::shared_future<DdcRequestResult> future =
            promise->get_future().share();
        m_inFlight.emplace(cookKey, future);
        m_jobs.push_back({
            .cookKey = std::move(cookKey),
            .stopToken = stopToken,
            .builder = std::move(builder),
            .promise = std::move(promise),
        });
        m_condition.notify_one();
        return future;
    }

    DdcRequestResult LocalDerivedDataCache::execute(PendingJob& job) {
        if (job.stopToken.stop_requested()) {
            return { .status = DdcRequestStatus::Cancelled };
        }
        DdcReadResult cached = read(job.cookKey);
        if (cached.status == DdcLookupStatus::Hit) {
            return {
                .status = DdcRequestStatus::CacheHit,
                .blob = std::move(cached.blob),
                .diagnostics = std::move(cached.diagnostics),
            };
        }
        if (cached.status == DdcLookupStatus::Corrupt) {
            downgradeForRebuild(cached.diagnostics);
        }
        try {
            CookedArtifactBlob built = job.builder(job.stopToken);
            if (job.stopToken.stop_requested()) {
                return {
                    .status = DdcRequestStatus::Cancelled,
                    .diagnostics = std::move(cached.diagnostics),
                };
            }
            std::vector<CookDiagnostic> publish =
                storeAtomic(job.cookKey, built);
            cached.diagnostics.insert(cached.diagnostics.end(),
                publish.begin(), publish.end());
            if (hasCookErrors(cached.diagnostics)) {
                return {
                    .status = DdcRequestStatus::Failed,
                    .diagnostics = std::move(cached.diagnostics),
                };
            }
            return {
                .status = DdcRequestStatus::Built,
                .blob = std::move(built),
                .diagnostics = std::move(cached.diagnostics),
            };
        } catch (const std::exception& exception) {
            cached.diagnostics.push_back(error("DDC_BUILD_EXCEPTION", exception.what()));
            return {
                .status = DdcRequestStatus::Failed,
                .diagnostics = std::move(cached.diagnostics),
            };
        }
    }

    void LocalDerivedDataCache::workerLoop(std::stop_token stopToken) {
        while (true) {
            PendingJob job;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, stopToken,
                    [this] { return !m_jobs.empty(); });
                if (m_jobs.empty()) {
                    if (stopToken.stop_requested()) return;
                    continue;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            DdcRequestResult result = execute(job);
            {
                std::lock_guard lock(m_mutex);
                m_inFlight.erase(job.cookKey);
            }
            job.promise->set_value(std::move(result));
        }
    }

} // namespace Iridium
