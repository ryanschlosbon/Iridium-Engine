#pragma once

#include "assets/cooker/CookedArtifact.h"

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace Iridium {

    namespace Tasks {
        class TaskSystem;
    }

    enum class DdcLookupStatus : uint8_t {
        Miss,
        Hit,
        Corrupt,
    };

    struct DdcProbeResult {
        DdcLookupStatus status = DdcLookupStatus::Miss;
        uint64_t artifactSize = 0;
        std::vector<CookDiagnostic> diagnostics;
    };

    struct DdcReadResult {
        DdcLookupStatus status = DdcLookupStatus::Miss;
        std::optional<CookedArtifactBlob> blob;
        std::vector<CookDiagnostic> diagnostics;
    };

    enum class DdcRequestStatus : uint8_t {
        CacheHit,
        Built,
        Cancelled,
        Failed,
    };

    struct DdcRequestResult {
        DdcRequestStatus status = DdcRequestStatus::Failed;
        std::optional<CookedArtifactBlob> blob;
        std::vector<CookDiagnostic> diagnostics;
    };

    using DdcBuilder = std::function<CookedArtifactBlob(std::stop_token)>;
    // Called once with the request's result, after its future is ready, on the
    // task that finished the cook (M7R R5b.2). It must not block.
    using DdcCompletion = std::function<void(const DdcRequestResult&)>;

    class DerivedDataCache {
    public:
        virtual ~DerivedDataCache() = default;

        [[nodiscard]] virtual DdcProbeResult probe(
            std::string_view cookKey) const = 0;
        [[nodiscard]] virtual DdcReadResult read(
            std::string_view cookKey, bool quarantineCorrupt = true) = 0;
        [[nodiscard]] virtual std::vector<CookDiagnostic> storeAtomic(
            std::string_view cookKey, const CookedArtifactBlob& blob) = 0;
        [[nodiscard]] virtual std::shared_future<DdcRequestResult> request(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder) = 0;
        // As above, and calls onComplete with the result when the cook (or
        // the in-flight cook this request joined) finishes. Task-system work
        // continues through onComplete instead of blocking on the future.
        [[nodiscard]] virtual std::shared_future<DdcRequestResult> request(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder, DdcCompletion onComplete) = 0;
        // Reads the entry, or builds and stores it, on the calling thread. It
        // does not join a request in flight for the same key (both may build;
        // the store keeps one). For a single serial requester that would
        // otherwise block a task on request().get().
        [[nodiscard]] virtual DdcRequestResult resolve(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder) = 0;
    };

    // M7R R5b.2 (ADR-0015): each cook key in flight is a Background task on the
    // engine task system (de-duplicated by key), not a job on a cache thread.
    class LocalDerivedDataCache final : public DerivedDataCache {
    public:
        LocalDerivedDataCache(std::filesystem::path root, Tasks::TaskSystem& tasks);
        // Waits for the cooks in flight.
        ~LocalDerivedDataCache() override;

        LocalDerivedDataCache(const LocalDerivedDataCache&) = delete;
        LocalDerivedDataCache& operator=(const LocalDerivedDataCache&) = delete;

        [[nodiscard]] const std::filesystem::path& root() const noexcept {
            return m_root;
        }
        [[nodiscard]] std::filesystem::path entryPath(
            std::string_view cookKey) const;
        [[nodiscard]] DdcProbeResult probe(
            std::string_view cookKey) const override;
        [[nodiscard]] DdcReadResult read(std::string_view cookKey,
            bool quarantineCorrupt = true) override;
        [[nodiscard]] std::vector<CookDiagnostic> storeAtomic(
            std::string_view cookKey, const CookedArtifactBlob& blob) override;
        [[nodiscard]] std::shared_future<DdcRequestResult> request(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder) override;
        [[nodiscard]] std::shared_future<DdcRequestResult> request(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder, DdcCompletion onComplete) override;
        [[nodiscard]] DdcRequestResult resolve(
            std::string cookKey, std::stop_token stopToken,
            DdcBuilder builder) override;
        [[nodiscard]] Tasks::TaskSystem& taskSystem() const noexcept {
            return m_tasks_system;
        }

    private:
        class CookTask;
        struct PendingJob {
            std::string cookKey;
            std::stop_token stopToken;
            DdcBuilder builder;
            std::shared_ptr<std::promise<DdcRequestResult>> promise;
        };

        [[nodiscard]] bool validCookKey(std::string_view cookKey) const noexcept;
        void quarantine(const std::filesystem::path& path, std::string_view cookKey,
            std::vector<CookDiagnostic>& diagnostics);
        [[nodiscard]] DdcRequestResult execute(PendingJob& job);

        std::filesystem::path m_root;
        Tasks::TaskSystem& m_tasks_system;
        mutable std::mutex m_mutex;
        std::map<std::string, CookTask*> m_inFlight;
        std::vector<std::unique_ptr<CookTask>> m_tasks;
    };

} // namespace Iridium
