#include "editor/EditorAssetDocumentService.h"
#include "renderer/rhi/TransparencyQualityOverride.h"
#include "editor/EditorOrbitCamera.h"
#include "editor/EditorMaterialDraftHistory.h"
#include "editor/EditorViewCadence.h"
#include "editor/EditorAssetPartList.h"
#include "editor/EditorPreviewLighting.h"
#include "editor/EditorPreviewImageFit.h"
#include "editor/EditorAssetSessionTracker.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorTransactionService.h"
#include "scene/SceneWorld.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "check failed: " #condition \
                    << " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    Iridium::AssetGuid guid(std::string_view text) {
        const auto parsed = Iridium::AssetGuid::parse(text);
        if (!parsed) throw std::runtime_error("invalid test GUID");
        return *parsed;
    }

    bool previewImageFillPreservesAspectAndFov() {
        for (float source : {0.5f, 1.0f, 16.0f / 9.0f, 3.0f}) {
            for (float target : {0.4f, 1.0f, 2.5f}) {
                const auto fit = Iridium::previewImageFit(source, target);
                const glm::vec2 span = fit.uvMaximum - fit.uvMinimum;
                CHECK(span.x > 0 && span.y > 0 && span.x <= 1 && span.y <= 1);
                CHECK(std::abs(fit.uvMinimum.x + fit.uvMaximum.x - 1) < 0.00001f);
                // After cropping, the projection is exactly the target-aspect
                // projection with unchanged vertical FOV, not a stretched image.
                CHECK(std::abs(fit.projectionScale / span.y - 1) < 0.00001f);
                CHECK(std::abs(fit.projectionScale / source / span.x - 1 / target) < 0.00001f);
            }
        }
        return true;
    }

    bool previewLightingUsesLinearIndependentValues() {
        Iridium::EditorPreviewLighting first, second;
        first.environmentRotationDegrees = 180;
        first.lightingEv = 2;
        first.backgroundEv = -1;
        first.exposureEv = 3;
        const auto converted = first.environmentSettings();
        CHECK(std::abs(converted.rotationRadians - 3.14159265f) < 0.00001f);
        CHECK(converted.lightingIntensity == 4);
        CHECK(converted.backgroundIntensity == 0.5f);
        CHECK(converted.affectsLighting && converted.visibleToCamera);
        CHECK(second.environmentSettings().lightingIntensity == 1);
        CHECK(second.exposureEv == 0);
        // Output exposure does not multiply environment radiance a second time.
        first.exposureEv = -4;
        CHECK(first.environmentSettings().lightingIntensity == converted.lightingIntensity);
        return true;
    }

    bool layeredBudgetPreservesAuthorIntent() {
        using namespace Iridium;
        CompiledTransparencyPolicy authored;
        authored.requestedClass = TransparencyClass::Auto;
        authored.resolvedClass = TransparencyClass::LayeredGlass;
        authored.quality = TransparencyQuality::Hero4;
        authored.flags = CompiledTransparencyTopologyRequired;
        authored.priority = 7;
        authored.thinSheetThicknessMeters = .02f;
        for (const unsigned count : {2u, 4u, 8u}) {
            const auto effective = withLayeredInterfaceBudget(authored, count);
            CHECK(static_cast<unsigned>(effective.quality) == count);
            auto restored = effective;
            restored.quality = authored.quality;
            CHECK(restored == authored);
        }
        CHECK(withLayeredInterfaceBudget(authored, 0) == authored);
        CHECK(withLayeredInterfaceBudget(authored, 16) == authored);
        for (const auto type : {TransparencyClass::None, TransparencyClass::AlphaClip,
            TransparencyClass::ThinGlass, TransparencyClass::SortedSurface, TransparencyClass::WeightedOit}) {
            auto other = authored;
            other.resolvedClass = type;
            CHECK(withLayeredInterfaceBudget(other, 8) == other);
        }
        CHECK(authored.quality == TransparencyQuality::Hero4);
        return true;
    }

    bool viewSchedulingDoesNotStarveFocus() {
        using namespace std::chrono_literals;
        Iridium::EditorViewScheduler scheduler;
        Iridium::EditorViewCadence::TimePoint now{};
        // At 20 submissions/second, both views remain responsive rather than
        // giving every overdue turn to the background view.
        for (unsigned i = 0; i < 20; ++i) {
            const auto view = scheduler.choose(now, 1, true, true, 30);
            CHECK(view == i % 2);
            scheduler.rendered(view, now);
            now += 50ms;
        }
        scheduler.reset();
        now = {};
        CHECK(scheduler.choose(now, 1, true, true, 30) == 0);
        scheduler.rendered(0, now);
        CHECK(scheduler.choose(now + 1ms, 1, true, true, 30) == 1);
        scheduler.rendered(1, now + 1ms);
        CHECK(scheduler.choose(now + 2ms, 1, true, true, 30) == 1);
        CHECK(scheduler.choose(now + 34ms, 1, true, true, 30) == 0);
        // Choosing without a successful submission must not consume a turn.
        CHECK(scheduler.choose(now + 34ms, 1, true, true, 30) == 0);
        CHECK(scheduler.choose(now + 34ms, 1, true, false, 30) == 0);
        CHECK(scheduler.choose(now + 34ms, 0, false, true, 30) == 1);
        // Swap focus: the previously focused view is now background and must
        // yield the next turn to the new foreground view.
        CHECK(scheduler.choose(now + 100ms, 0, true, true, 30) == 0);
        return true;
    }

    bool draftGesturesAndViewCadence() {
        Iridium::EditorMaterialDraftHistory history;
        history.reset({{"roughness", 0.1}});
        history.observe({{"roughness", 0.2}}, true);
        history.observe({{"roughness", 0.4}}, true);
        history.observe({{"roughness", 0.6}}, false);
        CHECK(history.undo()->at("roughness") == 0.1);
        CHECK(!history.canUndo());
        CHECK(history.redo()->at("roughness") == 0.6);
        CHECK(history.undo().has_value());
        history.observe({{"roughness", 0.8}}, false);
        CHECK(!history.canRedo());
        history.reset({{"roughness", 1.0}});
        CHECK(!history.canUndo() && !history.canRedo());

        using namespace std::chrono_literals;
        Iridium::EditorViewCadence scene, viewer;
        const Iridium::EditorViewCadence::TimePoint start{};
        CHECK(scene.shouldRender(start, true, false));
        scene.rendered(start);
        viewer.rendered(start);
        CHECK(!scene.shouldRender(start + 10ms, true, false));
        CHECK(viewer.shouldRender(start + 10ms, true, true));
        CHECK(scene.shouldRender(start + 34ms, true, false));
        CHECK(!viewer.shouldRender(start + 34ms, false, true));
        scene.invalidate();
        CHECK(scene.shouldRender(start + 1ms, true, false));
        scene.rendered(start + 1ms);
        CHECK(!scene.shouldRender(start + 2ms, true, false));
        scene.setBackgroundFramesPerSecond(60);
        CHECK(scene.shouldRender(start + 2ms, true, false));
        scene.rendered(start + 2ms);
        CHECK(!scene.shouldRender(start + 18ms, true, false));
        CHECK(scene.shouldRender(start + 19ms, true, false));
        // No catch-up burst after a long stall: one successful render resets time.
        scene.rendered(start + 10s);
        CHECK(!scene.shouldRender(start + 10s + 1ms, true, false));
        bool rejected = false;
        try { scene.setBackgroundFramesPerSecond(0); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool sourcePartRowsAggregateRuntimePieces() {
        using namespace Iridium;
        ModelAsset model;
        const auto material = guid("0198d100-0000-7000-8000-000000000001");
        const auto primitive = guid("0198d100-0000-7000-8000-000000000002");
        SubMesh first;
        first.materialGuid = material;
        first.sourcePrimitiveGuid = primitive;
        first.indexCount = 6;
        first.boundsMin = glm::vec3(-1);
        first.boundsMax = glm::vec3(1);
        auto second = first;
        second.indexCount = 9;
        second.boundsMax = glm::vec3(3);
        second.transparency.resolvedClass = TransparencyClass::ThinGlass;
        model.subMeshes = {first, second};
        const auto rows = makeEditorAssetPartRows(model);
        CHECK(rows.size() == 2);
        for (const auto& row : rows) {
            CHECK(row.runtimePieces == 2);
            CHECK(row.triangles == 5);
            CHECK(row.transparent);
            CHECK(row.minimum == glm::vec3(-1));
            CHECK(row.maximum == glm::vec3(3));
        }
        CHECK(rows[0].material && rows[0].guid == material);
        CHECK(!rows[1].material && rows[1].guid == primitive);
        CHECK(rows[0].materials.empty());
        CHECK(rows[1].materials == std::vector<AssetGuid>{material});
        CHECK(model.subMeshes.size() == 2); // UI aggregation never merges geometry.
        const auto otherMaterial = guid("0198d100-0000-7000-8000-000000000003");
        model.subMeshes[1].materialGuid = otherMaterial;
        const auto mixedRows = makeEditorAssetPartRows(model);
        CHECK(mixedRows.size() == 3);
        CHECK((mixedRows[1].materials == std::vector<AssetGuid>{material, otherMaterial}));
        model.subMeshes[1].materialGuid = {};
        CHECK(makeEditorAssetPartRows(model)[1].materials == std::vector<AssetGuid>{material});
        return true;
    }

    bool draftControlsHaveIndependentUndoSteps() {
        using Json = nlohmann::json;
        Iridium::EditorMaterialDraftHistory history;
        const Json initial{{"roughness", .1}, {"metallic", .2}};
        const Json roughnessEdit{{"roughness", .6}, {"metallic", .2}};
        const Json bothEdited{{"roughness", .6}, {"metallic", .9}};
        history.reset(initial);
        history.observe({{"roughness", .3}, {"metallic", .2}}, 101, true);
        history.observe(roughnessEdit, 101);
        // No idle frame between two different controls: never merge them.
        history.observe(bothEdited, 202, true);
        history.observe(bothEdited, 0);
        CHECK(history.undo() == std::optional(roughnessEdit));
        CHECK(history.undo() == std::optional(initial));
        CHECK(!history.canUndo());
        CHECK(history.redo() == std::optional(roughnessEdit));
        CHECK(history.redo() == std::optional(bothEdited));
        CHECK(!history.canRedo());

        // Separate drags of the same slider are independent too.
        history.reset(initial);
        history.observe(roughnessEdit, 101, true);
        const Json secondDrag{{"roughness", .8}, {"metallic", .2}};
        history.observe(secondDrag, 101, true);
        history.observe(secondDrag, 0);
        CHECK(history.undo() == std::optional(roughnessEdit));
        CHECK(history.undo() == std::optional(initial));
        CHECK(history.redo() == std::optional(roughnessEdit));
        CHECK(history.redo() == std::optional(secondDrag));

        // Unrelated UI focus commits the previous edit, but creates no entry.
        history.reset(initial);
        history.observe(roughnessEdit, 101, true);
        history.observe(roughnessEdit, 999, true);
        history.observe(roughnessEdit, 0);
        CHECK(history.undo() == std::optional(initial));
        CHECK(!history.canUndo());
        return true;
    }

    bool viewerCachesRetireByOpeningSession() {
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        Iridium::EditorAssetDocumentService documents(&registry);
        Iridium::EditorAssetSessionTracker tracker;
        const auto model = guid("0198d100-0000-7000-8000-000000000001");
        const auto part = guid("0198d100-0000-7000-8000-000000000002");
        const Iridium::EditorAssetOpenRequest request{part, model, "iridium.model-primitive", "Lamp"};
        CHECK(documents.open(request));
        CHECK(!tracker.matches(*documents.active()));
        CHECK(tracker.synchronize(documents).empty());
        CHECK(tracker.matches(*documents.active()));
        CHECK(documents.activate(part));
        CHECK(tracker.synchronize(documents).empty());
        // Reusing an open document retains its local draft and camera state.
        CHECK(documents.open(request).reused);
        CHECK(tracker.synchronize(documents).empty());
        documents.closeAll();
        // No intermediate synchronize: the GUID is unchanged across this gap.
        CHECK(documents.open(request));
        CHECK(!tracker.matches(*documents.active()));
        const auto changed = tracker.synchronize(documents);
        CHECK(changed == std::vector<Iridium::AssetGuid>{part});
        CHECK(tracker.matches(*documents.active()));
        documents.closeAll();
        CHECK(tracker.synchronize(documents) == std::vector<Iridium::AssetGuid>{part});
        CHECK(tracker.synchronize(documents).empty());
        return true;
    }

    bool primitiveIsolationRetainsParentAndSelection() {
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        Iridium::EditorAssetDocumentService documents(&registry);
        const auto model = guid("0198d100-0000-7000-8000-000000000001");
        const auto part = guid("0198d100-0000-7000-8000-000000000002");
        const Iridium::EditorAssetOpenRequest request{part, model, "iridium.model-primitive", "Lamp"};
        CHECK(documents.open(request));
        CHECK(documents.active()->presentationAssetGuid == model);
        CHECK(documents.active()->selectedPart == part);
        CHECK(documents.active()->isolateSelectedPart);
        CHECK(documents.pinReferenceCount(model) == 1);
        const auto initialFraming = documents.active()->framingRevision;
        documents.selectPreviewPart(part, false, false);
        CHECK(documents.active()->framingRevision > initialFraming);
        CHECK(!documents.active()->isolateSelectedPart);
        const auto wholeModelFraming = documents.active()->framingRevision;
        CHECK(documents.open(request).reused);
        CHECK(documents.active()->framingRevision > wholeModelFraming);
        CHECK(documents.active()->isolateSelectedPart);
        CHECK(documents.pinReferenceCount(model) == 1);
        documents.selectPreviewPart(std::nullopt, false, true);
        CHECK(!documents.active()->isolateSelectedPart);
        CHECK(!documents.active()->selectedPart);
        // Material thumbnail double-click focuses all pieces using that material.
        documents.selectPreviewPart(part, true, true);
        CHECK(documents.active()->selectedPartIsMaterial);
        CHECK(documents.active()->isolateSelectedPart);
        const auto materialFraming = documents.active()->framingRevision;
        // Deselect restores whole-model visibility without releasing the document.
        documents.selectPreviewPart(std::nullopt, false, false);
        CHECK(!documents.active()->selectedPart);
        CHECK(!documents.active()->isolateSelectedPart);
        CHECK(!documents.active()->selectedPartIsMaterial);
        CHECK(documents.active()->framingRevision > materialFraming);
        CHECK(documents.pinReferenceCount(model) == 1);
        const auto previousSession = documents.active()->sessionSerial;
        documents.closeAll();
        CHECK(documents.open(request));
        CHECK(documents.active()->sessionSerial != previousSession);
        return true;
    }

    bool registrationIsExplicitAndFrozen() {
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        const auto registrations = registry.registrations();
        CHECK(registrations.size() == 3);
        CHECK(registrations[0].assetType == "iridium.material");
        CHECK(registrations[1].assetType == "iridium.model");
        CHECK(registry.find("iridium.model") != nullptr);
        CHECK(registry.find("iridium.material") != nullptr);
        CHECK(registry.find("iridium.model-primitive") != nullptr);
        CHECK(registry.find("iridium.texture") == nullptr);
        registry.freeze();
        CHECK(registry.frozen());
        bool rejected = false;
        try {
            registry.registerViewer({
                .assetType = "iridium.texture",
                .viewerId = "iridium.viewer.texture",
            });
        }
        catch (const std::logic_error&) {
            rejected = true;
        }
        CHECK(rejected);
        return true;
    }

    bool documentLifecyclePinsPresentationAssets() {
        const auto model = guid("0198d100-0000-7000-8000-000000000001");
        const auto material = guid("0198d100-0000-7000-8000-000000000002");
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        registry.freeze();
        Iridium::EditorAssetDocumentService documents(&registry);
        std::vector<std::pair<Iridium::AssetGuid, bool>> pins;
        documents.setRuntimePinCallback(
            [&pins](Iridium::AssetGuid assetGuid, bool pinned) {
                pins.emplace_back(assetGuid, pinned);
            });

        const auto openedModel = documents.open({
            .assetGuid = model,
            .assetType = "iridium.model",
            .displayName = "Roadster",
        });
        CHECK(openedModel && !openedModel.reused);
        CHECK(documents.documents().size() == 1);
        CHECK(documents.active()->assetGuid == model);
        CHECK(documents.pinReferenceCount(model) == 1);
        const std::vector<std::pair<Iridium::AssetGuid, bool>> expectedInitialPins{
            { model, true },
        };
        CHECK(pins == expectedInitialPins);

        const auto openedMaterial = documents.open({
            .assetGuid = material,
            .parentAssetGuid = model,
            .assetType = "iridium.material",
            .displayName = "Paint",
        });
        CHECK(openedMaterial && !openedMaterial.reused);
        CHECK(documents.documents().size() == 2);
        CHECK(documents.active()->assetGuid == material);
        CHECK(documents.active()->presentationAssetGuid == model);
        CHECK(documents.pinReferenceCount(model) == 2);
        CHECK(pins.size() == 1);

        CHECK(documents.activate(model));
        CHECK(documents.active()->assetGuid == model);
        const auto reopened = documents.open({
            .assetGuid = material,
            .parentAssetGuid = model,
            .assetType = "iridium.material",
            .displayName = "Paint renamed elsewhere",
        });
        CHECK(reopened && reopened.reused);
        CHECK(documents.documents().size() == 2);
        CHECK(documents.active()->assetGuid == material);

        documents.updateRuntimeState(model,
            Iridium::RuntimeAssetState::Failed, "cook failed");
        CHECK(documents.find(model)->runtimeState ==
            Iridium::RuntimeAssetState::Failed);
        CHECK(documents.find(material)->runtimeDiagnostic == "cook failed");
        documents.updateRuntimeState(model,
            Iridium::RuntimeAssetState::Ready, {});
        CHECK(documents.find(material)->runtimeState ==
            Iridium::RuntimeAssetState::Ready);
        CHECK(documents.find(material)->runtimeDiagnostic.empty());

        CHECK(documents.close(model));
        CHECK(documents.active()->assetGuid == material);
        CHECK(documents.pinReferenceCount(model) == 1);
        CHECK(pins.size() == 1);
        CHECK(documents.close(material));
        CHECK(documents.active() == nullptr);
        CHECK(documents.pinReferenceCount(model) == 0);
        CHECK(pins.size() == 2);
        const std::pair<Iridium::AssetGuid, bool> expectedRelease{ model, false };
        CHECK(pins.back() == expectedRelease);
        return true;
    }

    bool unsupportedAndParentlessAssetsAreRejected() {
        const auto asset = guid("0198d100-0000-7000-8000-000000000010");
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        registry.freeze();
        Iridium::EditorAssetDocumentService documents(&registry);
        CHECK(!documents.open({
            .assetGuid = asset,
            .assetType = "iridium.texture",
        }));
        CHECK(!documents.open({
            .assetGuid = asset,
            .assetType = "iridium.material",
        }));
        CHECK(documents.documents().empty());
        return true;
    }

    bool openingAssetsCannotMutateSceneState() {
        const auto model = guid("0198d100-0000-7000-8000-000000000020");
        Iridium::SceneWorld world;
        (void)world.createEntity();
        Iridium::EditorSceneDocumentService sceneDocument(world);
        Iridium::EditorTransactionService transactions(sceneDocument);
        const size_t entityCount = world.registry().aliveCount();
        const auto state = sceneDocument.currentState();
        const bool dirty = sceneDocument.dirty();
        const size_t history = transactions.historyEntryCount();

        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        registry.freeze();
        Iridium::EditorAssetDocumentService documents(&registry);
        CHECK(documents.open({
            .assetGuid = model,
            .assetType = "iridium.model",
            .displayName = "Isolated preview",
        }));
        CHECK(documents.close(model));

        CHECK(world.registry().aliveCount() == entityCount);
        CHECK(sceneDocument.currentState() == state);
        CHECK(sceneDocument.dirty() == dirty);
        CHECK(transactions.historyEntryCount() == history);
        CHECK(!transactions.canUndo());
        return true;
    }

    bool boundsFramingFitsWideAndTallViewports() {
        constexpr std::array<glm::vec3, 8> corners{
            glm::vec3{-2.0f, -1.0f, -0.5f},
            glm::vec3{-2.0f, -1.0f,  0.5f},
            glm::vec3{-2.0f,  1.0f, -0.5f},
            glm::vec3{-2.0f,  1.0f,  0.5f},
            glm::vec3{ 2.0f, -1.0f, -0.5f},
            glm::vec3{ 2.0f, -1.0f,  0.5f},
            glm::vec3{ 2.0f,  1.0f, -0.5f},
            glm::vec3{ 2.0f,  1.0f,  0.5f},
        };
        for (const float aspect : { 16.0f / 9.0f, 9.0f / 16.0f }) {
            Iridium::EditorOrbitCamera camera;
            camera.frameBounds(corners.front(), corners.back(), aspect);
            CHECK(glm::length(camera.state().target) < 1.0e-5f);
            CHECK(camera.state().nearPlane > 0.0f);
            CHECK(camera.state().farPlane > camera.state().nearPlane);
            const glm::mat4 viewProjection =
                camera.projectionMatrix(aspect) * camera.viewMatrix();
            for (const glm::vec3& corner : corners) {
                const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
                CHECK(clip.w > 0.0f);
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                CHECK(std::abs(ndc.x) <= 1.0f);
                CHECK(std::abs(ndc.y) <= 1.0f);
                CHECK(std::isfinite(ndc.z));
                CHECK(ndc.z >= 0.0f && ndc.z <= 1.0f);
            }
        }
        return true;
    }

    bool previewProjectionAndSurfaceDolly() {
        Iridium::EditorOrbitCamera camera;
        auto state = camera.state();
        state.nearPlane = .25f;
        state.farPlane = 100.0f;
        camera.setState(state);
        const auto projection = camera.projectionMatrix(1.5f);
        const auto nearClip = projection * glm::vec4(0, 0, -state.nearPlane, 1);
        const auto farClip = projection * glm::vec4(0, 0, -state.farPlane, 1);
        CHECK(std::abs(nearClip.z / nearClip.w) < 1.0e-5f);
        CHECK(std::abs(farClip.z / farClip.w - 1.0f) < 1.0e-5f);
        camera.frameBounds(glm::vec3(-1), glm::vec3(1), 1.5f);
        CHECK(camera.state().nearPlane <= .01f);
        float previousStep = 100000.0f;
        for (int i = 0; i < 100; ++i) {
            const auto before = camera.position();
            camera.dolly(1);
            const float step = glm::distance(before, camera.position());
            CHECK(step <= previousStep + 1.0e-5f);
            previousStep = step;
            const auto eye = glm::abs(camera.position());
            CHECK(std::max({eye.x, eye.y, eye.z}) > 1.0f);
            CHECK(camera.state().nearPlane > 0);
        }
        for (int i = 0; i < 36; ++i) {
            camera.orbit(40, 0);
            const auto eye = glm::abs(camera.position());
            CHECK(std::max({eye.x, eye.y, eye.z}) > 1.0f);
        }
        return true;
    }

    bool orbitPanAndDollyAreBounded() {
        Iridium::EditorOrbitCamera camera;
        camera.orbit(200.0f, 10000.0f);
        CHECK(camera.state().pitchDegrees == 89.0f);
        const glm::vec3 targetBefore = camera.state().target;
        camera.pan(20.0f, -10.0f, 720.0f);
        CHECK(glm::distance(targetBefore, camera.state().target) > 0.0f);
        const float distanceBefore = camera.state().distance;
        camera.dolly(3.0f);
        CHECK(camera.state().distance < distanceBefore);
        CHECK(camera.state().distance >= 0.01f);
        CHECK(camera.state().farPlane > camera.state().nearPlane);
        return true;
    }

} // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        { "viewer caches retire by opening session", viewerCachesRetireByOpeningSession },
        { "preview fills without stretching or letterboxing", previewImageFillPreservesAspectAndFov },
        { "preview lighting EV and isolation", previewLightingUsesLinearIndependentValues },
        { "source primitive row aggregation", sourcePartRowsAggregateRuntimePieces },
        { "independent per-control undo and redo", draftControlsHaveIndependentUndoSteps },
        { "private draft gestures and independent view cadence", draftGesturesAndViewCadence },
        { "view scheduling does not starve focus", viewSchedulingDoesNotStarveFocus },
        { "layered budget preserves author intent", layeredBudgetPreservesAuthorIntent },
        { "explicit frozen registration", registrationIsExplicitAndFrozen },
        { "primitive isolation retains parent", primitiveIsolationRetainsParentAndSelection },
        { "document lifecycle and runtime pins", documentLifecyclePinsPresentationAssets },
        { "unsupported assets", unsupportedAndParentlessAssetsAreRejected },
        { "scene isolation", openingAssetsCannotMutateSceneState },
        { "bounds framing", boundsFramingFitsWideAndTallViewports },
        { "Vulkan depth and surface-aware dolly", previewProjectionAndSurfaceDolly },
        { "orbit pan and dolly", orbitPanAndDollyAreBounded },
    };
    size_t passed = 0;
    for (const auto& test : tests) {
        try {
            if (test.run()) {
                ++passed;
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                std::cout << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            std::cout << "[FAIL] " << test.name << ": "
                << exception.what() << '\n';
        }
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
