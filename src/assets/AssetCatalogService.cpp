#include "assets/AssetCatalogService.h"
#include "core/EngineLog.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace Iridium {

    namespace {

        const char* jobKindName(
            AssetCatalogJobKind kind) {
            switch (kind) {
            case AssetCatalogJobKind::Import:
                return "Import";
            case AssetCatalogJobKind::Refresh:
                return "Refresh";
            case AssetCatalogJobKind::UpdateSettings:
                return "Update settings";
            case AssetCatalogJobKind::CreateFolder:
                return "Create folder";
            case AssetCatalogJobKind::RenameFolder:
                return "Rename folder";
            case AssetCatalogJobKind::DeleteFolder:
                return "Delete folder";
            case AssetCatalogJobKind::MoveAsset:
                return "Move asset";
            case AssetCatalogJobKind::RenameAsset:
                return "Rename asset";
            case AssetCatalogJobKind::DeleteAsset:
                return "Delete asset";
            }
            return "Asset operation";
        }

        std::string jobLabel(
            uint64_t serial,
            AssetCatalogJobKind kind) {
            return std::string(
                jobKindName(kind)) +
                " #" +
                std::to_string(serial);
        }

    } // namespace

    AssetCatalogService::AssetCatalogService(
        AssetCatalog* catalog,
        std::vector<AssetRoot> roots,
        EngineLog* log)
        : AssetCatalogService(
            catalog, std::move(roots),
            createStandardAssetImporterRegistry(),
            log) {}

    AssetCatalogService::AssetCatalogService(
        AssetCatalog* catalog,
        std::vector<AssetRoot> roots,
        ImporterRegistry importers,
        EngineLog* log)
        : catalog_(catalog),
          roots_(std::move(roots)),
          contentOperations_(roots_),
          importers_(std::move(importers)),
          log_(log) {
        if (!catalog_ || roots_.empty()) {
            throw std::invalid_argument(
                "Asset catalog service requires a catalog and asset roots.");
        }
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                workerLoop(stopToken);
            });
    }

    AssetCatalogService::~AssetCatalogService() {
        shutdown();
    }

    uint64_t AssetCatalogService::requestImport(
        std::filesystem::path sourcePath,
        std::string rootId,
        std::filesystem::path
            destinationDirectory) {
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot import after asset catalog service shutdown.");
        }
        if (serial_ == UINT64_MAX) {
            throw std::overflow_error(
                "Asset catalog job serial is exhausted.");
        }
        const uint64_t serial = ++serial_;
        jobs_.push_back({
            .serial = serial,
            .kind = AssetCatalogJobKind::Import,
            .sourcePath = std::move(sourcePath),
            .destinationPath =
                std::move(destinationDirectory),
            .rootId = std::move(rootId),
        });
        if (log_) {
            log_->info(
                "Asset Import",
                jobLabel(
                    serial,
                    AssetCatalogJobKind::Import) +
                    " queued: " +
                    jobs_.back()
                        .sourcePath
                        .generic_string());
        }
        condition_.notify_all();
        return serial;
    }

    uint64_t AssetCatalogService::requestRefresh() {
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot refresh after asset catalog service shutdown.");
        }
        const auto queued = std::ranges::find_if(
            jobs_, [](const Job& job) {
                return job.kind == AssetCatalogJobKind::Refresh;
            });
        if (queued != jobs_.end()) return queued->serial;
        if (serial_ == UINT64_MAX) {
            throw std::overflow_error(
                "Asset catalog job serial is exhausted.");
        }
        const uint64_t serial = ++serial_;
        jobs_.push_back({
            .serial = serial,
            .kind = AssetCatalogJobKind::Refresh,
        });
        condition_.notify_all();
        return serial;
    }

    uint64_t AssetCatalogService::requestUpdateSettings(
        AssetGuid assetGuid, nlohmann::json settings) {
        if (assetGuid.isNil() || !settings.is_object()) {
            throw std::invalid_argument(
                "Settings update requires a root GUID and settings object.");
        }
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot update settings after asset catalog service shutdown.");
        }
        if (serial_ == UINT64_MAX) {
            throw std::overflow_error(
                "Asset catalog job serial is exhausted.");
        }
        const uint64_t serial = ++serial_;
        jobs_.push_back({
            .serial = serial,
            .kind = AssetCatalogJobKind::UpdateSettings,
            .assetGuid = assetGuid,
            .settings = std::move(settings),
        });
        condition_.notify_all();
        return serial;
    }

    uint64_t AssetCatalogService::enqueue(
        Job job) {
        std::lock_guard lock(mutex_);
        if (shutdown_) {
            throw std::logic_error(
                "Cannot mutate project content after asset catalog service shutdown.");
        }
        if (serial_ == UINT64_MAX) {
            throw std::overflow_error(
                "Asset catalog job serial is exhausted.");
        }
        job.serial = ++serial_;
        jobs_.push_back(std::move(job));
        condition_.notify_all();
        return serial_;
    }

    uint64_t AssetCatalogService::requestCreateFolder(
        std::string rootId,
        std::filesystem::path parentDirectory,
        std::string name) {
        return enqueue({
            .kind = AssetCatalogJobKind::CreateFolder,
            .sourcePath = std::move(parentDirectory),
            .rootId = std::move(rootId),
            .name = std::move(name),
        });
    }

    uint64_t AssetCatalogService::requestRenameFolder(
        std::string rootId,
        std::filesystem::path directory,
        std::string name) {
        return enqueue({
            .kind = AssetCatalogJobKind::RenameFolder,
            .sourcePath = std::move(directory),
            .rootId = std::move(rootId),
            .name = std::move(name),
        });
    }

    uint64_t AssetCatalogService::requestDeleteFolder(
        std::string rootId,
        std::filesystem::path directory) {
        return enqueue({
            .kind = AssetCatalogJobKind::DeleteFolder,
            .sourcePath = std::move(directory),
            .rootId = std::move(rootId),
        });
    }

    uint64_t AssetCatalogService::requestMoveAsset(
        AssetGuid assetGuid,
        std::filesystem::path destinationDirectory) {
        return enqueue({
            .kind = AssetCatalogJobKind::MoveAsset,
            .destinationPath =
                std::move(destinationDirectory),
            .assetGuid = assetGuid,
        });
    }

    uint64_t AssetCatalogService::requestRenameAsset(
        AssetGuid assetGuid,
        std::string name) {
        return enqueue({
            .kind = AssetCatalogJobKind::RenameAsset,
            .name = std::move(name),
            .assetGuid = assetGuid,
        });
    }

    uint64_t AssetCatalogService::requestDeleteAsset(
        AssetGuid assetGuid) {
        return enqueue({
            .kind = AssetCatalogJobKind::DeleteAsset,
            .assetGuid = assetGuid,
        });
    }

    std::vector<AssetCatalogJobResult>
        AssetCatalogService::takeResults() {
        std::lock_guard lock(mutex_);
        std::vector<AssetCatalogJobResult> result;
        result.swap(results_);
        return result;
    }

    bool AssetCatalogService::busy() const {
        std::lock_guard lock(mutex_);
        return active_ || !jobs_.empty();
    }

    void AssetCatalogService::shutdown() noexcept {
        std::vector<Job> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            cancelled.assign(
                std::make_move_iterator(
                    jobs_.begin()),
                std::make_move_iterator(
                    jobs_.end()));
            jobs_.clear();
        }
        worker_.request_stop();
        condition_.notify_all();
        if (log_) {
            for (const Job& job :
                cancelled) {
                log_->warning(
                    "Asset Import",
                    jobLabel(
                        job.serial,
                        job.kind) +
                        " cancelled during editor shutdown.");
            }
        }
        if (worker_.joinable()) worker_.join();
    }

    bool AssetCatalogService::sourceBelongsToRoot(
        const std::filesystem::path& source) const {
        std::error_code error;
        const auto canonicalSource =
            std::filesystem::weakly_canonical(source, error);
        if (error) return false;
        for (const AssetRoot& root : roots_) {
            const auto canonicalRoot =
                std::filesystem::weakly_canonical(root.path, error);
            if (error) {
                error.clear();
                continue;
            }
            const auto relative = std::filesystem::relative(
                canonicalSource, canonicalRoot, error);
            if (!error && !relative.empty() &&
                !relative.generic_string().starts_with("..")) {
                return true;
            }
            error.clear();
        }
        return false;
    }

    AssetCatalogRecord
        AssetCatalogService::readyRootRecord(
            AssetGuid guid) const {
        const std::vector<AssetCatalogRecord>
            records = catalog_->recordsForGuid(guid);
        const auto found = std::ranges::find_if(
            records,
            [](const AssetCatalogRecord& record) {
                return !record.parentGuid &&
                    record.status ==
                        AssetCatalogStatus::Ready;
            });
        if (found == records.end()) {
            throw std::runtime_error(
                "Project mutation target is not a ready root asset.");
        }
        return *found;
    }

    AssetCatalogJobResult AssetCatalogService::execute(
        const Job& job,
        std::stop_token stopToken) {
        AssetCatalogJobResult result{
            .serial = job.serial,
            .kind = job.kind,
            .sourcePath = job.sourcePath,
        };
        std::optional<std::filesystem::path>
            copiedPackage;
        try {
            if (job.kind == AssetCatalogJobKind::Import) {
                const bool alreadyInProject =
                    sourceBelongsToRoot(
                        job.sourcePath);
                const AssetContentMutationResult
                    staged =
                        contentOperations_
                            .importAsset(
                                job.rootId.empty()
                                    ? "project"
                                    : job.rootId,
                                job.destinationPath,
                                job.sourcePath,
                                stopToken);
                if (!staged.succeeded()) {
                    throw std::runtime_error(
                        staged.diagnostic);
                }
                if (!alreadyInProject) {
                    copiedPackage =
                        staged.path
                            .parent_path();
                    if (log_) {
                        log_->info(
                            "Asset Import",
                            jobLabel(
                                job.serial,
                                job.kind) +
                                " copied source package into the project; discovering metadata.");
                    }
                }
                const AssetImportResult imported =
                    importAssetSource(
                        staged.path,
                        importers_,
                        stopToken);
                result.sourcePath = imported.sourcePath;
                result.assetGuid = imported.metadata.assetGuid;
            }
            else if (job.kind ==
                AssetCatalogJobKind::UpdateSettings) {
                const std::vector<AssetCatalogRecord>
                    records =
                        catalog_->recordsForGuid(
                            job.assetGuid);
                const auto rootRecord =
                    std::ranges::find_if(
                        records,
                        [](const AssetCatalogRecord&
                            record) {
                            return !record.parentGuid &&
                                record.status ==
                                    AssetCatalogStatus::Ready;
                        });
                if (rootRecord == records.end()) {
                    throw std::runtime_error(
                        "Settings target is not a ready root asset.");
                }
                const auto assetRoot =
                    std::ranges::find_if(
                        roots_,
                        [&rootRecord](
                            const AssetRoot& root) {
                            return root.id ==
                                rootRecord->assetRoot;
                        });
                if (assetRoot == roots_.end()) {
                    throw std::runtime_error(
                        "Settings target uses an unknown asset root.");
                }
                const std::filesystem::path
                    sourcePath =
                        assetRoot->path /
                        rootRecord->sourcePath;
                const AssetImportResult imported =
                    updateAssetImportSettings(
                        sourcePath, importers_,
                        job.settings,
                        stopToken);
                result.sourcePath =
                    imported.sourcePath;
                result.assetGuid =
                    imported.metadata.assetGuid;
            }
            else if (job.kind ==
                AssetCatalogJobKind::CreateFolder) {
                const AssetContentMutationResult mutation =
                    contentOperations_.createFolder(
                        job.rootId, job.sourcePath,
                        job.name);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath = mutation.path;
            }
            else if (job.kind ==
                AssetCatalogJobKind::RenameFolder) {
                const AssetContentMutationResult mutation =
                    contentOperations_.renameFolder(
                        job.rootId, job.sourcePath,
                        job.name);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath = mutation.path;
            }
            else if (job.kind ==
                AssetCatalogJobKind::DeleteFolder) {
                const AssetContentMutationResult mutation =
                    contentOperations_.deleteFolder(
                        job.rootId, job.sourcePath);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath =
                    mutation.previousPath;
            }
            else if (job.kind ==
                AssetCatalogJobKind::MoveAsset) {
                const AssetCatalogRecord record =
                    readyRootRecord(job.assetGuid);
                const AssetContentMutationResult mutation =
                    contentOperations_.moveAsset(
                        record,
                        job.destinationPath);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath = mutation.path;
                result.assetGuid = record.guid;
            }
            else if (job.kind ==
                AssetCatalogJobKind::RenameAsset) {
                const AssetCatalogRecord record =
                    readyRootRecord(job.assetGuid);
                const AssetContentMutationResult mutation =
                    contentOperations_.renameAsset(
                        record, job.name);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath = mutation.path;
                result.assetGuid = record.guid;
            }
            else if (job.kind ==
                AssetCatalogJobKind::DeleteAsset) {
                const AssetCatalogRecord record =
                    readyRootRecord(job.assetGuid);
                const AssetContentMutationResult mutation =
                    contentOperations_.deleteAsset(
                        record);
                if (!mutation.succeeded()) {
                    throw std::runtime_error(
                        mutation.diagnostic);
                }
                result.sourcePath =
                    mutation.previousPath;
                result.assetGuid = record.guid;
            }
            const AssetDiscoveryResult discovery =
                discoverAssetRoots(roots_);
            catalog_->rebuild(
                discovery.records,
                discovery.sourceDirectories);
            result.recordCount = discovery.records.size();
            if (discovery.hasErrors()) {
                result.diagnostic =
                    "Catalog rebuilt with discovery errors; inspect asset status.";
            }
            result.succeeded = true;
        }
        catch (const std::exception& exception) {
            result.cancelled =
                stopToken.stop_requested();
            result.diagnostic = exception.what();
            if (copiedPackage &&
                !result.assetGuid) {
                std::error_code ignored;
                std::filesystem::remove_all(
                    *copiedPackage, ignored);
            }
        }
        return result;
    }

    void AssetCatalogService::workerLoop(
        std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this, &stopToken] {
                    return shutdown_ ||
                        stopToken.stop_requested() ||
                        !jobs_.empty();
                });
                if (shutdown_ || stopToken.stop_requested()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                active_ = true;
            }
            if (log_) {
                log_->info(
                    job.kind ==
                            AssetCatalogJobKind::Import
                        ? "Asset Import"
                        : "Asset Catalog",
                    jobLabel(
                        job.serial,
                        job.kind) +
                        " started.");
            }
            AssetCatalogJobResult result =
                execute(job, stopToken);
            if (log_) {
                const std::string category =
                    job.kind ==
                            AssetCatalogJobKind::Import
                        ? "Asset Import"
                        : "Asset Catalog";
                std::string message =
                    jobLabel(
                        job.serial,
                        job.kind);
                if (result.cancelled) {
                    log_->warning(
                        category,
                        message +
                            " cancelled.");
                }
                else if (!result.succeeded) {
                    log_->error(
                        category,
                        message +
                            " failed: " +
                            result.diagnostic);
                }
                else {
                    message +=
                        " completed";
                    if (!result.sourcePath
                            .empty()) {
                        message += ": " +
                            result.sourcePath
                                .generic_string();
                    }
                    if (!result.diagnostic
                            .empty()) {
                        message +=
                            " (" +
                            result.diagnostic +
                            ')';
                    }
                    log_->info(
                        category,
                        std::move(message));
                }
            }
            {
                std::lock_guard lock(mutex_);
                active_ = false;
                results_.push_back(std::move(result));
            }
        }
    }

} // namespace Iridium
