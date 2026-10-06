#include "assets/environment/AssetEnvironmentPreparationService.h"

#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "core/EngineLog.h"
#include "core/tasks/TaskSystem.h"

#include <algorithm>
#include <stdexcept>

namespace Iridium {
namespace {

    std::string failureMessage(std::string prefix,
        const std::vector<CookDiagnostic>& diagnostics) {
        for (const CookDiagnostic& diagnostic : diagnostics) {
            if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                return prefix + ": " + diagnostic.code + " " + diagnostic.message;
            }
        }
        return prefix;
    }

    bool isInsideRoot(const std::filesystem::path& root,
        const std::filesystem::path& candidate) {
        std::error_code error;
        const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
        if (error) return false;
        const auto canonicalCandidate =
            std::filesystem::weakly_canonical(candidate, error);
        if (error) return false;
        const auto relative = std::filesystem::relative(
            canonicalCandidate, canonicalRoot, error);
        return !error && !relative.empty() &&
            !relative.generic_string().starts_with("..");
    }

    uint64_t residentBytes(const CookedEnvironmentProductData& product) {
        return static_cast<uint64_t>(product.radiance.size()) +
            product.irradiance.size() + product.prefilteredSpecular.size() +
            product.brdfLut.size();
    }

} // namespace

AssetEnvironmentPreparationService::AssetEnvironmentPreparationService(
    Tasks::TaskSystem& tasks,
    std::filesystem::path assetRoot,
    std::shared_ptr<LocalDerivedDataCache> cache,
    CookTarget target,
    EngineLog* log)
    : AssetEnvironmentPreparationService(tasks,
          std::vector<AssetRoot>{ AssetRoot{ "project", std::move(assetRoot) } },
          std::move(cache), std::move(target), log) {}

AssetEnvironmentPreparationService::AssetEnvironmentPreparationService(
    Tasks::TaskSystem& tasks,
    std::vector<AssetRoot> roots,
    std::shared_ptr<LocalDerivedDataCache> cache,
    CookTarget target,
    EngineLog* log)
    : roots_(std::move(roots)), cache_(std::move(cache)),
      target_(std::move(target)), importers_(createStandardAssetImporterRegistry()),
      log_(log) {
    if (roots_.empty() ||
        std::ranges::any_of(roots_,
            [](const AssetRoot& root) { return root.path.empty(); }) ||
        !cache_) {
        throw std::invalid_argument(
            "Catalog environment preparation requires an asset root and DDC.");
    }
    strand_ = std::make_unique<Tasks::FunctionStrand>(tasks,
        Tasks::TaskPriority::Background, "asset.environment.prepare");
}

AssetEnvironmentPreparationService::~AssetEnvironmentPreparationService() {
    shutdown();
}

bool AssetEnvironmentPreparationService::request(
    const AssetCatalogRecord& record) {
    if (record.guid.isNil() || record.parentGuid ||
        record.assetType != "iridium.environment" ||
        record.status != AssetCatalogStatus::Ready) {
        throw std::invalid_argument(
            "Catalog environment preparation requires one ready root environment.");
    }
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) throw std::logic_error("Environment preparation is shut down.");
        if (!pending_.insert(record.guid).second) return false;
        requests_.push_back(record);
        if (log_) log_->info("Asset Cook",
            "Queued HDRI environment preparation: " + record.sourcePath);
    }
    // One strand item per request, posted outside the mutex.
    (void)strand_->post([this] { runNext(); });
    return true;
}

std::vector<PreparedCatalogEnvironment>
AssetEnvironmentPreparationService::takeResults() {
    std::lock_guard lock(mutex_);
    std::vector<PreparedCatalogEnvironment> result;
    result.swap(results_);
    return result;
}

bool AssetEnvironmentPreparationService::pending(AssetGuid assetGuid) const {
    std::lock_guard lock(mutex_);
    return pending_.contains(assetGuid);
}

void AssetEnvironmentPreparationService::shutdown() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
        requests_.clear();
        pending_.clear();
    }
    stop_.request_stop();
    // Waits for the preparation in progress, then for the cooks in flight:
    // their continuations finish (a cancelled cook reports cancellation).
    strand_->waitIdle();
    std::unique_lock lock(mutex_);
    cooksIdle_.wait(lock, [this] { return cooksInFlight_ == 0; });
}

struct AssetEnvironmentPreparationService::CookState {
    AssetCatalogRecord record;
    bool usedReceipt = false;
    std::shared_ptr<PreparedAssetCook> prepared;
};

// Phase 1, on the strand. Returns null when `result` is already final.
std::shared_ptr<AssetEnvironmentPreparationService::CookState>
AssetEnvironmentPreparationService::prepareCook(
    const AssetCatalogRecord& record, std::stop_token stopToken,
    PreparedCatalogEnvironment& result) {
    try {
        const std::filesystem::path& recordRoot = assetRootPathFor(roots_, record);
        const auto sourcePath = recordRoot / record.sourcePath;
        const auto metadataPath = recordRoot / record.metadataPath;
        if (!isInsideRoot(recordRoot, sourcePath) ||
            !isInsideRoot(recordRoot, metadataPath)) {
            throw std::runtime_error("Environment paths escape the asset root.");
        }
        const AssetMetadataReadResult metadata = readAssetMetadata(metadataPath);
        if (!metadata.metadata || metadata.hasErrors() ||
            metadata.metadata->assetGuid != record.guid) {
            throw std::runtime_error(
                "Environment metadata is invalid or changed identity.");
        }
        std::vector<CookDiagnostic> receiptDiagnostics;
        std::optional<PreparedAssetCook> warm = tryPrepareAssetCookFromReceipt(
            importers_, *cache_, recordRoot, record.sourcePath,
            *metadata.metadata, target_, "reflection-resolution-v3",
            receiptDiagnostics);
        PreparedAssetCook prepared = warm
            ? std::move(*warm)
            : prepareAssetCook(importers_, recordRoot, record.sourcePath,
                *metadata.metadata, target_, "reflection-resolution-v3", stopToken);
        if (!prepared.valid()) {
            throw std::runtime_error(failureMessage(
                "Environment cook preparation failed", prepared.diagnostics));
        }
        auto sharedPrepared = std::make_shared<PreparedAssetCook>(
            std::move(prepared));
        auto state = std::make_shared<CookState>();
        state->record = record;
        state->usedReceipt = warm.has_value();
        state->prepared = std::move(sharedPrepared);
        return state;
    }
    catch (const std::exception& exception) {
        result.diagnostic = exception.what();
    }
    return nullptr;
}

// Phase 2, the cook's continuation (M7R R5b.2: it replaced the blocking
// future get).
void AssetEnvironmentPreparationService::finishCook(const CookState& state,
    const DdcRequestResult& cooked, PreparedCatalogEnvironment& result) {
    const AssetCatalogRecord& record = state.record;
    const bool warm = state.usedReceipt;
    const std::shared_ptr<PreparedAssetCook>& sharedPrepared = state.prepared;
    try {
        if ((cooked.status != DdcRequestStatus::Built &&
             cooked.status != DdcRequestStatus::CacheHit) || !cooked.blob) {
            throw std::runtime_error(failureMessage(
                "Environment cook failed", cooked.diagnostics));
        }
        if (!warm) {
            (void)storePreparedCookReceipt(
                *cache_, record.sourcePath, *sharedPrepared);
        }
        CookedArtifactReadResult artifact = readCookedArtifact(
            cooked.blob->bytes, cooked.blob->artifactHash);
        if (!artifact.valid() || artifact.artifact->assetGuid != record.guid) {
            throw std::runtime_error("Environment artifact validation failed.");
        }
        CookedEnvironmentReadResult product =
            readCookedEnvironmentProduct(*artifact.artifact);
        if (!product.valid()) {
            throw std::runtime_error(
                "Environment runtime product validation failed.");
        }
        result.gpuResidentBytes = residentBytes(*product.data);
        result.artifact = std::make_shared<CookedArtifact>(
            std::move(*artifact.artifact));
        result.product = std::make_shared<CookedEnvironmentProductData>(
            std::move(*product.data));
        result.succeeded = true;
    }
    catch (const std::exception& exception) {
        result.diagnostic = exception.what();
    }
}

void AssetEnvironmentPreparationService::runNext() {
    AssetCatalogRecord record;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_ || requests_.empty()) return;
        record = std::move(requests_.front());
        requests_.pop_front();
    }
    const std::stop_token stopToken = stop_.get_token();
    PreparedCatalogEnvironment result{ .assetGuid = record.guid };
    std::shared_ptr<CookState> state = prepareCook(record, stopToken, result);
    if (!state) {
        publish(record, std::move(result));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        ++cooksInFlight_;
    }
    const auto finished = [this, state](const DdcRequestResult& cooked) {
        PreparedCatalogEnvironment completed{ .assetGuid = state->record.guid };
        finishCook(*state, cooked, completed);
        publish(state->record, std::move(completed));
        std::lock_guard lock(mutex_);
        --cooksInFlight_;
        cooksIdle_.notify_all();
    };
    try {
        // The strand moves on; the cook (a DDC task) finishes this request
        // through its continuation.
        (void)requestPreparedCook(*cache_, state->prepared, stopToken, finished);
    }
    catch (const std::exception& exception) {
        finished(DdcRequestResult{
            .status = DdcRequestStatus::Failed,
            .diagnostics = { CookDiagnostic{
                .code = "ENVIRONMENT_COOK_REQUEST",
                .message = exception.what(),
            } },
        });
    }
}

void AssetEnvironmentPreparationService::publish(
    const AssetCatalogRecord& record, PreparedCatalogEnvironment result) {
    std::lock_guard lock(mutex_);
    pending_.erase(record.guid);
    if (!shutdown_) results_.push_back(std::move(result));
}

} // namespace Iridium
