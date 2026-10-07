#include "assets/cooker/LocalDerivedDataCache.h"

#include "core/tasks/TaskSystem.h"
#include "core/types/AssetGuid.h"

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

    // A cook in flight (M7R R5b.2): one Background task per cook key. It runs
    // the job, publishes the result to the shared future, then calls the
    // continuations registered by every request of the key.
    class LocalDerivedDataCache::CookTask final : public Tasks::TaskSet {
    public:
        CookTask(LocalDerivedDataCache& owner, PendingJob job)
            : TaskSet(Tasks::TaskPriority::Background, 1, 1, "asset.ddc.cook"),
              owner_(owner), job_(std::move(job)),
              future_(job_.promise->get_future().share()) {}

        [[nodiscard]] const std::shared_future<DdcRequestResult>& future() const noexcept {
            return future_;
        }
        void addContinuation(DdcCompletion continuation) {
            if (continuation) continuations_.push_back(std::move(continuation));
        }
        // The task system cancelled the task (shutdown) before it ran.
        void completeCancelled() {
            complete({ .status = DdcRequestStatus::Cancelled });
        }

    private:
        void execute(Tasks::TaskRange, uint32_t) override {
            complete(owner_.execute(job_));
        }

        void complete(DdcRequestResult result) {
            std::vector<DdcCompletion> continuations;
            {
                std::lock_guard lock(owner_.m_mutex);
                if (finished_) return;
                finished_ = true;
                owner_.m_inFlight.erase(job_.cookKey);
                continuations.swap(continuations_);
            }
            job_.promise->set_value(std::move(result));
            const DdcRequestResult& published = future_.get();
            for (DdcCompletion& continuation : continuations) {
                try {
                    continuation(published);
                }
                catch (...) {
                    // Continuations report through their own result queues.
                }
            }
        }

        LocalDerivedDataCache& owner_;
        PendingJob job_;
        std::shared_future<DdcRequestResult> future_;
        std::vector<DdcCompletion> continuations_;

    public:
        // Guarded by the owner's mutex: the result was published (a task that
        // was created but not yet submitted also reads as complete).
        bool finished_ = false;
    };

    LocalDerivedDataCache::LocalDerivedDataCache(std::filesystem::path root,
        Tasks::TaskSystem& tasks)
        : m_root(std::move(root)),
          m_tasks_system(tasks) {
        std::error_code filesystemError;
        std::filesystem::create_directories(m_root, filesystemError);
        if (filesystemError) {
            throw std::runtime_error("Could not create local DDC root: " +
                filesystemError.message());
        }
    }

    LocalDerivedDataCache::~LocalDerivedDataCache() {
        // Queued cooks still run (their requests' stop tokens cancel them),
        // as the former cache thread drained its queue before joining.
        std::vector<std::unique_ptr<CookTask>> tasks;
        {
            std::lock_guard lock(m_mutex);
            tasks.swap(m_tasks);
        }
        for (const std::unique_ptr<CookTask>& task : tasks) {
            if (!task->isComplete()) {
                m_tasks_system.wait(*task);
            }
            if (task->wasCancelled()) {
                task->completeCancelled();
            }
        }
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
        return request(std::move(cookKey), stopToken, std::move(builder), {});
    }

    std::shared_future<DdcRequestResult> LocalDerivedDataCache::request(
        std::string cookKey, std::stop_token stopToken, DdcBuilder builder,
        DdcCompletion onComplete) {
        CookTask* task = nullptr;
        {
            std::lock_guard lock(m_mutex);
            const auto existing = m_inFlight.find(cookKey);
            if (existing != m_inFlight.end()) {
                existing->second->addContinuation(std::move(onComplete));
                return existing->second->future();
            }
            // Completed cook tasks are reclaimed here and in the destructor.
            std::erase_if(m_tasks, [](const std::unique_ptr<CookTask>& done) {
                return done->finished_ && done->isComplete();
            });
            auto created = std::make_unique<CookTask>(*this, PendingJob{
                .cookKey = cookKey,
                .stopToken = stopToken,
                .builder = std::move(builder),
                .promise = std::make_shared<std::promise<DdcRequestResult>>(),
            });
            task = created.get();
            task->addContinuation(std::move(onComplete));
            m_inFlight.emplace(std::move(cookKey), task);
            m_tasks.push_back(std::move(created));
        }
        std::shared_future<DdcRequestResult> future = task->future();
        try {
            m_tasks_system.submit(*task);
        }
        catch (...) {
            task->completeCancelled();
            throw;
        }
        if (task->wasCancelled()) {
            // The task system is shutting down.
            task->completeCancelled();
        }
        return future;
    }

    DdcRequestResult LocalDerivedDataCache::resolve(
        std::string cookKey, std::stop_token stopToken, DdcBuilder builder) {
        PendingJob job{
            .cookKey = std::move(cookKey),
            .stopToken = stopToken,
            .builder = std::move(builder),
        };
        return execute(job);
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

} // namespace Iridium
