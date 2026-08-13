#include "assets/model/AssetModelPreparationService.h"

#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "core/EngineLog.h"
#include "renderer/rhi/Mesh.h"

#include <chrono>
#include <stdexcept>

namespace Iridium {

    namespace {

        std::pair<uint64_t, uint64_t> residentBytes(
            const CookedModelProductData& product) {
            uint64_t gpuBytes =
                product.vertices.size() * sizeof(Vertex) +
                product.indices.size() * sizeof(uint32_t) +
                product.materials.size() * sizeof(PackedGpuMaterial);
            for (const CookedModelTextureView& view :
                product.textureViews) {
                gpuBytes += view.payload.size();
            }
            const uint64_t cpuBytes =
                sizeof(ModelAsset) +
                product.manifest.primitives.size() * sizeof(SubMesh) +
                product.materials.size() * sizeof(MaterialBinding);
            return { cpuBytes, gpuBytes };
        }

        std::string diagnosticsMessage(
            std::string prefix,
            const std::vector<CookDiagnostic>& diagnostics) {
            for (const CookDiagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == CookDiagnosticSeverity::Error) {
                    return prefix + ": " + diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            return prefix;
        }

        bool isInsideRoot(
            const std::filesystem::path& root,
            const std::filesystem::path& candidate) {
            std::error_code error;
            const std::filesystem::path
                canonicalRoot =
                    std::filesystem::weakly_canonical(
                        root, error);
            if (error) return false;
            const std::filesystem::path
                canonicalCandidate =
                    std::filesystem::weakly_canonical(
                        candidate, error);
            if (error) return false;
            const std::filesystem::path relative =
                std::filesystem::relative(
                    canonicalCandidate,
                    canonicalRoot, error);
            return !error &&
                !relative.empty() &&
                !relative.generic_string()
                    .starts_with("..");
        }

    } // namespace

    AssetModelPreparationService::AssetModelPreparationService(
        std::filesystem::path assetRoot,
        std::filesystem::path ddcRoot,
        CookTarget target,
        EngineLog* log)
        : AssetModelPreparationService(
            std::move(assetRoot),
            std::make_shared<
                LocalDerivedDataCache>(
                    std::move(ddcRoot)),
            std::move(target),
            log) {}

    AssetModelPreparationService::AssetModelPreparationService(
        std::filesystem::path assetRoot,
        std::shared_ptr<LocalDerivedDataCache> cache,
        CookTarget target,
        EngineLog* log)
        : assetRoot_(std::move(assetRoot)),
          cache_(std::move(cache)),
          target_(std::move(target)),
          importers_(createStandardAssetImporterRegistry()),
          log_(log) {
        if (assetRoot_.empty() || !cache_) {
            throw std::invalid_argument(
                "Catalog model preparation requires an asset root and DDC.");
        }
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                workerLoop(stopToken);
            });
    }

    AssetModelPreparationService::~AssetModelPreparationService() {
        shutdown();
    }

    bool AssetModelPreparationService::request(
        const AssetCatalogRecord& record) {
        if (record.guid.isNil() || record.parentGuid ||
            record.assetType != "iridium.model" ||
            record.status != AssetCatalogStatus::Ready) {
            throw std::invalid_argument(
                "Catalog model preparation requires one ready root model.");
        }
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot prepare a model after shutdown.");
        }
        if (!pending_.insert(record.guid).second) return false;
        requests_.push_back(record);
        if (log_) {
            log_->info(
                "Asset Cook",
                "Queued model preparation: " +
                    record.sourcePath);
        }
        condition_.notify_all();
        return true;
    }

    std::vector<PreparedCatalogModel>
        AssetModelPreparationService::takeResults() {
        std::lock_guard lock(mutex_);
        std::vector<PreparedCatalogModel> result;
        result.swap(results_);
        return result;
    }

    bool AssetModelPreparationService::pending(
        AssetGuid assetGuid) const {
        std::lock_guard lock(mutex_);
        return pending_.contains(assetGuid);
    }

    void AssetModelPreparationService::shutdown() noexcept {
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

    PreparedCatalogModel AssetModelPreparationService::prepare(
        const AssetCatalogRecord& record,
        std::stop_token stopToken) {
        PreparedCatalogModel result{
            .assetGuid = record.guid,
        };
        try {
            const std::filesystem::path
                sourcePath =
                    assetRoot_ /
                    record.sourcePath;
            const std::filesystem::path
                metadataPath =
                    assetRoot_ /
                    record.metadataPath;
            if (!isInsideRoot(
                    assetRoot_, sourcePath) ||
                !isInsideRoot(
                    assetRoot_, metadataPath)) {
                throw std::runtime_error(
                    "Catalog model paths escape the registered asset root.");
            }
            const AssetMetadataReadResult metadata =
                readAssetMetadata(metadataPath);
            if (!metadata.metadata || metadata.hasErrors() ||
                metadata.metadata->assetGuid != record.guid) {
                throw std::runtime_error(
                    "Catalog model metadata is invalid or changed identity.");
            }
            std::vector<CookDiagnostic>
                receiptDiagnostics;
            std::optional<PreparedAssetCook>
                warmPrepared =
                    tryPrepareAssetCookFromReceipt(
                        importers_, *cache_, assetRoot_,
                        record.sourcePath,
                        *metadata.metadata, target_,
                        "m3.6-browser-model-v4",
                        receiptDiagnostics);
            const bool usedReceipt =
                warmPrepared.has_value();
            if (log_) {
                log_->info(
                    "Asset Cook",
                    usedReceipt
                    ? "Using cached model cook: " +
                        record.sourcePath
                    : "Parsing model source: " +
                        record.sourcePath);
            }
            PreparedAssetCook prepared =
                usedReceipt
                ? std::move(*warmPrepared)
                : prepareAssetCook(
                    importers_, assetRoot_,
                    record.sourcePath,
                    *metadata.metadata, target_,
                    "m3.6-browser-model-v4",
                    stopToken);
            if (!prepared.valid()) {
                throw std::runtime_error(diagnosticsMessage(
                    "Catalog model cook preparation failed",
                    prepared.diagnostics));
            }
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Catalog model preparation was cancelled.");
            }
            auto sharedPrepared =
                std::make_shared<PreparedAssetCook>(
                    std::move(prepared));
            if (log_ && !usedReceipt) {
                const size_t imageSourceCount =
                    sharedPrepared->source
                        .subassetPayloads.size();
                log_->info(
                    "Asset Cook",
                    target_.profile == "editor"
                    ? "Cooking full-resolution editor model (" +
                        std::to_string(
                            imageSourceCount) +
                        " image sources): " +
                        record.sourcePath
                    : "Cooking model artifact: " +
                        record.sourcePath);
            }
            std::shared_future<DdcRequestResult>
                cookFuture =
                    requestPreparedCook(
                        *cache_, sharedPrepared,
                        stopToken);
            const auto waitStart =
                std::chrono::steady_clock::now();
            auto nextProgress =
                waitStart +
                std::chrono::seconds(5);
            while (cookFuture.wait_for(
                    std::chrono::milliseconds(
                        250)) !=
                std::future_status::ready) {
                if (log_ &&
                    std::chrono::steady_clock::
                        now() >= nextProgress) {
                    const auto elapsed =
                        std::chrono::
                            duration_cast<
                                std::chrono::
                                    seconds>(
                                std::chrono::
                                    steady_clock::
                                    now() -
                                waitStart)
                                .count();
                    log_->info(
                        "Asset Cook",
                        std::string(
                            usedReceipt
                            ? "Loading cached model"
                            : "Model cook is still active") +
                            " (" +
                            std::to_string(
                                elapsed) +
                            " s): " +
                            record.sourcePath);
                    nextProgress +=
                        std::chrono::seconds(5);
                }
            }
            DdcRequestResult cooked =
                cookFuture.get();
            if ((cooked.status != DdcRequestStatus::Built &&
                 cooked.status != DdcRequestStatus::CacheHit) ||
                !cooked.blob) {
                throw std::runtime_error(diagnosticsMessage(
                    "Catalog model cook failed", cooked.diagnostics));
            }
            if (!usedReceipt) {
                const std::vector<CookDiagnostic>
                    receiptWarnings =
                        storePreparedCookReceipt(
                            *cache_,
                            record.sourcePath,
                            *sharedPrepared);
                if (log_ &&
                    !receiptWarnings.empty()) {
                    log_->warning(
                        "Asset Cook",
                        "Model cooked, but its warm-cache receipt could not be stored: " +
                            record.sourcePath);
                }
            }
            CookedArtifactReadResult artifact = readCookedArtifact(
                cooked.blob->bytes, cooked.blob->artifactHash);
            if (!artifact.valid() ||
                artifact.artifact->assetGuid != record.guid) {
                throw std::runtime_error(
                    "Catalog model artifact validation failed.");
            }
            CookedModelReadResult product =
                readCookedModelProduct(*artifact.artifact);
            if (!product.valid()) {
                throw std::runtime_error(
                    "Catalog model runtime product validation failed.");
            }
            const auto [cpuBytes, gpuBytes] =
                residentBytes(*product.data);
            result.artifact =
                std::make_shared<CookedArtifact>(
                    std::move(*artifact.artifact));
            result.product =
                std::make_shared<CookedModelProductData>(
                    std::move(*product.data));
            result.cpuResidentBytes = cpuBytes;
            result.gpuResidentBytes = gpuBytes;
            result.succeeded = true;
        }
        catch (const std::exception& exception) {
            result.diagnostic = exception.what();
        }
        return result;
    }

    void AssetModelPreparationService::workerLoop(
        std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            AssetCatalogRecord record;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stopToken, [this] {
                    return shutdown_ || !requests_.empty();
                });
                if (shutdown_ || stopToken.stop_requested()) return;
                record = std::move(requests_.front());
                requests_.pop_front();
            }
            if (log_) {
                log_->info(
                    "Asset Cook",
                    "Preparing model: " +
                        record.sourcePath);
            }
            PreparedCatalogModel result =
                prepare(record, stopToken);
            if (log_) {
                if (stopToken.stop_requested()) {
                    log_->warning(
                        "Asset Cook",
                        "Model preparation cancelled: " +
                            record.sourcePath);
                }
                else if (!result.succeeded) {
                    log_->error(
                        "Asset Cook",
                        "Model preparation failed for " +
                            record.sourcePath +
                            ": " +
                            result.diagnostic);
                }
                else {
                    log_->info(
                        "Asset Cook",
                        "Model preparation completed: " +
                            record.sourcePath);
                }
            }
            {
                std::lock_guard lock(mutex_);
                pending_.erase(record.guid);
                results_.push_back(std::move(result));
            }
        }
    }

} // namespace Iridium
