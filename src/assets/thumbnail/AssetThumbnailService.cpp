#include "assets/thumbnail/AssetThumbnailService.h"
#include "core/EngineLog.h"

#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/environment/EnvironmentProduct.h"

#include <algorithm>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace Iridium {

    namespace {

        std::string cookFailure(
            std::string prefix,
            const std::vector<CookDiagnostic>& diagnostics) {
            for (const CookDiagnostic& diagnostic :
                diagnostics) {
                if (diagnostic.severity ==
                    CookDiagnosticSeverity::Error) {
                    return prefix + ": " +
                        diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            return prefix;
        }

        bool supportedRecord(
            const AssetCatalogRecord& record) {
            return record.status ==
                    AssetCatalogStatus::Ready &&
                (record.assetType ==
                    "iridium.model" ||
                 record.assetType ==
                    "iridium.model-primitive" ||
                 record.assetType ==
                    "iridium.material" ||
                 record.assetType ==
                    "iridium.texture" ||
                 record.assetType ==
                    "iridium.environment");
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

    AssetThumbnailService::AssetThumbnailService(
        std::filesystem::path assetRoot,
        std::filesystem::path ddcRoot,
        CookTarget target,
        EngineLog* log)
        : AssetThumbnailService(
            std::move(assetRoot),
            std::make_shared<
                LocalDerivedDataCache>(
                    std::move(ddcRoot)),
            std::move(target),
            log) {}

    AssetThumbnailService::AssetThumbnailService(
        std::filesystem::path assetRoot,
        std::shared_ptr<LocalDerivedDataCache> cache,
        CookTarget target,
        EngineLog* log)
        : assetRoot_(std::move(assetRoot)),
          cache_(std::move(cache)),
          target_(std::move(target)),
          importers_(
              createStandardAssetImporterRegistry()),
          log_(log) {
        if (assetRoot_.empty() || !cache_) {
            throw std::invalid_argument(
                "Asset thumbnail service requires an asset root and DDC.");
        }
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                workerLoop(stopToken);
            });
    }

    AssetThumbnailService::~AssetThumbnailService() {
        shutdown();
    }

    std::map<AssetGuid,
        AssetThumbnailService::Job>
        AssetThumbnailService::groupDemand(
            std::span<const AssetCatalogRecord>
                records) {
        std::map<AssetGuid, Job> grouped;
        for (const AssetCatalogRecord& record :
            records) {
            if (!supportedRecord(record)) continue;
            const AssetGuid rootGuid =
                record.parentGuid.value_or(
                    record.guid);
            Job& job = grouped[rootGuid];
            if (job.rootAssetGuid.isNil()) {
                job.rootAssetGuid = rootGuid;
                job.rootRecord = record;
                job.rootRecord.guid = rootGuid;
                job.rootRecord.parentGuid.reset();
                if (record.parentGuid) {
                    job.rootRecord.assetType =
                        "iridium.model";
                    job.rootRecord.sourceKey.clear();
                }
            }
            job.records.push_back(record);
        }
        return grouped;
    }

    void AssetThumbnailService::setDemand(
        std::span<const AssetCatalogRecord>
            visibleRecords) {
        auto grouped =
            groupDemand(visibleRecords);
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        visibleDemandByRoot_ =
            std::move(grouped);
        rebuildDemandLocked();
    }

    void AssetThumbnailService::
        setPinnedDemand(
            std::span<const
                AssetCatalogRecord> records) {
        auto grouped =
            groupDemand(records);
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        pinnedDemandByRoot_ =
            std::move(grouped);
        rebuildDemandLocked();
    }

    void AssetThumbnailService::
        setDetailDemand(
            std::span<const
                AssetCatalogRecord> sourceRecords,
            std::optional<AssetGuid>
                selectedAsset) {
        std::lock_guard lock(mutex_);
        if (shutdown_) return;
        if (detailAsset_ == selectedAsset) {
            return;
        }
        jobs_.erase(
            std::remove_if(
                jobs_.begin(), jobs_.end(),
                [](const Job& job) {
                    return job.detail;
                }),
            jobs_.end());
        detailDemand_.reset();
        detailAsset_ = selectedAsset;
        if (!selectedAsset ||
            selectedAsset->isNil()) {
            condition_.notify_all();
            return;
        }
        auto grouped =
            groupDemand(sourceRecords);
        for (auto& [rootGuid, job] :
            grouped) {
            (void)rootGuid;
            const auto selected =
                std::ranges::find_if(
                    job.records,
                    [selectedAsset](
                        const AssetCatalogRecord&
                            record) {
                        return record.guid ==
                            *selectedAsset;
                    });
            if (selected ==
                    job.records.end()) {
                continue;
            }
            const AssetCatalogRecord record =
                *selected;
            job.records.clear();
            job.records.push_back(record);
            job.extent =
                kAssetDetailThumbnailExtent;
            job.detail = true;
            detailDemand_ = job;
            jobs_.push_front(
                std::move(job));
            ++stats_.rootsQueued;
            break;
        }
        stats_.queuedRoots =
            static_cast<uint32_t>(
                jobs_.size());
        condition_.notify_all();
    }

    void AssetThumbnailService::
        rebuildDemandLocked() {
        demandByRoot_ =
            visibleDemandByRoot_;
        for (const auto& [rootGuid, job] :
            pinnedDemandByRoot_) {
            demandByRoot_.insert_or_assign(
                rootGuid, job);
        }
        demandedAssets_.clear();
        for (const auto& [rootGuid, job] :
            demandByRoot_) {
            (void)rootGuid;
            for (const AssetCatalogRecord& record :
                job.records) {
                demandedAssets_.insert(
                    record.guid);
                rootByAsset_[record.guid] =
                    job.rootAssetGuid;
                if (!completedAssets_.contains(
                        record.guid)) {
                    info_[record.guid] = {
                        .status =
                            AssetThumbnailStatus::Pending,
                    };
                }
            }
        }

        for (auto job = jobs_.begin();
            job != jobs_.end();) {
            if (job->detail) {
                if (!detailDemand_ ||
                    !detailAsset_ ||
                    job->records.empty() ||
                    job->records.front().guid !=
                        *detailAsset_) {
                    job = jobs_.erase(job);
                }
                else {
                    ++job;
                }
                continue;
            }
            if (!demandByRoot_.contains(
                    job->rootAssetGuid)) {
                ++stats_.rootsCancelled;
                job = jobs_.erase(job);
            }
            else {
                job = jobs_.erase(job);
            }
        }
        queueMissingLocked(demandByRoot_);
        stats_.queuedRoots =
            static_cast<uint32_t>(jobs_.size());
        stats_.demandedAssets =
            static_cast<uint32_t>(
                demandedAssets_.size());
        stats_.active =
            activeRoot_.has_value();
        condition_.notify_all();
    }

    void AssetThumbnailService::invalidate(
        AssetGuid rootAssetGuid) {
        std::lock_guard lock(mutex_);
        const auto demanded =
            demandByRoot_.find(rootAssetGuid);
        if (demanded ==
            demandByRoot_.end()) {
            return;
        }
        for (const AssetCatalogRecord& record :
            demanded->second.records) {
            completedAssets_.erase(
                record.guid);
            info_[record.guid] = {
                .status =
                    AssetThumbnailStatus::Pending,
            };
        }
        std::map<AssetGuid, Job> grouped;
        grouped.emplace(
            rootAssetGuid,
            demanded->second);
        queueMissingLocked(
            std::move(grouped));
        if (detailDemand_ &&
            detailDemand_->rootAssetGuid ==
                rootAssetGuid) {
            jobs_.erase(
                std::remove_if(
                    jobs_.begin(), jobs_.end(),
                    [](const Job& job) {
                        return job.detail;
                    }),
                jobs_.end());
            jobs_.push_front(
                *detailDemand_);
        }
        stats_.queuedRoots =
            static_cast<uint32_t>(jobs_.size());
        condition_.notify_all();
    }

    void AssetThumbnailService::markPublished(
        AssetGuid assetGuid) {
        std::lock_guard lock(mutex_);
        info_[assetGuid] = {
            .status =
                AssetThumbnailStatus::Ready,
        };
    }

    void AssetThumbnailService::markEvicted(
        AssetGuid assetGuid) {
        std::lock_guard lock(mutex_);
        completedAssets_.erase(assetGuid);
        info_.erase(assetGuid);
        if (!demandedAssets_.contains(
                assetGuid)) {
            return;
        }
        for (const auto& [rootGuid, job] :
            demandByRoot_) {
            if (std::ranges::any_of(
                    job.records,
                    [assetGuid](
                        const AssetCatalogRecord&
                            record) {
                        return record.guid ==
                            assetGuid;
                    })) {
                info_[assetGuid] = {
                    .status =
                        AssetThumbnailStatus::Pending,
                };
                std::map<AssetGuid, Job>
                    grouped;
                grouped.emplace(rootGuid, job);
                queueMissingLocked(
                    std::move(grouped));
                condition_.notify_all();
                break;
            }
        }
    }

    void AssetThumbnailService::reportFailure(
        AssetGuid assetGuid,
        std::string diagnostic) {
        if (assetGuid.isNil() ||
            diagnostic.empty()) {
            throw std::invalid_argument(
                "Thumbnail failure requires a GUID and diagnostic.");
        }
        std::lock_guard lock(mutex_);
        completedAssets_.insert(assetGuid);
        info_[assetGuid] = {
            .status =
                AssetThumbnailStatus::Failed,
            .diagnostic =
                std::move(diagnostic),
        };
        ++stats_.thumbnailsFailed;
    }

    bool AssetThumbnailService::isDemanded(
        AssetGuid assetGuid) const {
        std::lock_guard lock(mutex_);
        return demandedAssets_.contains(
            assetGuid);
    }

    bool AssetThumbnailService::
        isDetailDemanded(
            AssetGuid assetGuid) const {
        std::lock_guard lock(mutex_);
        return detailAsset_ ==
            std::optional(assetGuid);
    }

    AssetThumbnailInfo AssetThumbnailService::info(
        AssetGuid assetGuid) const {
        std::lock_guard lock(mutex_);
        const auto found =
            info_.find(assetGuid);
        return found != info_.end()
            ? found->second
            : AssetThumbnailInfo{};
    }

    std::vector<AssetThumbnailInfo>
        AssetThumbnailService::info(
            std::span<const AssetGuid>
                assetGuids) const {
        std::lock_guard lock(mutex_);
        std::vector<AssetThumbnailInfo>
            result;
        result.reserve(
            assetGuids.size());
        for (const AssetGuid guid :
            assetGuids) {
            const auto found =
                info_.find(guid);
            result.push_back(
                found != info_.end()
                ? found->second
                : AssetThumbnailInfo{});
        }
        return result;
    }

    AssetThumbnailSourceDetail
        AssetThumbnailService::sourceDetail(
            AssetGuid assetGuid) const {
        std::lock_guard lock(mutex_);
        const auto root =
            rootByAsset_.find(assetGuid);
        if (root ==
            rootByAsset_.end()) {
            return {};
        }
        const auto detail =
            detailByRoot_.find(
                root->second);
        return detail !=
            detailByRoot_.end()
            ? detail->second
            : AssetThumbnailSourceDetail{};
    }

    std::vector<PreparedAssetThumbnailBatch>
        AssetThumbnailService::takeResults() {
        std::lock_guard lock(mutex_);
        std::vector<PreparedAssetThumbnailBatch>
            result;
        result.swap(results_);
        return result;
    }

    AssetThumbnailServiceStats
        AssetThumbnailService::stats() const {
        std::lock_guard lock(mutex_);
        AssetThumbnailServiceStats result =
            stats_;
        result.queuedRoots =
            static_cast<uint32_t>(jobs_.size());
        result.demandedAssets =
            static_cast<uint32_t>(
                demandedAssets_.size());
        result.active =
            activeRoot_.has_value();
        return result;
    }

    void AssetThumbnailService::shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            jobs_.clear();
            demandByRoot_.clear();
            visibleDemandByRoot_.clear();
            pinnedDemandByRoot_.clear();
            demandedAssets_.clear();
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    PreparedAssetThumbnailBatch
        AssetThumbnailService::prepare(
            const Job& job,
            std::stop_token stopToken) {
        PreparedAssetThumbnailBatch result{
            .rootAssetGuid =
                job.rootAssetGuid,
        };
        try {
            const std::filesystem::path
                sourcePath =
                    assetRoot_ /
                    job.rootRecord.sourcePath;
            const std::filesystem::path
                metadataPath =
                    assetRoot_ /
                    job.rootRecord.metadataPath;
            if (!isInsideRoot(
                    assetRoot_, sourcePath) ||
                !isInsideRoot(
                    assetRoot_, metadataPath)) {
                throw std::runtime_error(
                    "Thumbnail source paths escape the registered asset root.");
            }
            const AssetMetadataReadResult metadata =
                readAssetMetadata(
                    metadataPath);
            if (!metadata.metadata ||
                metadata.hasErrors() ||
                metadata.metadata->assetGuid !=
                    job.rootAssetGuid) {
                throw std::runtime_error(
                    "Thumbnail metadata is invalid or changed identity.");
            }
            result.settingsJson =
                metadata.metadata->settings.dump(2);
            std::vector<CookDiagnostic>
                receiptDiagnostics;
            std::optional<PreparedAssetCook>
                warmPrepared =
                    tryPrepareAssetCookFromReceipt(
                        importers_, *cache_,
                        assetRoot_,
                        job.rootRecord.sourcePath,
                        *metadata.metadata,
                        target_,
                        "m3.6-browser-model-v4",
                        receiptDiagnostics);
            const bool usedReceipt =
                warmPrepared.has_value();
            if (!usedReceipt &&
                job.rootRecord.assetType ==
                    "iridium.model") {
                result.deferredReason =
                    "Preview will be generated after the model's first editor cook.";
                return result;
            }
            PreparedAssetCook prepared =
                usedReceipt
                ? std::move(*warmPrepared)
                : prepareAssetCook(
                    importers_, assetRoot_,
                    job.rootRecord.sourcePath,
                    *metadata.metadata,
                    target_,
                    "m3.6-browser-model-v4",
                    stopToken);
            if (!prepared.valid()) {
                throw std::runtime_error(
                    cookFailure(
                        "Thumbnail cook preparation failed",
                        prepared.diagnostics));
            }
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Thumbnail preparation was cancelled.");
            }
            auto sharedPrepared =
                std::make_shared<PreparedAssetCook>(
                    std::move(prepared));
            DdcRequestResult cooked =
                requestPreparedCook(
                    *cache_, sharedPrepared,
                    stopToken).get();
            if ((cooked.status !=
                    DdcRequestStatus::Built &&
                 cooked.status !=
                    DdcRequestStatus::CacheHit) ||
                !cooked.blob) {
                throw std::runtime_error(
                    cookFailure(
                        "Thumbnail cook failed",
                        cooked.diagnostics));
            }
            if (!usedReceipt) {
                (void)storePreparedCookReceipt(
                    *cache_,
                    job.rootRecord.sourcePath,
                    *sharedPrepared);
            }
            CookedArtifactReadResult artifact =
                readCookedArtifact(
                    cooked.blob->bytes,
                    cooked.blob->artifactHash);
            if (!artifact.valid() ||
                artifact.artifact->assetGuid !=
                    job.rootAssetGuid) {
                throw std::runtime_error(
                    "Thumbnail artifact validation failed.");
            }
            result.dependencies =
                artifact.artifact->dependencies;
            result.thumbnails.reserve(
                job.records.size());
            if (artifact.artifact->artifactType ==
                "iridium.model") {
                CookedModelReadResult product =
                    readCookedModelProduct(
                        *artifact.artifact);
                if (!product.valid()) {
                    throw std::runtime_error(
                        "Thumbnail model product validation failed.");
                }
                for (const CookedModelMaterial&
                        material :
                    product.data->materials) {
                    result.associations.push_back({
                        .parentGuid =
                            job.rootAssetGuid,
                        .childGuid =
                            material.materialGuid,
                    });
                    std::set<AssetGuid>
                        textures;
                    for (const CookedModelTextureBinding&
                            binding :
                        material.textureBindings) {
                        if (textures.insert(
                                binding.textureGuid)
                                .second) {
                            result.associations.push_back({
                                .parentGuid =
                                    material.materialGuid,
                                .childGuid =
                                    binding.textureGuid,
                            });
                        }
                    }
                }
                std::ranges::sort(
                    result.associations,
                    [](const AssetThumbnailAssociation&
                            lhs,
                        const AssetThumbnailAssociation&
                            rhs) {
                        if (lhs.parentGuid !=
                            rhs.parentGuid) {
                            return lhs.parentGuid <
                                rhs.parentGuid;
                        }
                        return lhs.childGuid <
                            rhs.childGuid;
                    });
                result.transparencyDetails.reserve(
                    product.data->materials.size() +
                    product.data->manifest.primitives.size());
                for (const CookedModelMaterial& material :
                        product.data->materials) {
                    result.transparencyDetails.push_back({
                        .assetGuid = material.materialGuid,
                        .policy = material.compiled.transparency,
                        .runtimePrimitiveCount = static_cast<uint32_t>(
                            std::ranges::count_if(
                                product.data->manifest.primitives,
                                [&material](const CookedModelPrimitive& primitive) {
                                    return primitive.materialGuid ==
                                        material.materialGuid;
                                })),
                    });
                }
                std::map<AssetGuid, size_t> sourcePrimitiveDetails;
                for (const CookedModelPrimitive& primitive :
                        product.data->manifest.primitives) {
                    const auto [found, inserted] =
                        sourcePrimitiveDetails.emplace(
                            primitive.sourcePrimitiveGuid,
                            result.transparencyDetails.size());
                    if (inserted) {
                        result.transparencyDetails.push_back({
                            .assetGuid = primitive.sourcePrimitiveGuid,
                            .policy = primitive.transparency,
                            .runtimePrimitiveCount = 1,
                        });
                    }
                    else {
                        AssetThumbnailTransparencyDetail& detail =
                            result.transparencyDetails[found->second];
                        ++detail.runtimePrimitiveCount;
                        detail.uniformPolicy = detail.uniformPolicy &&
                            detail.policy == primitive.transparency;
                    }
                }
                for (const AssetCatalogRecord& record :
                    job.records) {
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Thumbnail preparation was cancelled.");
                    }
                    result.thumbnails.push_back(
                        makeAssetThumbnail(
                            *product.data, record,
                            job.extent));
                    result.thumbnails.back().purpose =
                        job.detail
                        ? AssetThumbnailPurpose::Detail
                        : AssetThumbnailPurpose::Browser;
                }
            }
            else if (artifact.artifact->artifactType ==
                "iridium.texture") {
                const auto manifestSection =
                    std::ranges::find_if(
                        artifact.artifact->sections,
                        [](const CookSection& section) {
                            return section.id ==
                                kCookedTextureManifestSection;
                        });
                const auto payloadSection =
                    std::ranges::find_if(
                        artifact.artifact->sections,
                        [](const CookSection& section) {
                            return section.id ==
                                kCookedTexturePayloadSection;
                        });
                if (manifestSection ==
                        artifact.artifact->sections.end() ||
                    payloadSection ==
                        artifact.artifact->sections.end()) {
                    throw std::runtime_error(
                        "Thumbnail texture product sections are missing.");
                }
                std::vector<CookDiagnostic>
                    textureDiagnostics;
                const std::optional<
                    CookedTextureManifest> manifest =
                        readTextureManifest(
                            manifestSection->bytes,
                            textureDiagnostics);
                if (!manifest ||
                    hasCookErrors(textureDiagnostics) ||
                    hasCookErrors(
                        validateTextureProduct(
                            *manifest,
                            payloadSection->bytes
                                .size()))) {
                    throw std::runtime_error(
                        "Thumbnail texture product validation failed.");
                }
                for (const AssetCatalogRecord& record :
                    job.records) {
                    result.thumbnails.push_back(
                        makeCookedTextureThumbnail(
                            record.guid,
                            *manifest,
                            payloadSection->bytes,
                            job.extent));
                    result.thumbnails.back().purpose =
                        job.detail
                        ? AssetThumbnailPurpose::Detail
                        : AssetThumbnailPurpose::Browser;
                }
            }
            else if (artifact.artifact->artifactType ==
                "iridium.environment") {
                CookedEnvironmentReadResult product =
                    readCookedEnvironmentProduct(*artifact.artifact);
                if (!product.valid()) {
                    throw std::runtime_error(
                        "Thumbnail environment product validation failed.");
                }
                for (const AssetCatalogRecord& record : job.records) {
                    result.thumbnails.push_back(
                        makeCookedEnvironmentThumbnail(
                            record.guid, *product.data, job.extent));
                    result.thumbnails.back().purpose = job.detail
                        ? AssetThumbnailPurpose::Detail
                        : AssetThumbnailPurpose::Browser;
                }
            }
            else {
                throw std::runtime_error(
                    "Cooked artifact type has no thumbnail producer.");
            }
        }
        catch (const std::exception& exception) {
            result.diagnostic =
                exception.what();
        }
        return result;
    }

    void AssetThumbnailService::queueMissingLocked(
        std::map<AssetGuid, Job> grouped) {
        for (auto& [rootGuid, job] : grouped) {
            if (activeRoot_ == rootGuid) {
                continue;
            }
            if (std::ranges::any_of(
                    jobs_,
                    [rootGuid](const Job& queued) {
                        return queued.rootAssetGuid ==
                            rootGuid;
                    })) {
                continue;
            }
            job.records.erase(
                std::remove_if(
                    job.records.begin(),
                    job.records.end(),
                    [this](
                        const AssetCatalogRecord&
                            record) {
                        return completedAssets_
                            .contains(record.guid);
                    }),
                job.records.end());
            if (job.records.empty()) continue;
            jobs_.push_back(std::move(job));
            ++stats_.rootsQueued;
        }
    }

    void AssetThumbnailService::workerLoop(
        std::stop_token stopToken) {
#if defined(_WIN32)
        // Thumbnail rasterization is opportunistic editor work. Large selected
        // models must not compete at normal priority with the render/UI thread.
        (void)SetThreadPriority(
            GetCurrentThread(),
            THREAD_PRIORITY_BELOW_NORMAL);
#endif
        while (!stopToken.stop_requested()) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(
                    lock, stopToken,
                    [this] {
                        return shutdown_ ||
                            !jobs_.empty();
                    });
                if (shutdown_ ||
                    stopToken.stop_requested()) {
                    return;
                }
                job = std::move(
                    jobs_.front());
                jobs_.pop_front();
                activeRoot_ =
                    job.rootAssetGuid;
            }
            if (log_) {
                log_->info(
                    "Asset Thumbnail",
                    std::string(
                        job.detail
                        ? "Preparing selected preview: "
                        : "Preparing thumbnails: ") +
                        job.rootRecord
                            .sourcePath);
            }
            PreparedAssetThumbnailBatch result =
                prepare(job, stopToken);
            if (log_) {
                const std::string source =
                    job.rootRecord.sourcePath;
                if (stopToken.stop_requested()) {
                    log_->warning(
                        "Asset Thumbnail",
                        "Thumbnail preparation cancelled: " +
                            source);
                }
                else if (!result.diagnostic.empty()) {
                    log_->error(
                        "Asset Thumbnail",
                        "Thumbnail preparation failed for " +
                            source + ": " +
                            result.diagnostic);
                }
                else if (!result.deferredReason.empty()) {
                    log_->info(
                        "Asset Thumbnail",
                        "Thumbnail deferred for " +
                            source + ": " +
                            result.deferredReason);
                }
                else {
                    log_->info(
                        "Asset Thumbnail",
                        std::string(
                            job.detail
                            ? "Selected preview ready: "
                            : "Thumbnails ready: ") +
                            source);
                }
            }
            {
                std::lock_guard lock(mutex_);
                activeRoot_.reset();
                if (shutdown_) return;
                if (!result.deferredReason.empty() &&
                    !job.detail) {
                    for (const AssetCatalogRecord&
                        record : job.records) {
                        if (!demandedAssets_.contains(
                                record.guid)) {
                            continue;
                        }
                        completedAssets_.insert(
                            record.guid);
                        info_[record.guid] = {
                            .status =
                                AssetThumbnailStatus::Unavailable,
                            .diagnostic =
                                result.deferredReason,
                        };
                    }
                }
                auto thumbnail =
                    result.thumbnails.begin();
                while (thumbnail !=
                    result.thumbnails.end()) {
                    const bool demanded =
                        job.detail
                        ? detailAsset_ ==
                            std::optional(
                                thumbnail->assetGuid)
                        : demandedAssets_.contains(
                            thumbnail->assetGuid);
                    if (!demanded) {
                        thumbnail =
                            result.thumbnails.erase(
                                thumbnail);
                        continue;
                    }
                    if (!job.detail) {
                        completedAssets_.insert(
                            thumbnail->assetGuid);
                    }
                    if (thumbnail->valid()) {
                        ++stats_.thumbnailsProduced;
                        if (!job.detail) {
                            info_[thumbnail->assetGuid] = {
                                .status =
                                    AssetThumbnailStatus::Prepared,
                            };
                        }
                    }
                    else {
                        ++stats_.thumbnailsFailed;
                        if (!job.detail) {
                            info_[thumbnail->assetGuid] = {
                                .status =
                                    AssetThumbnailStatus::Failed,
                                .diagnostic =
                                    thumbnail->diagnostic,
                            };
                        }
                    }
                    ++thumbnail;
                }
                if (!result.diagnostic.empty() &&
                    !job.detail) {
                    for (const AssetCatalogRecord&
                        record : job.records) {
                        if (demandedAssets_.contains(
                                record.guid)) {
                            completedAssets_.insert(
                                record.guid);
                            ++stats_.thumbnailsFailed;
                            info_[record.guid] = {
                                .status =
                                    AssetThumbnailStatus::Failed,
                                .diagnostic =
                                    result.diagnostic,
                            };
                        }
                    }
                }
                detailByRoot_[
                    job.rootAssetGuid] = {
                    .available =
                        result.diagnostic.empty(),
                    .settingsJson =
                        result.settingsJson,
                    .dependencies =
                        result.dependencies,
                    .associations =
                        result.associations,
                    .transparencyDetails =
                        result.transparencyDetails,
                    .diagnostic =
                        !result.diagnostic.empty()
                        ? result.diagnostic
                        : result.deferredReason,
                };
                if (!result.thumbnails.empty() ||
                    !result.diagnostic.empty()) {
                    results_.push_back(
                        std::move(result));
                }
                const auto demand =
                    demandByRoot_.find(
                        job.rootAssetGuid);
                if (demand !=
                    demandByRoot_.end()) {
                    std::map<AssetGuid, Job>
                        grouped;
                    grouped.emplace(
                        demand->first,
                        demand->second);
                    queueMissingLocked(
                        std::move(grouped));
                }
                stats_.queuedRoots =
                    static_cast<uint32_t>(
                        jobs_.size());
                stats_.active = false;
            }
        }
    }

} // namespace Iridium
