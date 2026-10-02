#include "assets/environment/AssetEnvironmentPreparationService.h"

#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "core/EngineLog.h"

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
    std::filesystem::path assetRoot,
    std::shared_ptr<LocalDerivedDataCache> cache,
    CookTarget target,
    EngineLog* log)
    : assetRoot_(std::move(assetRoot)), cache_(std::move(cache)),
      target_(std::move(target)), importers_(createStandardAssetImporterRegistry()),
      log_(log) {
    if (assetRoot_.empty() || !cache_) {
        throw std::invalid_argument(
            "Catalog environment preparation requires an asset root and DDC.");
    }
    worker_ = std::jthread([this](std::stop_token token) { workerLoop(token); });
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
    std::lock_guard lock(mutex_);
    if (shutdown_) throw std::logic_error("Environment preparation is shut down.");
    if (!pending_.insert(record.guid).second) return false;
    requests_.push_back(record);
    if (log_) log_->info("Asset Cook",
        "Queued HDRI environment preparation: " + record.sourcePath);
    condition_.notify_all();
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
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

PreparedCatalogEnvironment AssetEnvironmentPreparationService::prepare(
    const AssetCatalogRecord& record, std::stop_token stopToken) {
    PreparedCatalogEnvironment result{ .assetGuid = record.guid };
    try {
        const auto sourcePath = assetRoot_ / record.sourcePath;
        const auto metadataPath = assetRoot_ / record.metadataPath;
        if (!isInsideRoot(assetRoot_, sourcePath) ||
            !isInsideRoot(assetRoot_, metadataPath)) {
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
            importers_, *cache_, assetRoot_, record.sourcePath,
            *metadata.metadata, target_, "reflection-resolution-v3",
            receiptDiagnostics);
        PreparedAssetCook prepared = warm
            ? std::move(*warm)
            : prepareAssetCook(importers_, assetRoot_, record.sourcePath,
                *metadata.metadata, target_, "reflection-resolution-v3", stopToken);
        if (!prepared.valid()) {
            throw std::runtime_error(failureMessage(
                "Environment cook preparation failed", prepared.diagnostics));
        }
        auto sharedPrepared = std::make_shared<PreparedAssetCook>(
            std::move(prepared));
        DdcRequestResult cooked = requestPreparedCook(
            *cache_, sharedPrepared, stopToken).get();
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
    return result;
}

void AssetEnvironmentPreparationService::workerLoop(
    std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        AssetCatalogRecord record;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stopToken,
                [this] { return shutdown_ || !requests_.empty(); });
            if (shutdown_ || stopToken.stop_requested()) return;
            record = std::move(requests_.front());
            requests_.pop_front();
        }
        PreparedCatalogEnvironment result = prepare(record, stopToken);
        {
            std::lock_guard lock(mutex_);
            pending_.erase(record.guid);
            if (!shutdown_) results_.push_back(std::move(result));
        }
    }
}

} // namespace Iridium
