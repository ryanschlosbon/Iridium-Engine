#include "assets/model/AssetModelPreparationService.h"

#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "core/EngineLog.h"
#include "core/tasks/TaskSystem.h"
#include "renderer/rhi/Mesh.h"

#include <algorithm>
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
            uint64_t lodLevelCount = 0;
            uint64_t lodChildCount = 0;
            for (const CookedModelLodChain& chain : product.lodChains) {
                lodLevelCount += chain.levels.size();
                lodChildCount += chain.levels.empty() ? 0u : chain.levels.size() - 1u;
            }
            const uint64_t cpuBytes =
                sizeof(ModelAsset) +
                (product.manifest.primitives.size() - lodChildCount) * sizeof(SubMesh) +
                product.lodChains.size() * sizeof(ModelLodChain) +
                lodLevelCount * sizeof(ModelLodLevel) +
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
        Tasks::TaskSystem& tasks,
        std::filesystem::path assetRoot,
        std::filesystem::path ddcRoot,
        CookTarget target,
        EngineLog* log)
        : AssetModelPreparationService(
            tasks,
            std::move(assetRoot),
            std::make_shared<
                LocalDerivedDataCache>(
                    std::move(ddcRoot), tasks),
            std::move(target),
            log) {}

    AssetModelPreparationService::AssetModelPreparationService(
        Tasks::TaskSystem& tasks,
        std::filesystem::path assetRoot,
        std::shared_ptr<LocalDerivedDataCache> cache,
        CookTarget target,
        EngineLog* log)
        : AssetModelPreparationService(
            tasks,
            std::vector<AssetRoot>{
                AssetRoot{ "project", std::move(assetRoot) },
            },
            std::move(cache),
            std::move(target),
            log) {}

    AssetModelPreparationService::AssetModelPreparationService(
        Tasks::TaskSystem& tasks,
        std::vector<AssetRoot> roots,
        std::shared_ptr<LocalDerivedDataCache> cache,
        CookTarget target,
        EngineLog* log)
        : roots_(std::move(roots)),
          cache_(std::move(cache)),
          target_(std::move(target)),
          importers_(createStandardAssetImporterRegistry()),
          log_(log) {
        if (roots_.empty() ||
            std::ranges::any_of(roots_, [](const AssetRoot& root) {
                return root.path.empty();
            }) ||
            !cache_) {
            throw std::invalid_argument(
                "Catalog model preparation requires an asset root and DDC.");
        }
        strand_ = std::make_unique<Tasks::FunctionStrand>(tasks,
            Tasks::TaskPriority::Background, "asset.model.prepare");
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
        {
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
        }
        // One strand item per request, posted outside the mutex.
        (void)strand_->post([this] { runNext(); });
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
        stop_.request_stop();
        // Waits for the preparation in progress, then for the cooks in flight:
        // their continuations finish (a cancelled cook reports cancellation).
        strand_->waitIdle();
        std::unique_lock lock(mutex_);
        cooksIdle_.wait(lock, [this] { return cooksInFlight_ == 0; });
    }

    struct AssetModelPreparationService::CookState {
        AssetCatalogRecord record;
        bool usedReceipt = false;
        std::shared_ptr<PreparedAssetCook> prepared;
    };

    // Phase 1, on the strand: metadata, the warm receipt or the source parse,
    // and the cook preparation. Returns null when `result` is already final.
    std::shared_ptr<AssetModelPreparationService::CookState>
        AssetModelPreparationService::prepareCook(
            const AssetCatalogRecord& record,
            std::stop_token stopToken,
            PreparedCatalogModel& result) {
        try {
            const std::filesystem::path& recordRoot =
                assetRootPathFor(roots_, record);
            const std::filesystem::path
                sourcePath =
                    recordRoot /
                    record.sourcePath;
            const std::filesystem::path
                metadataPath =
                    recordRoot /
                    record.metadataPath;
            if (!isInsideRoot(
                    recordRoot, sourcePath) ||
                !isInsideRoot(
                    recordRoot, metadataPath)) {
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
                        importers_, *cache_, recordRoot,
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
            const auto preparationStart =
                std::chrono::steady_clock::now();
            PreparedAssetCook prepared =
                usedReceipt
                ? std::move(*warmPrepared)
                : prepareAssetCook(
                    importers_, recordRoot,
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
            const auto preparationMilliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    preparationStart).count();
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
            if (log_) {
                log_->info(
                    "Asset Cook",
                    "Model preparation complete in " +
                        std::to_string(preparationMilliseconds) +
                        " ms; cook key " +
                        sharedPrepared->cookKey + ": " +
                        record.sourcePath);
            }
            const auto cookProgressStart =
                std::chrono::steady_clock::now();
            sharedPrepared->context.progress =
                [log = log_, sourcePath = record.sourcePath,
                    cookProgressStart](
                    const AssetCookContext::Progress& progress) {
                    if (!log) return;
                    const auto elapsedMilliseconds =
                        std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() -
                            cookProgressStart).count();
                    std::string message = "[" + progress.stage + "] ";
                    if (progress.total != 0) {
                        message += std::to_string(progress.completed) +
                            "/" + std::to_string(progress.total) + " ";
                    }
                    message += progress.detail + " (" +
                        std::to_string(elapsedMilliseconds) +
                        " ms): " + sourcePath;
                    log->info("Asset Cook", std::move(message));
                };
            auto state = std::make_shared<CookState>();
            state->record = record;
            state->usedReceipt = usedReceipt;
            state->prepared = std::move(sharedPrepared);
            return state;
        }
        catch (const std::exception& exception) {
            result.diagnostic = exception.what();
        }
        return nullptr;
    }

    // Phase 2, the cook's continuation (M7R R5b.2: it replaced the 250 ms
    // future poll): receipt, artifact and runtime product validation.
    void AssetModelPreparationService::finishCook(
        const CookState& state,
        const DdcRequestResult& cooked,
        PreparedCatalogModel& result) {
        const AssetCatalogRecord& record = state.record;
        const bool usedReceipt = state.usedReceipt;
        const std::shared_ptr<PreparedAssetCook>& sharedPrepared =
            state.prepared;
        try {
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
    }

    void AssetModelPreparationService::runNext() {
        AssetCatalogRecord record;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_ || requests_.empty()) return;
            record = std::move(requests_.front());
            requests_.pop_front();
        }
        const std::stop_token stopToken = stop_.get_token();
        if (log_) {
            log_->info(
                "Asset Cook",
                "Preparing model: " +
                    record.sourcePath);
        }
        PreparedCatalogModel result{
            .assetGuid = record.guid,
        };
        std::shared_ptr<CookState> state =
            prepareCook(record, stopToken, result);
        if (!state) {
            publish(record, std::move(result), stopToken);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            ++cooksInFlight_;
        }
        const auto finished = [this, state, stopToken](
            const DdcRequestResult& cooked) {
            PreparedCatalogModel completed{
                .assetGuid = state->record.guid,
            };
            finishCook(*state, cooked, completed);
            publish(state->record, std::move(completed), stopToken);
            std::lock_guard lock(mutex_);
            --cooksInFlight_;
            cooksIdle_.notify_all();
        };
        try {
            // The strand moves on to the next request; the cook (a DDC task)
            // finishes this one through its continuation.
            (void)requestPreparedCook(
                *cache_, state->prepared, stopToken, finished);
        }
        catch (const std::exception& exception) {
            finished(DdcRequestResult{
                .status = DdcRequestStatus::Failed,
                .diagnostics = { CookDiagnostic{
                    .code = "MODEL_COOK_REQUEST",
                    .message = exception.what(),
                } },
            });
        }
    }

    void AssetModelPreparationService::publish(
        const AssetCatalogRecord& record,
        PreparedCatalogModel result,
        std::stop_token stopToken) {
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
        std::lock_guard lock(mutex_);
        pending_.erase(record.guid);
        results_.push_back(std::move(result));
    }

} // namespace Iridium
