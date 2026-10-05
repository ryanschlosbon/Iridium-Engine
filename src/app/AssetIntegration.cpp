// M7R R5a.1: code moved from Application.cpp (design section 3.4); see
// AssetIntegration.h.
#include "app/AssetIntegration.h"

#include "core/tasks/TaskSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "core/BuildInfo.h"
#include "core/EngineLog.h"
#include "profiling/CpuProfiler.h"
#include "assets/AssetDiscovery.h"
#include "assets/AssetMetadata.h"
#include "assets/SqliteAssetCatalog.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookKey.h"
#include "assets/cooker/CookReceipt.h"
#include "assets/cooker/LocalDerivedDataCache.h"
#include "assets/cooker/TextFixtureImporter.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "assets/environment/EnvironmentProduct.h"
#include "assets/model/GltfModelImporter.h"
#include "assets/texture/TextureImporter.h"
#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorSceneDocumentService.h"
#include "renderer/rhi/EditorRenderBridge.h"
#include "renderer/rhi/IRenderBackend.h"
#include "renderer/rhi/Mesh.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/SkyComponent.h"
#include "utils/Sha256.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace Iridium {

    namespace {

        constexpr uint64_t
            EditorRuntimeUploadBudgetBytes =
                128ull * 1024ull * 1024ull;
        // Model publication is still atomic today, but the per-frame admission
        // budget is not a product-size limit. Permit one oversized model when
        // the queue is otherwise idle while retaining a high-end-editor safety
        // cap until progressive geometry/texture residency lands.
        constexpr uint64_t EditorModelPublicationLimitBytes =
            1024ull * 1024ull * 1024ull;
        // One atomic Cinematic 2048/2048 environment is about 512 MiB. Keep a
        // bounded margin for product metadata without silently allowing
        // arbitrary oversized model/texture publications.
        constexpr uint64_t EditorEnvironmentPublicationLimitBytes =
            640ull * 1024ull * 1024ull;
        struct ModelSourceReimportContext {
            std::filesystem::path assetRoot;
            std::filesystem::path sourceRelativePath;
            std::filesystem::path metadataPath;
            ImporterRegistry importers;
            std::shared_ptr<LocalDerivedDataCache>
                cache;
            CookTarget target;
        };

        std::string cookFailureMessage(
            std::string_view prefix,
            std::span<const CookDiagnostic>
                diagnostics) {
            std::string message(prefix);
            for (const CookDiagnostic& diagnostic :
                diagnostics) {
                if (diagnostic.severity ==
                    CookDiagnosticSeverity::Error) {
                    message += ": " +
                        diagnostic.code + " " +
                        diagnostic.message;
                }
            }
            return message;
        }

        bool writeBinaryAtomic(const std::filesystem::path& destination,
            std::span<const std::byte> bytes, std::string& error) {
            error.clear();
            std::error_code filesystemError;
            std::filesystem::create_directories(destination.parent_path(),
                filesystemError);
            if (filesystemError) {
                error = "Could not create baked-probe directory: " +
                    filesystemError.message();
                return false;
            }
            std::filesystem::path temporary = destination;
            temporary += ".tmp";
            {
                std::ofstream output(temporary,
                    std::ios::binary | std::ios::trunc);
                if (!output) {
                    error = "Could not open the temporary baked-probe product.";
                    return false;
                }
                output.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                output.flush();
                if (!output) {
                    error = "Could not write the temporary baked-probe product.";
                    output.close();
                    std::filesystem::remove(temporary, filesystemError);
                    return false;
                }
            }
#if defined(_WIN32)
            if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
                return true;
            error = "Could not atomically publish the baked-probe product (Windows " +
                std::to_string(GetLastError()) + ").";
#else
            if (std::rename(temporary.c_str(), destination.c_str()) == 0)
                return true;
            error = "Could not atomically publish the baked-probe product.";
#endif
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
    }

    AssetIntegration::AssetIntegration(ApplicationConfig& config,
        CpuProfiler& profiler, EngineLog& log, Tasks::TaskSystem& tasks,
        SceneWorld& scene, EditorSceneDocumentService& sceneDocuments)
        : config_(config),
          cpuProfiler_(profiler),
          engineLog_(log),
          tasks_(tasks),
          sceneWorld_(scene),
          registry(scene.registry()),
          sceneDocumentService_(sceneDocuments) {}

    AssetIntegration::~AssetIntegration() = default;

    AssetManager& AssetIntegration::createAssetManager(IRenderBackend& backend,
        IEditorRenderBridge* editorBridge,
        TransparencyExecutionMode transparencyExecutionMode,
        uint32_t gpuLodMinimumResidentLevel) {
        renderBackend = &backend;
        assetManager_ = std::make_unique<AssetManager>(renderBackend,
            transparencyExecutionMode,
            gpuLodMinimumResidentLevel);
        assetManager_->setEditorRenderBridge(editorBridge);
        assetManager_->setTaskSystem(&tasks_);
        return *assetManager_;
    }

    void AssetIntegration::startServices(bool deterministicContent) {
        assetRuntimeService_ =
            std::make_unique<AssetRuntimeService>(
                AssetRuntimeServiceConfig{
                    .uploadBudgetBytes =
                        EditorRuntimeUploadBudgetBytes,
                    .startSourceWorkers =
                        !deterministicContent,
                    .tasks = &tasks_,
                });
        const std::filesystem::path assetRoot =
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets";
        assetCatalog_ = createSqliteAssetCatalog(
            !deterministicContent
                ? std::filesystem::path(PROJECT_ROOT_DIR) / "out" /
                    "editor" / "asset-catalog.sqlite"
                : std::filesystem::path(":memory:"));
        if (!deterministicContent) {
            const AssetDiscoveryResult discovery =
                discoverAssetRoots(std::array{
                    AssetRoot{ "project", assetRoot },
                });
            assetCatalog_->rebuild(
                discovery.records,
                discovery.sourceDirectories);
            for (const AssetDiscoveryDiagnostic& diagnostic :
                discovery.diagnostics) {
                std::cerr << "Asset catalog " << diagnostic.code << " at "
                    << diagnostic.path << ": " << diagnostic.message << '\n';
            }
            assetCatalogService_ =
                std::make_unique<AssetCatalogService>(
                    tasks_,
                    assetCatalog_.get(),
                    std::vector<AssetRoot>{
                        AssetRoot{ "project", assetRoot },
                    },
                    &engineLog_);
            editorModelDdc_ =
                std::make_shared<
                    LocalDerivedDataCache>(
                        std::filesystem::path(
                            PROJECT_ROOT_DIR) /
                        "out" / "editor" /
                        "model-ddc",
                        tasks_);
            assetModelPreparationService_ =
                std::make_unique<AssetModelPreparationService>(
                    tasks_,
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
            assetEnvironmentPreparationService_ =
                std::make_unique<AssetEnvironmentPreparationService>(
                    tasks_,
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
            assetThumbnailService_ =
                std::make_unique<AssetThumbnailService>(
                    tasks_,
                    assetRoot,
                    editorModelDdc_,
                    CookTarget{
                        .platform = "windows-x64",
                        .profile = "editor",
                        .qualityPolicy = "reference",
                        .artifactContainerVersion =
                            kCookedArtifactContainerVersion,
                        .materialSchemaVersion = 2,
                    },
                    &engineLog_);
        }
    }

    AssetGuid AssetIntegration::loadInteractiveStartupContent(
        AppStartupTimings& timings) {
        AssetGuid startupModelGuid;
        const auto importStart = std::chrono::steady_clock::now();
        const std::shared_ptr<ModelAsset> builtInCube =
            assetManager_->loadBuiltInCubeModel();
        if (config_.cookedModelArtifact.empty()) {
            mainModel_ = builtInCube;
            startupModelGuid = mainModel_->assetGuid;
        } else {
            startupModelGuid =
                loadCookedStartupModel()->assetGuid;
        }
        timings.modelLoadNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - importStart).count());
        configureCookedModelHotReload();
        const auto environmentStart = std::chrono::steady_clock::now();
        if (!config_.cookedEnvironmentArtifact.empty()) {
            loadCookedStartupEnvironment();
        }
        timings.environmentCreationNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - environmentStart).count());
        configureCookedEnvironmentHotReload();
        return startupModelGuid;
    }

    void AssetIntegration::tickRuntime() {
        CpuScope assetScope(
            cpuProfiler_,
            "cpu.asset_runtime.tick");
        const AssetRuntimeServiceTick
            assetTick =
                assetRuntimeService_
                    ->tick();
        const AssetRuntimeServiceStats
            assetStats =
                assetRuntimeService_
                    ->stats();
        cpuProfiler_.recordCounter(
            "asset.change_batches",
            assetTick.changeBatches);
        cpuProfiler_.recordCounter(
            "asset.rebuilds_requested",
            assetTick.rebuildsRequested);
        cpuProfiler_.recordCounter(
            "asset.rebuilds.total",
            assetStats.reimport.enqueued);
        cpuProfiler_.recordCounter(
            "asset.source.events",
            assetStats.source.watcher
                .changes);
        cpuProfiler_.recordCounter(
            "asset.source.same_content",
            assetStats.source.tracker
                .sameContent);
        cpuProfiler_.recordCounter(
            "asset.reimport.cancel_requests",
            assetStats.reimport
                .cancellationRequests);
        cpuProfiler_.recordCounter(
            "asset.publish.count",
            assetTick.publication.published);
        cpuProfiler_.recordCounter(
            "asset.publish.total",
            assetStats.publisher.published);
        cpuProfiler_.recordCounter(
            "asset.publish.failed",
            assetTick.publication.failed +
                assetTick.reimport.failed);
        cpuProfiler_.recordCounter(
            "asset.publish.failures_total",
            assetStats.publisher.failed);
        cpuProfiler_.recordCounter(
            "asset.upload.bytes",
            assetTick.publication
                .scheduledUploadBytes);
        cpuProfiler_.recordCounter(
            "asset.upload.bytes_total",
            assetStats.publisher
                .scheduledUploadBytes);
        cpuProfiler_.recordCounter(
            "asset.publish.queued",
            assetStats.publisher.queued);
        cpuProfiler_.recordCounter(
            "asset.resident.count",
            assetStats.publisher.resident);
        cpuProfiler_.recordCounter(
            "asset.resident.cpu_bytes",
            assetStats.publisher
                .cpuResidentBytes);
        cpuProfiler_.recordCounter(
            "asset.resident.gpu_bytes",
            assetStats.publisher
                .gpuResidentBytes);
        cpuProfiler_.recordCounter(
            "asset.evictions",
            assetStats.publisher.evicted);
        cpuProfiler_.recordCounter(
            "asset.retirements",
            assetStats.publisher.retired);
    }

    void AssetIntegration::publishCaptureCompletion(
        const ReflectionProbeCaptureCompletion& completion) {
        const std::optional<Entity> entity =
            sceneWorld_.identities().resolve(completion.owner);
        auto* probePool = registry.findPool<ReflectionProbeComponent>();
        if (entity && registry.isAlive(*entity) && probePool &&
            probePool->has(*entity)) {
            std::string& diagnostic = probePool->get(*entity).publicationDiagnostic;
            if (completion.bakedProduct) {
                diagnostic = persistBakedReflectionProbe(completion.owner,
                    *completion.bakedProduct);
            }
            else {
                // M7R R5c.8: the same text, formatted into the existing
                // string's storage (a runtime capture republishes every few
                // frames).
                char slot[16]{};
                const auto written = std::to_chars(slot, slot + sizeof(slot),
                    completion.environmentSlot);
                diagnostic.assign("Runtime capture published in environment slot ");
                diagnostic.append(slot, written.ptr);
                diagnostic.push_back('.');
            }
        }
    }

    void AssetIntegration::processMaterialPreviews(
        const EditorAssetDocumentService& documents) {
        CpuScope previewScope(cpuProfiler_, "cpu.asset.material_previews");
        std::vector<AssetGuid> previewDocuments;
        for (const auto& document : documents.documents()) previewDocuments.push_back(document.assetGuid);
        assetManager_->processMaterialPreviews(previewDocuments);
    }

    AppEnvironmentIdentity AssetIntegration::environmentIdentity() const {
        return {
            .cookedArtifact = activeCookedEnvironmentArtifact_,
            .assetGuid = activeEnvironmentAssetGuid_,
            .sourceGuid = activeEnvironmentSourceGuid_,
            .cookKey = activeEnvironmentCookKey_,
            .sourcePrimaries = activeEnvironmentSourcePrimaries_,
            .radianceScale = activeEnvironmentRadianceScale_,
        };
    }

    void AssetIntegration::shutdown() {
        assetThumbnailService_.reset();
        pendingThumbnailUploads_.clear();
        assetEnvironmentPreparationService_.reset();
        assetModelPreparationService_.reset();
        editorModelDdc_.reset();
        assetRuntimeService_.reset();
        assetCatalogService_.reset();
        assetCatalog_.reset();
        loadedEnvironments_.clear();
        assetManager_.reset();

        environmentLighting_ = {};
    }

    std::string AssetIntegration::persistBakedReflectionProbe(
        SceneEntityUuid owner,
        const ReflectionProbeCaptureCompletion::Product& captured) {
        try {
            const AssetGuid sceneGuid = sceneDocumentService_.sceneAssetGuid();
            const std::filesystem::path scenePath =
                sceneDocumentService_.currentPath();
            if (sceneGuid.isNil() || scenePath.empty())
                return "Bake failed: save the scene once to establish stable asset identity.";
            if (sceneDocumentService_.dirty())
                return "Bake failed: save the scene first so capture provenance matches it.";
            if (captured.resolution == 0 || captured.mipLevels == 0 ||
                captured.radiance.empty() ||
                captured.prefilteredSpecular.empty())
                return "Bake failed: GPU capture readback is incomplete.";

            const LoadedEnvironmentAsset* sharedEnvironment = nullptr;
            if (const auto active = loadedEnvironments_.find(
                    activeEnvironmentAssetGuid_);
                active != loadedEnvironments_.end())
                sharedEnvironment = &active->second;
            else if (!loadedEnvironments_.empty())
                sharedEnvironment = &loadedEnvironments_.begin()->second;
            if (sharedEnvironment == nullptr ||
                sharedEnvironment->brdfLut.empty())
                return "Bake failed: a resident HDRI is required for the shared BRDF product.";

            const std::filesystem::path projectRoot(PROJECT_ROOT_DIR);
            const std::filesystem::path relativeSource =
                std::filesystem::path("generated") / "reflection-probes" /
                sceneGuid.toString() / (owner.toString() + ".irprobe");
            const std::filesystem::path sourcePath =
                projectRoot / "assets" / relativeSource;
            const std::filesystem::path metadataPath =
                assetMetadataSidecarPath(sourcePath);

            AssetMetadata metadata;
            std::error_code filesystemError;
            if (std::filesystem::exists(metadataPath, filesystemError) &&
                !filesystemError) {
                AssetMetadataReadResult existing =
                    readAssetMetadata(metadataPath);
                if (!existing.metadata || existing.hasErrors())
                    return "Bake failed: the existing generated asset metadata is invalid.";
                metadata = std::move(*existing.metadata);
                if (metadata.assetType != "iridium.environment" ||
                    metadata.importerId !=
                        "iridium.environment.probe_capture")
                    return "Bake failed: the generated path belongs to an incompatible asset.";
            } else {
                if (filesystemError)
                    return "Bake failed: could not inspect generated asset metadata.";
                metadata.assetGuid = createAssetGuidV7();
                metadata.assetType = "iridium.environment";
                metadata.importerId =
                    "iridium.environment.probe_capture";
                metadata.importerVersion = 1;
                metadata.settingsSchemaVersion = 1;
                metadata.settings = nlohmann::json::object();
            }
            metadata.tags = { "generated", "reflection-probe" };

            constexpr uint32_t IrradianceSize = 32;
            std::vector<std::byte> irradiance =
                makeCapturedCubeDiffuseIrradiance(captured.radiance,
                    captured.resolution, IrradianceSize);
            const CookedEnvironmentManifest manifest{
                .sourceTextureGuid = metadata.assetGuid,
                .sourcePrimaries = "acescg_ap1_d60",
                .sourceRadianceScale = 1.0f,
                .convolutionImplementation =
                    "iridium_gpu_scene_capture_ggx_v1",
                .sampleSequence =
                    "exact_cube_texel_sh9_v1+hammersley_base2_vdc_v1_" +
                    std::to_string(
                        config_.reflectionProbeSettings.prefilterSampleCount),
                .toolVersion = std::string("Iridium ") +
                    BuildInfo::sourceCommit() + " " +
                    BuildInfo::configuration(),
                .radiance = { captured.resolution, captured.resolution, 1, 6,
                    TextureFormat::RGBA16_SFloat },
                .irradiance = { IrradianceSize, IrradianceSize, 1, 6,
                    TextureFormat::RGBA16_SFloat },
                .prefilteredSpecular = { captured.resolution,
                    captured.resolution, captured.mipLevels, 6,
                    TextureFormat::RGBA16_SFloat },
                .brdfLut = sharedEnvironment->manifest.brdfLut,
            };
            CookProduct product = makeCookedEnvironmentProduct(manifest,
                { captured.radiance, irradiance,
                    captured.prefilteredSpecular,
                    sharedEnvironment->brdfLut });
            if (hasCookErrors(product.diagnostics))
                return cookFailureMessage("Bake failed", product.diagnostics);

            const std::string sceneHash = sha256File(scenePath);
            std::string sourceIdentity = sceneGuid.toString() + "\n" +
                owner.toString() + "\n" + sceneHash + "\n" +
                sha256(captured.radiance) + "\n" +
                sha256(captured.prefilteredSpecular) + "\n" +
                sha256(irradiance) + "\n" +
                sha256(sharedEnvironment->brdfLut);
            const std::string sourceHash = sha256(std::as_bytes(std::span(
                sourceIdentity.data(), sourceIdentity.size())));
            std::vector<AssetDependency> dependencies{
                {
                    .type = AssetDependencyType::Asset,
                    .assetGuid = sceneGuid,
                    .location = std::filesystem::relative(scenePath,
                        projectRoot, filesystemError).generic_string(),
                    .contentHash = sceneHash,
                },
                {
                    .type = AssetDependencyType::Asset,
                    .assetGuid = sharedEnvironment->assetGuid,
                    .location = "environment/" +
                        sharedEnvironment->assetGuid.toString(),
                },
            };
            if (filesystemError) {
                filesystemError.clear();
                dependencies[0].location = scenePath.generic_string();
            }
            for (const char* shader : {
                    "reflection_probe_capture.vert",
                    "reflection_probe_capture.frag",
                    "reflection_probe_prefilter.comp" }) {
                const std::filesystem::path shaderPath =
                    projectRoot / "assets" / "shaders" / shader;
                dependencies.push_back({
                    .type = AssetDependencyType::Tool,
                    .location = (std::filesystem::path("assets") /
                        "shaders" / shader).generic_string(),
                    .contentHash = sha256File(shaderPath),
                });
            }
            std::ranges::sort(dependencies);
            const CookTarget target{
                .platform = "windows-x64",
                .profile = "editor",
                .qualityPolicy = "reflection-probe-high",
                .artifactContainerVersion = kCookedArtifactContainerVersion,
                .materialSchemaVersion = 2,
            };
            static constexpr std::byte EmptySettings[]{
                std::byte{ '{' }, std::byte{ '}' },
            };
            const std::string cookKey = calculateCookKey({
                .assetGuid = metadata.assetGuid,
                .importerId = "iridium.environment.probe_capture",
                .importerImplementationVersion = 1,
                .settingsSchemaVersion = 1,
                .canonicalSettings = EmptySettings,
                .sourceContentHash = sourceHash,
                .dependencies = dependencies,
                .target = target,
                .cookerFeatureVersion =
                    "reflection-probe-scene-capture-v1",
            });
            const CookedArtifact artifact{
                .assetGuid = metadata.assetGuid,
                .artifactType = product.artifactType,
                .artifactSchemaVersion = product.artifactSchemaVersion,
                .target = target,
                .cookKey = cookKey,
                .dependencies = std::move(dependencies),
                .sections = std::move(product.sections),
            };
            const CookedArtifactBlob blob = serializeCookedArtifact(artifact);
            std::string writeError;
            if (!writeBinaryAtomic(sourcePath, blob.bytes, writeError))
                return "Bake failed: " + writeError;
            if (!writeAssetMetadataAtomic(metadataPath, metadata, writeError))
                return "Bake product was written, but metadata publication failed: " +
                    writeError;
            if (assetCatalogService_)
                (void)assetCatalogService_->requestRefresh();
            return "Baked environment " + metadata.assetGuid.toString() +
                " to " + relativeSource.generic_string() +
                ". Assign it from the Asset Browser when ready.";
        } catch (const std::exception& exception) {
            return std::string("Bake failed: ") + exception.what();
        }
    }

    void AssetIntegration::processMeshSwaps() {
        CpuScope swapScope(cpuProfiler_, "cpu.scene.asset_swaps");
        if (!assetManager_) return;

        if (assetModelPreparationService_ &&
            assetRuntimeService_) {
            for (PreparedCatalogModel& result :
                assetModelPreparationService_
                    ->takeResults()) {
                if (!result.succeeded ||
                    !result.artifact ||
                    !result.product) {
                    const std::string diagnostic =
                        result.diagnostic.empty()
                        ? "Catalog model preparation failed without a diagnostic."
                        : result.diagnostic;
                    assetRuntimeService_->reportFailure(
                        result.assetGuid,
                        diagnostic);
                    continue;
                }
                const AssetGuid assetGuid =
                    result.assetGuid;
                if (assetThumbnailService_) {
                    assetThumbnailService_->invalidate(
                        assetGuid);
                }
                const uint64_t cpuBytes =
                    result.cpuResidentBytes;
                const uint64_t gpuBytes =
                    result.gpuResidentBytes;
                if (gpuBytes >
                    EditorModelPublicationLimitBytes) {
                    const std::string diagnostic =
                        "Prepared model requires " +
                        std::to_string(
                            gpuBytes / (1024ull *
                                1024ull)) +
                        " MiB of GPU upload data, exceeding the editor's 1 GiB atomic model-publication safety cap.";
                    assetRuntimeService_->
                        reportFailure(
                            assetGuid,
                            diagnostic);
                    engineLog_.error(
                        "Asset Runtime",
                        diagnostic);
                    continue;
                }
                if (gpuBytes > EditorRuntimeUploadBudgetBytes) {
                    engineLog_.warning("Asset Runtime",
                        "Prepared model requires " + std::to_string(
                            gpuBytes / (1024ull * 1024ull)) +
                        " MiB of GPU upload data. It will publish atomically as the only asset upload this frame; the 128 MiB value is a scheduling budget, not a model-size limit.");
                }
                std::shared_ptr<CookedArtifact> artifact =
                    std::move(result.artifact);
                std::shared_ptr<CookedModelProductData> product =
                    std::move(result.product);
                (void)assetRuntimeService_->enqueuePrepared(
                    assetGuid,
                    PreparedRuntimeAsset{
                        .cookKey = artifact->cookKey,
                        .estimatedUploadBytes = gpuBytes,
                        .allowSingleOversizedUpload =
                            gpuBytes > EditorRuntimeUploadBudgetBytes,
                        .publish =
                            [this,
                                artifact = std::move(artifact),
                                product = std::move(product),
                                cpuBytes,
                                gpuBytes] {
                                try {
                                    (void)assetManager_->
                                        replaceSelfContainedModelFromCookedProduct(
                                            *artifact, *product);
                                    return RuntimeAssetPublishOutcome{
                                        .succeeded = true,
                                        .cpuResidentBytes =
                                            cpuBytes,
                                        .gpuResidentBytes =
                                            gpuBytes,
                                    };
                                }
                                catch (const std::exception&
                                    exception) {
                                    return RuntimeAssetPublishOutcome{
                                        .diagnostic =
                                            exception.what(),
                                    };
                                }
                            },
                    });
            }
        }

        if (assetEnvironmentPreparationService_ &&
            assetRuntimeService_) {
            for (PreparedCatalogEnvironment& result :
                assetEnvironmentPreparationService_->takeResults()) {
                if (!result.succeeded || !result.artifact || !result.product) {
                    assetRuntimeService_->reportFailure(result.assetGuid,
                        result.diagnostic.empty()
                        ? "HDRI environment preparation failed without a diagnostic."
                        : result.diagnostic);
                    continue;
                }
                const AssetGuid assetGuid = result.assetGuid;
                if (assetThumbnailService_) {
                    assetThumbnailService_->invalidate(assetGuid);
                }
                const uint64_t gpuBytes = result.gpuResidentBytes;
                if (gpuBytes > EditorEnvironmentPublicationLimitBytes) {
                    assetRuntimeService_->reportFailure(assetGuid,
                        "Prepared HDRI environment exceeds the 640 MiB editor environment publication limit.");
                    continue;
                }
                std::shared_ptr<CookedArtifact> artifact =
                    std::move(result.artifact);
                (void)assetRuntimeService_->enqueuePrepared(assetGuid,
                    PreparedRuntimeAsset{
                        .cookKey = artifact->cookKey,
                        .estimatedUploadBytes = gpuBytes,
                        .allowSingleOversizedUpload = true,
                        .publish = [this, artifact = std::move(artifact),
                            gpuBytes] {
                            try {
                                LoadedEnvironmentAsset replacement =
                                    assetManager_->loadEnvironmentFromCookedArtifact(
                                        *artifact);
                                const AssetGuid replacementGuid =
                                    replacement.assetGuid;
                                const bool replacesActiveEnvironment =
                                    replacementGuid == activeEnvironmentAssetGuid_;
                                EnvironmentLightingHandles previous{};
                                if (const auto loaded = loadedEnvironments_.find(
                                        replacementGuid);
                                    loaded != loadedEnvironments_.end()) {
                                    previous = loaded->second.lighting;
                                }
                                const bool replacesBoundEnvironment = previous.isValid() &&
                                    previous == renderBackend->getEnvironmentLighting();
                                if (replacesActiveEnvironment || replacesBoundEnvironment) try {
                                    renderBackend->setEnvironmentLighting(
                                        replacement.lighting);
                                }
                                catch (...) {
                                    assetManager_->releaseEnvironment(
                                        replacement.lighting);
                                    throw;
                                }
                                if (replacesActiveEnvironment) {
                                    environmentLighting_ = replacement.lighting;
                                    activeEnvironmentSourceGuid_ =
                                        replacement.manifest.sourceTextureGuid;
                                    activeEnvironmentCookKey_ =
                                        replacement.cookKey;
                                    activeEnvironmentSourcePrimaries_ =
                                        replacement.manifest.sourcePrimaries;
                                    activeEnvironmentRadianceScale_ =
                                        replacement.manifest.sourceRadianceScale;
                                }
                                loadedEnvironments_.insert_or_assign(
                                    replacementGuid, std::move(replacement));
                                assetManager_->releaseEnvironment(previous);
                                return RuntimeAssetPublishOutcome{
                                    .succeeded = true,
                                    .gpuResidentBytes = gpuBytes,
                                };
                            }
                            catch (const std::exception& exception) {
                                return RuntimeAssetPublishOutcome{
                                    .diagnostic = exception.what(),
                                };
                            }
                        },
                    });
            }
        }

        if (assetThumbnailService_) {
            for (PreparedAssetThumbnailBatch& batch :
                assetThumbnailService_
                    ->takeResults()) {
                if (!batch.diagnostic.empty()) {
                    std::cerr
                        << "Failed to prepare thumbnails for "
                        << batch.rootAssetGuid.toString()
                        << ": " << batch.diagnostic
                        << '\n';
                }
                for (AssetThumbnailPixels& thumbnail :
                    batch.thumbnails) {
                    if (thumbnail.valid() &&
                        assetThumbnailService_
                            ->isDemanded(
                                thumbnail.assetGuid)) {
                        pendingThumbnailUploads_
                            .enqueue(
                                std::move(thumbnail));
                    }
                }
            }

            constexpr uint64_t
                thumbnailUploadBudget =
                    512ull * 1024ull;
            const AssetThumbnailUploadDrain
                thumbnailDrain =
                    pendingThumbnailUploads_.drain(
                        thumbnailUploadBudget,
                        [this](AssetGuid guid) {
                            return assetThumbnailService_
                                ->isDemanded(guid);
                        },
                        [this](
                            const AssetThumbnailPixels&
                                thumbnail) {
                            try {
                                if (thumbnail.purpose ==
                                        AssetThumbnailPurpose::
                                            Detail) {
                                    assetManager_->
                                        publishEditorDetailThumbnail(
                                            thumbnail.assetGuid,
                                            thumbnail.width,
                                            thumbnail.height,
                                            thumbnail.rgba8);
                                }
                                else {
                                    const std::optional<
                                        AssetGuid> evicted =
                                            assetManager_->
                                            publishEditorThumbnail(
                                                thumbnail.assetGuid,
                                                thumbnail.width,
                                                thumbnail.height,
                                                thumbnail.rgba8);
                                    assetThumbnailService_
                                        ->markPublished(
                                            thumbnail.assetGuid);
                                    if (evicted) {
                                        assetThumbnailService_
                                            ->markEvicted(
                                                *evicted);
                                    }
                                }
                            }
                            catch (const std::exception&
                                exception) {
                                assetThumbnailService_
                                    ->reportFailure(
                                        thumbnail.assetGuid,
                                        exception.what());
                                std::cerr
                                    << "Failed to upload thumbnail "
                                    << thumbnail.assetGuid
                                        .toString()
                                    << ": "
                                    << exception.what()
                                    << '\n';
                            }
                        });
            thumbnailUploadsTotal_ +=
                thumbnailDrain.uploaded;
            thumbnailUploadBytesTotal_ +=
                thumbnailDrain.uploadedBytes;
            const AssetThumbnailServiceStats
                thumbnailStats =
                    assetThumbnailService_->stats();
            cpuProfiler_.recordCounter(
                "asset.thumbnail.upload_bytes",
                thumbnailDrain.uploadedBytes);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.uploaded",
                thumbnailDrain.uploaded);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.cancelled",
                thumbnailDrain.cancelled);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.deferred",
                thumbnailDrain
                    .deferredByBudget);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.pending_uploads",
                thumbnailDrain
                    .queuedAfterDrain);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.uploaded_total",
                thumbnailUploadsTotal_);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.upload_bytes_total",
                thumbnailUploadBytesTotal_);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.produced_total",
                thumbnailStats
                    .thumbnailsProduced);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.failed_total",
                thumbnailStats
                    .thumbnailsFailed);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.demanded",
                thumbnailStats
                    .demandedAssets);
            cpuProfiler_.recordCounter(
                "asset.thumbnail.queued_roots",
                thumbnailStats
                    .queuedRoots);
        }

        auto* probePool = registry.getPool<ReflectionProbeComponent>();
        if (probePool) {
            for (Entity entity : probePool->entities) {
                ReflectionProbeComponent& probe = probePool->get(entity);
                const AssetGuid requested =
                    !probe.requestedEnvironmentAssetGuid.isNil()
                    ? probe.requestedEnvironmentAssetGuid
                    : probe.environmentAssetGuid;
                if (!probe.enabled || requested.isNil()) continue;
                if (loadedEnvironments_.contains(requested)) {
                    probe.environmentAssetGuid = requested;
                    probe.resolvedEnvironmentAssetGuid = requested;
                    probe.requestedEnvironmentAssetGuid = {};
                    probe.publicationDiagnostic.clear();
                    continue;
                }
                probe.resolvedEnvironmentAssetGuid = {};
                const auto snapshot = assetRuntimeService_
                    ? assetRuntimeService_->snapshot(requested)
                    : std::nullopt;
                if (snapshot &&
                    (snapshot->state == RuntimeAssetState::Failed ||
                     snapshot->state == RuntimeAssetState::ReadyWithError)) {
                    probe.publicationDiagnostic = snapshot->diagnostic;
                }
                const bool alreadyPending = snapshot &&
                    (snapshot->state == RuntimeAssetState::Queued ||
                     snapshot->state == RuntimeAssetState::Ready);
                if (alreadyPending ||
                    (assetCatalogService_ && assetCatalogService_->busy())) {
                    continue;
                }
                const std::vector<AssetCatalogRecord> records = assetCatalog_
                    ? assetCatalog_->recordsForGuid(requested)
                    : std::vector<AssetCatalogRecord>{};
                const auto record = std::ranges::find_if(records,
                    [](const AssetCatalogRecord& candidate) {
                        return !candidate.parentGuid &&
                            candidate.assetType == "iridium.environment" &&
                            candidate.assetRoot == "project" &&
                            candidate.status == AssetCatalogStatus::Ready;
                    });
                if (record == records.end()) {
                    probe.publicationDiagnostic =
                        "GUID is not currently present as a ready reflection-probe environment asset.";
                }
                else if (!assetEnvironmentPreparationService_) {
                    probe.publicationDiagnostic =
                        "Background reflection-probe environment preparation is unavailable.";
                }
                else if (!assetEnvironmentPreparationService_->pending(
                        requested)) {
                    (void)assetEnvironmentPreparationService_->request(*record);
                    probe.publicationDiagnostic.clear();
                }
            }
        }

        auto* skyPool = registry.getPool<SkyComponent>();
        if (skyPool && !skyPool->entities.empty()) {
            Entity activeSkyEntity = NULL_ENTITY;
            std::optional<SceneEntityUuid> activeSkyUuid;
            SkyComponent* activeSky = nullptr;
            for (Entity entity : skyPool->entities) {
                SkyComponent& candidate = skyPool->get(entity);
                if (!candidate.enabled || candidate.mode != SkyMode::Hdri) {
                    continue;
                }
                const auto candidateUuid =
                    sceneWorld_.identities().persistentId(entity);
                const bool stableTieBreak = candidateUuid
                    ? (!activeSkyUuid || *candidateUuid < *activeSkyUuid)
                    : (!activeSkyUuid &&
                        entity.index() < activeSkyEntity.index());
                if (!activeSky || candidate.priority > activeSky->priority ||
                    (candidate.priority == activeSky->priority &&
                     stableTieBreak)) {
                    activeSkyEntity = entity;
                    activeSkyUuid = candidateUuid;
                    activeSky = &candidate;
                }
            }
            if (activeSky) {
                sceneEnvironmentSettings_ = {
                    .lightingIntensity = activeSky->hdri.lightingIntensity,
                    .backgroundIntensity = activeSky->hdri.backgroundIntensity,
                    .rotationRadians = glm::radians(
                        activeSky->hdri.rotationDegrees),
                    .visibleToCamera = activeSky->hdri.visibleToCamera,
                    .affectsLighting = activeSky->hdri.affectsLighting,
                };
                renderBackend->setEnvironmentLightingSettings(sceneEnvironmentSettings_);
                const AssetGuid requested =
                    !activeSky->requestedEnvironmentAssetGuid.isNil()
                    ? activeSky->requestedEnvironmentAssetGuid
                    : activeSky->hdri.environmentAssetGuid;
                if (!requested.isNil() &&
                    requested == activeEnvironmentAssetGuid_) {
                    activeSky->hdri.environmentAssetGuid = requested;
                    activeSky->resolvedEnvironmentAssetGuid = requested;
                    activeSky->requestedEnvironmentAssetGuid = {};
                    activeSky->requestedAssetSourcePath.clear();
                    activeSky->assetResolutionDiagnostic.clear();
                }
                else if (!requested.isNil()) {
                    if (const auto loaded = loadedEnvironments_.find(requested);
                        loaded != loadedEnvironments_.end()) {
                        renderBackend->setEnvironmentLighting(
                            loaded->second.lighting);
                        environmentLighting_ = loaded->second.lighting;
                        activeEnvironmentAssetGuid_ = loaded->second.assetGuid;
                        activeEnvironmentSourceGuid_ =
                            loaded->second.manifest.sourceTextureGuid;
                        activeEnvironmentCookKey_ = loaded->second.cookKey;
                        activeEnvironmentSourcePrimaries_ =
                            loaded->second.manifest.sourcePrimaries;
                        activeEnvironmentRadianceScale_ =
                            loaded->second.manifest.sourceRadianceScale;
                        activeSky->hdri.environmentAssetGuid = requested;
                        activeSky->resolvedEnvironmentAssetGuid = requested;
                        activeSky->requestedEnvironmentAssetGuid = {};
                        activeSky->requestedAssetSourcePath.clear();
                        activeSky->assetResolutionDiagnostic.clear();
                    }
                    else {
                    const auto snapshot = assetRuntimeService_
                        ? assetRuntimeService_->snapshot(requested)
                        : std::nullopt;
                    if (snapshot &&
                        (snapshot->state == RuntimeAssetState::Failed ||
                         snapshot->state == RuntimeAssetState::ReadyWithError)) {
                        activeSky->assetResolutionDiagnostic =
                            snapshot->diagnostic;
                    }
                    const bool alreadyPending = snapshot &&
                        (snapshot->state == RuntimeAssetState::Queued ||
                         snapshot->state == RuntimeAssetState::Ready);
                    if (!alreadyPending &&
                        !(assetCatalogService_ && assetCatalogService_->busy())) {
                        const std::vector<AssetCatalogRecord> records =
                            assetCatalog_
                            ? assetCatalog_->recordsForGuid(requested)
                            : std::vector<AssetCatalogRecord>{};
                        const auto record = std::ranges::find_if(records,
                            [](const AssetCatalogRecord& candidate) {
                                return !candidate.parentGuid &&
                                    candidate.assetType ==
                                        "iridium.environment" &&
                                    candidate.assetRoot == "project" &&
                                    candidate.status ==
                                        AssetCatalogStatus::Ready;
                            });
                        if (record == records.end()) {
                            activeSky->assetResolutionDiagnostic =
                                "GUID is not currently present as a ready HDRI environment asset.";
                        }
                        else if (!assetEnvironmentPreparationService_) {
                            activeSky->assetResolutionDiagnostic =
                                "Background HDRI preparation is unavailable.";
                        }
                        else if (!assetEnvironmentPreparationService_->pending(
                                requested)) {
                            (void)assetEnvironmentPreparationService_->request(
                                *record);
                            activeSky->requestedAssetSourcePath =
                                record->sourcePath;
                            activeSky->assetResolutionDiagnostic.clear();
                        }
                    }
                    }
                }
            }
        }

        auto* meshPool = registry.getPool<MeshComponent>();
        if (!meshPool) return;

        for (Entity entity : meshPool->entities) {
            // Read first: a mutable access is recorded in the pool's write
            // journal (M7R R5c.5), and most entities need no change here.
            const MeshComponent& meshView =
                std::as_const(*meshPool).get(entity);
            if (meshView.requestedAssetGuid.isNil() &&
                meshView.materialOverrides.empty()) continue;
            auto& meshComp = meshPool->get(entity);
            if (!meshComp.requestedAssetGuid.isNil()) {
                const AssetGuid requestedGuid =
                    meshComp.requestedAssetGuid;
                try {
                    std::shared_ptr<ModelAsset> resolved =
                        assetManager_->findCookedModel(
                            requestedGuid);
                    if (!resolved && mainModel_ &&
                        mainModel_->assetGuid ==
                            requestedGuid) {
                        resolved = mainModel_;
                    }
                    if (resolved) {
                        meshComp.model =
                            std::move(resolved);
                        meshComp.assetGuid =
                            requestedGuid;
                        meshComp.requestedAssetGuid = {};
                        meshComp.requestedAssetSourcePath.clear();
                        meshComp.assetResolutionDiagnostic.clear();
                        continue;
                    }

                    std::string priorFailure;
                    if (assetRuntimeService_) {
                        const std::optional<
                            RuntimeAssetSnapshot> snapshot =
                                assetRuntimeService_->snapshot(
                                    requestedGuid);
                        if (snapshot &&
                            (snapshot->state ==
                                RuntimeAssetState::Queued ||
                             snapshot->state ==
                                RuntimeAssetState::Ready)) {
                            continue;
                        }
                        if (snapshot &&
                            (snapshot->state ==
                                RuntimeAssetState::Failed ||
                             snapshot->state ==
                                RuntimeAssetState::ReadyWithError)) {
                            priorFailure =
                                snapshot->diagnostic;
                        }
                    }

                    // A refresh replaces the rebuildable catalog atomically, but
                    // resolution can still arrive while its background discovery
                    // job is active. Keep the stable GUID pending instead of
                    // converting a transient refresh window into a permanent
                    // component failure. Failed stale runtime preparations also
                    // fall through so the current catalog path can be retried.
                    if (assetCatalogService_ &&
                        assetCatalogService_->busy()) {
                        continue;
                    }

                    const std::vector<AssetCatalogRecord> records =
                        assetCatalog_
                        ? assetCatalog_->recordsForGuid(
                            requestedGuid)
                        : std::vector<AssetCatalogRecord>{};
                    const auto record =
                        std::ranges::find_if(
                            records,
                            [](const AssetCatalogRecord&
                                candidate) {
                                return !candidate.parentGuid &&
                                    candidate.assetType ==
                                        "iridium.model" &&
                                    candidate.assetRoot ==
                                        "project" &&
                                    candidate.status ==
                                        AssetCatalogStatus::Ready;
                            });
                    if (record == records.end()) {
                        meshComp.assetResolutionDiagnostic =
                            "GUID is not currently present as a ready model asset. "
                            "The assignment will retry after catalog refresh.";
                        continue;
                    }
                    if (!assetModelPreparationService_) {
                        throw std::runtime_error(
                            "Background cooked model preparation is unavailable.");
                    }
                    if (!priorFailure.empty() &&
                        meshComp.requestedAssetSourcePath ==
                            record->sourcePath) {
                        meshComp.assetResolutionDiagnostic =
                            priorFailure;
                        continue;
                    }
                    if (!assetModelPreparationService_->pending(
                            requestedGuid)) {
                        (void)assetModelPreparationService_
                            ->request(*record);
                        meshComp.requestedAssetSourcePath =
                            record->sourcePath;
                        meshComp.assetResolutionDiagnostic.clear();
                    }
                }
                catch (const std::exception& error) {
                    const std::string diagnostic =
                        error.what();
                    if (meshComp.assetResolutionDiagnostic !=
                        diagnostic) {
                        std::cerr << "Failed to resolve model asset "
                            << requestedGuid.toString() << ": "
                            << diagnostic << '\n';
                    }
                    meshComp.assetResolutionDiagnostic =
                        diagnostic;
                }
                continue;
            }
            for (const MeshComponent::MaterialOverride&
                    materialOverride :
                meshComp.materialOverrides) {
                if (materialOverride.materialGuid.isNil() ||
                    assetManager_->findCookedMaterial(
                        materialOverride.materialGuid)) {
                    continue;
                }
                const std::vector<AssetCatalogRecord>
                    materialRecords =
                        assetCatalog_
                        ? assetCatalog_->recordsForGuid(
                            materialOverride.materialGuid)
                        : std::vector<AssetCatalogRecord>{};
                const auto materialRecord =
                    std::ranges::find_if(
                        materialRecords,
                        [](const AssetCatalogRecord& record) {
                            return record.parentGuid &&
                                record.assetType ==
                                    "iridium.material" &&
                                record.status ==
                                    AssetCatalogStatus::Ready;
                        });
                if (materialRecord ==
                        materialRecords.end() ||
                    !materialRecord->parentGuid ||
                    !assetModelPreparationService_) {
                    continue;
                }
                const AssetGuid ownerGuid =
                    *materialRecord->parentGuid;
                if (std::ranges::find(
                        meshComp.requestedMaterialAssetRoots,
                        ownerGuid) !=
                    meshComp.requestedMaterialAssetRoots.end()) {
                    continue;
                }
                const std::vector<AssetCatalogRecord>
                    ownerRecords =
                        assetCatalog_->recordsForGuid(
                            ownerGuid);
                const auto owner =
                    std::ranges::find_if(
                        ownerRecords,
                        [](const AssetCatalogRecord& record) {
                            return !record.parentGuid &&
                                record.assetType ==
                                    "iridium.model" &&
                                record.status ==
                                    AssetCatalogStatus::Ready;
                        });
                if (owner != ownerRecords.end()) {
                    if (!assetModelPreparationService_
                            ->pending(ownerGuid)) {
                        (void)assetModelPreparationService_
                            ->request(*owner);
                    }
                    meshComp.requestedMaterialAssetRoots
                        .push_back(ownerGuid);
                }
            }
        }
    }

    void AssetIntegration::configureCookedModelHotReload() {
        if (activeCookedModelArtifact_.empty() ||
            !mainModel_ ||
            mainModel_->assetGuid.isNil() ||
            !assetRuntimeService_) {
            return;
        }
        const CookedArtifactBlob baselineBlob =
            readCookedArtifactBlobFile(
                activeCookedModelArtifact_);
        const CookedArtifactReadResult baselineArtifact =
            readCookedArtifact(
                baselineBlob.bytes,
                baselineBlob.artifactHash);
        if (!baselineArtifact.valid()) {
            throw std::runtime_error(
                "Cooked hot-reload baseline container is invalid.");
        }
        const CookedModelReadResult baselineModel =
            readCookedModelProduct(
                *baselineArtifact.artifact);
        if (!baselineModel.valid()) {
            throw std::runtime_error(
                "Cooked hot-reload baseline model is invalid.");
        }
        if (baselineArtifact.artifact->assetGuid !=
            mainModel_->assetGuid) {
            throw std::runtime_error(
                "Cooked hot-reload baseline GUID does not match the loaded model.");
        }

        const auto residentBytes =
            [](const CookedModelProductData& product) {
                uint64_t gpuBytes =
                    product.vertices.size() *
                        sizeof(Vertex) +
                    product.indices.size() *
                        sizeof(uint32_t) +
                    product.materials.size() *
                        sizeof(PackedGpuMaterial);
                for (const CookedModelTextureView& view :
                    product.textureViews) {
                    gpuBytes += view.payload.size();
                }
                const uint64_t cpuBytes =
                    sizeof(ModelAsset) +
                    product.manifest.primitives.size() *
                        sizeof(SubMesh) +
                    product.materials.size() *
                        sizeof(MaterialBinding);
                return std::pair{
                    cpuBytes, gpuBytes,
                };
            };
        const auto [baselineCpuBytes,
            baselineGpuBytes] =
                residentBytes(
                    *baselineModel.data);
        if (baselineGpuBytes > EditorModelPublicationLimitBytes) {
            throw std::runtime_error(
                "Cooked model hot-reload baseline exceeds the 1 GiB atomic model-publication safety cap.");
        }
        const AssetGuid expectedGuid =
            mainModel_->assetGuid;
        const std::filesystem::path artifactPath =
            activeCookedModelArtifact_;
        std::map<std::filesystem::path,
            std::string> watchedSources;
        watchedSources.emplace(
            artifactPath,
            baselineBlob.artifactHash);
        std::shared_ptr<ModelSourceReimportContext>
            sourceContext;
        const std::filesystem::path assetRoot =
            std::filesystem::path(
                PROJECT_ROOT_DIR) / "assets";
        const AssetDiscoveryResult discovery =
            discoverAssetRoots(std::array{
                AssetRoot{ "project", assetRoot },
            });
        const auto sourceRecord =
            std::ranges::find_if(
                discovery.records,
                [expectedGuid](
                    const AssetCatalogRecord& record) {
                    return record.guid ==
                            expectedGuid &&
                        !record.parentGuid &&
                        record.status ==
                            AssetCatalogStatus::Ready;
                });
        if (sourceRecord !=
            discovery.records.end()) {
            sourceContext =
                std::make_shared<
                    ModelSourceReimportContext>();
            sourceContext->assetRoot =
                assetRoot;
            sourceContext->sourceRelativePath =
                sourceRecord->sourcePath;
            sourceContext->metadataPath =
                assetRoot /
                    sourceRecord->metadataPath;
            sourceContext->target =
                baselineArtifact.artifact->target;
            sourceContext->cache =
                std::make_shared<
                    LocalDerivedDataCache>(
                    std::filesystem::path(
                        PROJECT_ROOT_DIR) /
                    "out" / "m3.5" /
                    "editor-live-ddc",
                    tasks_);
            sourceContext->importers
                .registerImporter(
                    std::make_shared<
                        TextFixtureImporter>());
            sourceContext->importers
                .registerImporter(
                    std::make_shared<
                        TextureImporter>());
            registerGltfModelImporters(sourceContext->importers);
            const std::filesystem::path
                sourcePath =
                    assetRoot /
                    sourceContext
                        ->sourceRelativePath;
            watchedSources[sourcePath] =
                sha256File(sourcePath);
            watchedSources[
                sourceContext->metadataPath] =
                    sha256File(
                        sourceContext
                            ->metadataPath);
            for (const AssetDependency& dependency :
                baselineArtifact.artifact
                    ->dependencies) {
                if (dependency.type ==
                        AssetDependencyType::SourceFile &&
                    !dependency.location.empty() &&
                    !dependency.contentHash.empty()) {
                    watchedSources[
                        assetRoot /
                            dependency.location] =
                                dependency
                                    .contentHash;
                }
            }
        }
        std::vector<TrackedSourceFile>
            trackedSources;
        trackedSources.reserve(
            watchedSources.size());
        for (auto& [path, hash] :
            watchedSources) {
            trackedSources.push_back({
                path, std::move(hash),
            });
        }
        assetRuntimeService_->track({
            .assetGuid = expectedGuid,
            .sources =
                std::move(trackedSources),
            .dependencies =
                baselineArtifact.artifact
                    ->dependencies,
            .prepare =
                [this, artifactPath,
                    expectedGuid, residentBytes,
                    sourceContext](
                    const AssetReimportCause&
                        cause,
                    std::stop_token stopToken) {
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Cooked model reimport cancelled.");
                    }
                    const bool sourceChanged =
                        sourceContext &&
                        std::ranges::any_of(
                            cause.changedSources,
                            [&artifactPath](
                                const SourceContentChange&
                                    change) {
                                return change.sourcePath !=
                                    artifactPath;
                            });
                    CookedArtifactBlob blob;
                    if (sourceChanged) {
                        const AssetMetadataReadResult
                            metadata =
                                readAssetMetadata(
                                    sourceContext
                                        ->metadataPath);
                        if (!metadata.metadata ||
                            metadata.hasErrors() ||
                            metadata.metadata
                                ->assetGuid !=
                                    expectedGuid) {
                            throw std::runtime_error(
                                "Source reimport metadata is invalid or has the wrong GUID.");
                        }
                        auto prepared =
                            std::make_shared<
                                PreparedAssetCook>(
                            prepareAssetCook(
                                sourceContext
                                    ->importers,
                                sourceContext
                                    ->assetRoot,
                                sourceContext
                                    ->sourceRelativePath,
                                *metadata.metadata,
                                sourceContext
                                    ->target,
                                "m3.2-framework-v3",
                                stopToken));
                        if (!prepared->valid()) {
                            throw std::runtime_error(
                                cookFailureMessage(
                                    "Source reimport preparation failed",
                                    prepared
                                        ->diagnostics));
                        }
                        const auto cookProgressStart =
                            std::chrono::steady_clock::now();
                        prepared->context.progress =
                            [this,
                                sourcePath = sourceContext
                                    ->sourceRelativePath
                                    .generic_string(),
                                cookProgressStart](
                                const AssetCookContext::Progress&
                                    progress) {
                                const auto elapsedMilliseconds =
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() -
                                        cookProgressStart).count();
                                std::string message = "[" +
                                    progress.stage + "] ";
                                if (progress.total != 0) {
                                    message += std::to_string(
                                        progress.completed) + "/" +
                                        std::to_string(progress.total) + " ";
                                }
                                message += progress.detail + " (" +
                                    std::to_string(elapsedMilliseconds) +
                                    " ms): " + sourcePath;
                                engineLog_.info(
                                    "Asset Cook", std::move(message));
                            };
                        // The cook runs on this reimport strand item
                        // (M7R R5b.2): a task never blocks on a cook
                        // task; this hot-reload DDC has no other requester.
                        DdcRequestResult cooked =
                            resolvePreparedCook(
                                *sourceContext
                                    ->cache,
                                prepared,
                                stopToken);
                        if ((cooked.status !=
                                DdcRequestStatus::Built &&
                             cooked.status !=
                                DdcRequestStatus::CacheHit) ||
                            !cooked.blob) {
                            throw std::runtime_error(
                                cookFailureMessage(
                                    "Source reimport cook failed",
                                    cooked
                                        .diagnostics));
                        }
                        (void)storePreparedCookReceipt(
                            *sourceContext->cache,
                            sourceContext
                                ->sourceRelativePath,
                            *prepared);
                        blob =
                            std::move(*cooked.blob);
                    } else {
                        blob =
                            readCookedArtifactBlobFile(
                                artifactPath);
                    }
                    CookedArtifactReadResult decoded =
                        readCookedArtifact(
                            blob.bytes,
                            blob.artifactHash);
                    if (!decoded.valid() ||
                        decoded.artifact->assetGuid !=
                            expectedGuid) {
                        throw std::runtime_error(
                            "Cooked model replacement container or GUID is invalid.");
                    }
                    CookedModelReadResult model =
                        readCookedModelProduct(
                            *decoded.artifact);
                    if (!model.valid()) {
                        throw std::runtime_error(
                            "Cooked model replacement product is invalid.");
                    }
                    if (stopToken.stop_requested()) {
                        throw std::runtime_error(
                            "Cooked model reimport cancelled.");
                    }
                    const auto [cpuBytes, gpuBytes] =
                        residentBytes(*model.data);
                    if (gpuBytes > EditorModelPublicationLimitBytes) {
                        throw std::runtime_error(
                            "Cooked model replacement exceeds the 1 GiB atomic model-publication safety cap.");
                    }
                    const std::string cookKey =
                        decoded.artifact->cookKey;
                    CookedArtifact artifact =
                        std::move(*decoded.artifact);
                    CookedModelProductData product =
                        std::move(*model.data);
                    return PreparedRuntimeAsset{
                        .cookKey = cookKey,
                        .estimatedUploadBytes =
                            gpuBytes,
                        .allowSingleOversizedUpload =
                            gpuBytes > EditorRuntimeUploadBudgetBytes,
                        .publish =
                            [this,
                                artifact =
                                    std::move(artifact),
                                product =
                                    std::move(product),
                                cpuBytes,
                                gpuBytes]() mutable {
                                try {
                                    mainModel_ =
                                        assetManager_->
                                            replaceSelfContainedModelFromCookedProduct(
                                                artifact,
                                                product);
                                    return RuntimeAssetPublishOutcome{
                                        .succeeded = true,
                                        .cpuResidentBytes =
                                            cpuBytes,
                                        .gpuResidentBytes =
                                            gpuBytes,
                                    };
                                } catch (const std::exception&
                                    exception) {
                                    return RuntimeAssetPublishOutcome{
                                        .diagnostic =
                                            exception.what(),
                                    };
                                }
                            },
                    };
                },
            .pinned = true,
        });
        assetRuntimeService_->adoptPublished(
            expectedGuid,
            baselineArtifact.artifact->cookKey,
            baselineCpuBytes,
            baselineGpuBytes);
    }

    void AssetIntegration::configureCookedEnvironmentHotReload() {
        if (activeCookedEnvironmentArtifact_.empty() ||
            activeEnvironmentAssetGuid_.isNil() ||
            !assetRuntimeService_) {
            return;
        }
        const CookedArtifactBlob baselineBlob =
            readCookedArtifactBlobFile(activeCookedEnvironmentArtifact_);
        const CookedArtifactReadResult baselineArtifact =
            readCookedArtifact(baselineBlob.bytes, baselineBlob.artifactHash);
        if (!baselineArtifact.valid() ||
            baselineArtifact.artifact->assetGuid !=
                activeEnvironmentAssetGuid_) {
            throw std::runtime_error(
                "Cooked environment hot-reload baseline container is invalid.");
        }
        const CookedEnvironmentReadResult baselineEnvironment =
            readCookedEnvironmentProduct(*baselineArtifact.artifact);
        if (!baselineEnvironment.valid()) {
            throw std::runtime_error(
                "Cooked environment hot-reload baseline product is invalid.");
        }
        const auto residentBytes = [](const CookedEnvironmentProductData& product) {
            return static_cast<uint64_t>(product.radiance.size()) +
                product.irradiance.size() +
                product.prefilteredSpecular.size() +
                product.brdfLut.size();
        };
        const uint64_t baselineGpuBytes =
            residentBytes(*baselineEnvironment.data);
        if (baselineGpuBytes > EditorEnvironmentPublicationLimitBytes)
            throw std::runtime_error(
                "Cooked environment hot-reload baseline exceeds the 640 MiB editor environment publication limit.");
        const AssetGuid expectedGuid = activeEnvironmentAssetGuid_;
        const std::filesystem::path artifactPath =
            activeCookedEnvironmentArtifact_;
        assetRuntimeService_->track({
            .assetGuid = expectedGuid,
            .sources = { TrackedSourceFile{
                artifactPath, baselineBlob.artifactHash } },
            .dependencies = baselineArtifact.artifact->dependencies,
            .prepare = [this, artifactPath, expectedGuid, residentBytes](
                const AssetReimportCause&, std::stop_token stopToken) {
                if (stopToken.stop_requested())
                    throw std::runtime_error(
                        "Cooked environment reimport cancelled.");
                CookedArtifactBlob blob =
                    readCookedArtifactBlobFile(artifactPath);
                CookedArtifactReadResult decoded =
                    readCookedArtifact(blob.bytes, blob.artifactHash);
                if (!decoded.valid() ||
                    decoded.artifact->assetGuid != expectedGuid)
                    throw std::runtime_error(
                        "Cooked environment replacement container or GUID is invalid.");
                CookedEnvironmentReadResult environment =
                    readCookedEnvironmentProduct(*decoded.artifact);
                if (!environment.valid())
                    throw std::runtime_error(
                        "Cooked environment replacement product is invalid.");
                if (stopToken.stop_requested())
                    throw std::runtime_error(
                        "Cooked environment reimport cancelled.");
                const uint64_t gpuBytes = residentBytes(*environment.data);
                if (gpuBytes > EditorEnvironmentPublicationLimitBytes)
                    throw std::runtime_error(
                        "Cooked environment replacement exceeds the 640 MiB editor environment publication limit.");
                const std::string cookKey = decoded.artifact->cookKey;
                CookedArtifact artifact = std::move(*decoded.artifact);
                return PreparedRuntimeAsset{
                    .cookKey = cookKey,
                    .estimatedUploadBytes = gpuBytes,
                    .allowSingleOversizedUpload = true,
                    .publish = [this, artifact = std::move(artifact),
                        gpuBytes]() mutable {
                        try {
                            LoadedEnvironmentAsset replacement =
                                assetManager_->loadEnvironmentFromCookedArtifact(
                                    artifact);
                            try {
                                renderBackend->setEnvironmentLighting(
                                    replacement.lighting);
                            } catch (...) {
                                assetManager_->releaseEnvironment(
                                    replacement.lighting);
                                throw;
                            }
                            const EnvironmentLightingHandles previous =
                                environmentLighting_;
                            environmentLighting_ = replacement.lighting;
                            activeEnvironmentAssetGuid_ = replacement.assetGuid;
                            activeEnvironmentSourceGuid_ =
                                replacement.manifest.sourceTextureGuid;
                            activeEnvironmentCookKey_ =
                                replacement.cookKey;
                            activeEnvironmentSourcePrimaries_ =
                                replacement.manifest.sourcePrimaries;
                            activeEnvironmentRadianceScale_ =
                                replacement.manifest.sourceRadianceScale;
                            loadedEnvironments_.insert_or_assign(
                                replacement.assetGuid,
                                std::move(replacement));
                            assetManager_->releaseEnvironment(previous);
                            return RuntimeAssetPublishOutcome{
                                .succeeded = true,
                                .cpuResidentBytes = 0,
                                .gpuResidentBytes = gpuBytes,
                            };
                        } catch (const std::exception& exception) {
                            return RuntimeAssetPublishOutcome{
                                .diagnostic = exception.what(),
                            };
                        }
                    },
                };
            },
            .pinned = true,
        });
        assetRuntimeService_->adoptPublished(
            expectedGuid, baselineArtifact.artifact->cookKey,
            0, baselineGpuBytes);
    }

    std::shared_ptr<ModelAsset> AssetIntegration::loadCookedStartupModel() {
        const std::filesystem::path artifactPath =
            config_.cookedModelArtifact.is_absolute()
            ? config_.cookedModelArtifact
            : std::filesystem::path(PROJECT_ROOT_DIR) /
                config_.cookedModelArtifact;
        activeCookedModelArtifact_ =
            artifactPath.lexically_normal();
        mainModel_ =
            assetManager_->
                loadSelfContainedModelFromCookedArtifactFile(
                    artifactPath);
        return mainModel_;
    }

    void AssetIntegration::loadCookedStartupEnvironment() {
        const std::filesystem::path environmentPath =
            config_.cookedEnvironmentArtifact.is_absolute()
            ? config_.cookedEnvironmentArtifact
            : std::filesystem::path(PROJECT_ROOT_DIR) /
                config_.cookedEnvironmentArtifact;
        activeCookedEnvironmentArtifact_ =
            environmentPath.lexically_normal();
        publishStartupEnvironment(assetManager_->
            loadEnvironmentFromCookedArtifactFile(environmentPath));
    }

    void AssetIntegration::publishStartupEnvironment(
        LoadedEnvironmentAsset environment) {
        environmentLighting_ = environment.lighting;
        activeEnvironmentAssetGuid_ = environment.assetGuid;
        activeEnvironmentSourceGuid_ =
            environment.manifest.sourceTextureGuid;
        activeEnvironmentCookKey_ = environment.cookKey;
        activeEnvironmentSourcePrimaries_ =
            environment.manifest.sourcePrimaries;
        activeEnvironmentRadianceScale_ =
            environment.manifest.sourceRadianceScale;
        loadedEnvironments_.insert_or_assign(
            environment.assetGuid, std::move(environment));
    }
} // namespace Iridium
