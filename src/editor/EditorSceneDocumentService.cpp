#include "editor/EditorSceneDocumentService.h"

#include "scene/authoring/SourceSceneCapture.h"
#include "scene/authoring/SourceSceneEnvelopeMigrator.h"
#include "scene/authoring/SourceSceneLoadTransaction.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] bool readFile(const std::filesystem::path& path,
            std::string& bytes, std::string& diagnostic) {
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                diagnostic = "Could not open scene file: " + path.string();
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(input), {});
            if (!input.eof() && input.fail()) {
                diagnostic = "Could not read complete scene file: " + path.string();
                return false;
            }
            return true;
        }

        [[nodiscard]] std::string firstError(
            const std::vector<SceneDiagnostic>& diagnostics,
            std::string fallback) {
            const auto found = std::ranges::find_if(diagnostics,
                [](const SceneDiagnostic& value) {
                    return value.severity == SceneDiagnosticSeverity::Error;
                });
            return found == diagnostics.end() ? std::move(fallback) :
                found->code + ": " + found->message;
        }

        [[nodiscard]] bool canonicalScenePath(
            const std::filesystem::path& path) {
            return path.filename().string().ends_with(".iridium.scene.json");
        }

        [[nodiscard]] bool validSceneMetadata(
            const AssetMetadataReadResult& result) {
            return result.metadata && !result.hasErrors() &&
                result.metadata->assetType == "iridium.scene" &&
                !result.metadata->assetGuid.isNil();
        }

    } // namespace

    EditorSceneDocumentService::EditorSceneDocumentService(SceneWorld& world)
        : world_(world), registries_(createCoreSceneRegistryBundle()) {
        document_.name = "Untitled";
        if (!registries_) {
            setOperationFailure(ScenePhase::Transaction,
                "scene.document.registry_initialization",
                registries_.diagnostic.empty()
                    ? "Could not initialize core scene component registries"
                    : registries_.diagnostic);
        }
    }

    void EditorSceneDocumentService::restorePreservedEntities(
        std::span<const SourceSceneEntity> entities) {
        for (const SourceSceneEntity& entity : entities) {
            const auto found = std::ranges::find_if(document_.entities,
                [&entity](const SourceSceneEntity& current) {
                    return current.uuid == entity.uuid;
                });
            if (found == document_.entities.end()) {
                document_.entities.push_back(entity);
            }
            else {
                *found = entity;
            }
        }
    }

    bool EditorSceneDocumentService::open(const std::filesystem::path& path) {
        return openCandidate(path, path, false);
    }

    bool EditorSceneDocumentService::recoverBackup(
        const std::filesystem::path& primaryPath) {
        std::filesystem::path backup = primaryPath;
        backup += ".bak";
        return openCandidate(backup, primaryPath, true);
    }

    bool EditorSceneDocumentService::recoverTemporary(
        const std::filesystem::path& primaryPath,
        const std::filesystem::path& temporaryPath) {
        const auto candidates = findOrphanedSceneTemporaries(primaryPath);
        const std::filesystem::path normalized = temporaryPath.lexically_normal();
        const bool listed = std::ranges::any_of(candidates,
            [&](const OrphanedSceneTemporary& candidate) {
                return candidate.path.lexically_normal() == normalized;
            });
        if (!listed) {
            setOperationFailure(ScenePhase::Read,
                "scene.document.temporary_not_listed",
                "Recovery temporary is not a recognized sibling candidate");
            return false;
        }
        if (!openCandidate(normalized, primaryPath, true)) return false;
        operationDiagnostic_ =
            "Recovered crash temporary; save to restore the primary scene";
        return true;
    }

    bool EditorSceneDocumentService::openCandidate(
        const std::filesystem::path& loadPath,
        const std::filesystem::path& adoptedPath,
        bool recovered) {
        diagnostics_.clear();
        operationDiagnostic_.clear();
        if (!registries_) {
            setOperationFailure(ScenePhase::Transaction,
                "scene.document.not_ready",
                "Scene document service is not initialized");
            return false;
        }
        const std::filesystem::path sidecar = assetMetadataSidecarPath(adoptedPath);
        const AssetMetadataReadResult metadata = readAssetMetadata(sidecar);
        if (!validSceneMetadata(metadata)) {
            setOperationFailure(ScenePhase::Read,
                "scene.document.sidecar_invalid",
                "Scene source requires a valid iridium.scene metadata sidecar: " +
                    sidecar.string());
            return false;
        }
        std::string bytes;
        if (!readFile(loadPath, bytes, operationDiagnostic_)) {
            setOperationFailure(ScenePhase::Read, "scene.document.read_failed",
                operationDiagnostic_);
            return false;
        }
        SourceSceneReadResult read = readSourceSceneSchema1(bytes,
            registries_.runtime, registries_.source);
        bool migrated = false;
        if (!read) {
            SourceSceneEnvelopeMigrationResult migration = migrateSourceSceneV0(
                bytes, metadata.metadata->assetGuid.bytes());
            if (migration) {
                const std::string migratedBytes = migration.value->dump();
                read = readSourceSceneSchema1(migratedBytes,
                    registries_.runtime, registries_.source);
                read.diagnostics.insert(read.diagnostics.begin(),
                    migration.diagnostics.begin(), migration.diagnostics.end());
                migrated = static_cast<bool>(read);
            }
        }
        diagnostics_ = read.diagnostics;
        if (!read) {
            operationDiagnostic_ = firstError(diagnostics_,
                "Scene source validation failed");
            return false;
        }
        SourceSceneStageResult staged = stageSourceScene(
            std::move(*read.document), registries_.runtime, registries_.source);
        diagnostics_.insert(diagnostics_.end(), staged.diagnostics.begin(),
            staged.diagnostics.end());
        sortSceneDiagnostics(diagnostics_);
        if (!staged) {
            operationDiagnostic_ = firstError(diagnostics_,
                "Scene staging failed");
            return false;
        }

        // No document/path/token state changes occur before this atomic world swap.
        SourceSceneDocument adoptedDocument = staged.staging->document;
        commitStagedSourceScene(world_, *staged.staging);
        document_ = std::move(adoptedDocument);
        sceneAssetGuid_ = metadata.metadata->assetGuid;
        currentPath_ = adoptedPath.lexically_normal();
        currentState_ = nextState_++;
        requiresSaveAs_ = migrated || !canonicalScenePath(currentPath_);
        savedState_ = (recovered || requiresSaveAs_) ? 0 : currentState_;
        if (historyObserver_) {
            historyObserver_->onSceneDocumentCommitted(currentState_);
        }
        operationDiagnostic_ = recovered
            ? "Recovered last-known-good backup; save to restore the primary scene"
            : migrated
                ? "Migrated legacy scene in memory; Save As to adopt schema 1"
            : "Opened " + currentPath_.filename().string();
        return true;
    }

    bool EditorSceneDocumentService::save(AtomicSceneSaveOptions options) {
        if (currentPath_.empty() || requiresSaveAs_) {
            setOperationFailure(ScenePhase::Save, "scene.document.path_missing",
                "Save As is required before saving this document");
            return false;
        }
        return saveTo(currentPath_, options);
    }

    bool EditorSceneDocumentService::saveAs(const std::filesystem::path& path,
        AtomicSceneSaveOptions options) {
        if (path.empty()) {
            setOperationFailure(ScenePhase::Save, "scene.document.path_missing",
                "Choose a scene path before saving");
            return false;
        }
        if (!canonicalScenePath(path)) {
            setOperationFailure(ScenePhase::Save,
                "scene.document.noncanonical_suffix",
                "Source scenes must use the .iridium.scene.json suffix");
            return false;
        }
        return saveTo(path, options);
    }

    bool EditorSceneDocumentService::saveTo(const std::filesystem::path& path,
        AtomicSceneSaveOptions options) {
        diagnostics_.clear();
        operationDiagnostic_.clear();
        if (!registries_) {
            setOperationFailure(ScenePhase::Transaction,
                "scene.document.not_ready",
                "Scene document service is not initialized");
            return false;
        }
        SourceSceneCaptureResult captured = captureSourceScene(world_, document_,
            registries_.runtime, registries_.source);
        diagnostics_ = captured.diagnostics;
        if (!captured) {
            operationDiagnostic_ = firstError(diagnostics_,
                "Live scene capture failed");
            return false;
        }
        SourceSceneWriteResult written = writeSourceSceneCanonical(
            *captured.document, registries_.runtime, registries_.source);
        diagnostics_.insert(diagnostics_.end(), written.diagnostics.begin(),
            written.diagnostics.end());
        sortSceneDiagnostics(diagnostics_);
        if (!written) {
            operationDiagnostic_ = firstError(diagnostics_,
                "Canonical scene serialization failed");
            return false;
        }

        const SourceSceneFileVerifier verifier = [this](
            std::string_view bytes, std::string& diagnostic) {
            const SourceSceneReadResult verified = readSourceSceneSchema1(bytes,
                registries_.runtime, registries_.source);
            if (verified) return true;
            diagnostic = firstError(verified.diagnostics,
                "Written scene failed semantic verification");
            return false;
        };
        const std::filesystem::path normalized = path.lexically_normal();
        const bool newDestination = currentPath_.empty() ||
            normalized != currentPath_.lexically_normal() || requiresSaveAs_;
        AssetGuid destinationGuid = sceneAssetGuid_;
        std::filesystem::path sidecar;
        bool createdSidecar = false;
        if (newDestination) {
            destinationGuid = createAssetGuidV7();
            sidecar = assetMetadataSidecarPath(normalized);
            std::error_code existsError;
            if (std::filesystem::exists(sidecar, existsError) || existsError) {
                setOperationFailure(ScenePhase::Save,
                    "scene.document.sidecar_exists",
                    "Save As will not overwrite an existing scene metadata sidecar");
                return false;
            }
            AssetMetadata metadata;
            metadata.assetGuid = destinationGuid;
            metadata.assetType = "iridium.scene";
            metadata.importerId = "iridium.scene";
            metadata.importerVersion = 1;
            metadata.settingsSchemaVersion = 1;
            std::string sidecarError;
            if (!writeAssetMetadataAtomic(sidecar, metadata, sidecarError)) {
                setOperationFailure(ScenePhase::Save,
                    "scene.document.sidecar_write_failed", sidecarError);
                return false;
            }
            createdSidecar = true;
        }

        const AtomicSceneSaveResult saved = saveSourceSceneAtomically(normalized,
            *written.bytes, verifier, options);
        if (!saved) {
            if (createdSidecar) {
                std::error_code ignored;
                std::filesystem::remove(sidecar, ignored);
            }
            setOperationFailure(ScenePhase::Save,
                "scene.document.atomic_save_failed", saved.diagnostic);
            return false;
        }

        // Adopt the new path/document checkpoint only after filesystem adoption.
        document_ = std::move(*captured.document);
        currentPath_ = normalized;
        sceneAssetGuid_ = destinationGuid;
        requiresSaveAs_ = false;
        savedState_ = currentState_;
        operationDiagnostic_ = "Saved " + currentPath_.filename().string();
        return true;
    }

    SceneDocumentStateToken EditorSceneDocumentService::advanceState() {
        currentState_ = nextState_++;
        return currentState_;
    }

    void EditorSceneDocumentService::adoptState(
        SceneDocumentStateToken token) noexcept {
        currentState_ = token;
        nextState_ = (std::max)(nextState_, token + 1);
    }

    std::filesystem::path EditorSceneDocumentService::backupPath() const {
        std::filesystem::path backup = currentPath_;
        if (!backup.empty()) backup += ".bak";
        return backup;
    }

    void EditorSceneDocumentService::setOperationFailure(ScenePhase phase,
        std::string code, std::string message) {
        operationDiagnostic_ = message;
        diagnostics_.push_back({
            .severity = SceneDiagnosticSeverity::Error,
            .code = std::move(code),
            .phase = phase,
            .message = std::move(message),
        });
        sortSceneDiagnostics(diagnostics_);
    }

} // namespace Iridium
