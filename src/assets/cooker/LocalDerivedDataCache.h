#pragma once

#include "assets/cooker/CookedArtifact.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

namespace Iridium {

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
    };

    class LocalDerivedDataCache final : public DerivedDataCache {
    public:
        explicit LocalDerivedDataCache(std::filesystem::path root);
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

    private:
        struct PendingJob {
            std::string cookKey;
            std::stop_token stopToken;
            DdcBuilder builder;
            std::shared_ptr<std::promise<DdcRequestResult>> promise;
        };

        [[nodiscard]] bool validCookKey(std::string_view cookKey) const noexcept;
        void quarantine(const std::filesystem::path& path, std::string_view cookKey,
            std::vector<CookDiagnostic>& diagnostics);
        void workerLoop(std::stop_token stopToken);
        [[nodiscard]] DdcRequestResult execute(PendingJob& job);

        std::filesystem::path m_root;
        mutable std::mutex m_mutex;
        std::condition_variable_any m_condition;
        std::deque<PendingJob> m_jobs;
        std::map<std::string, std::shared_future<DdcRequestResult>> m_inFlight;
        std::jthread m_worker;
    };

} // namespace Iridium
