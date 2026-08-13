#include "editor/EditorAssetDocumentService.h"
#include "editor/EditorOrbitCamera.h"
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

    bool registrationIsExplicitAndFrozen() {
        Iridium::EditorAssetViewerRegistry registry;
        Iridium::registerCoreAssetViewers(registry);
        const auto registrations = registry.registrations();
        CHECK(registrations.size() == 2);
        CHECK(registrations[0].assetType == "iridium.material");
        CHECK(registrations[1].assetType == "iridium.model");
        CHECK(registry.find("iridium.model") != nullptr);
        CHECK(registry.find("iridium.material") != nullptr);
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
            }
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
        { "explicit frozen registration", registrationIsExplicitAndFrozen },
        { "document lifecycle and runtime pins", documentLifecyclePinsPresentationAssets },
        { "unsupported assets", unsupportedAndParentlessAssetsAreRejected },
        { "scene isolation", openingAssetsCannotMutateSceneState },
        { "bounds framing", boundsFramingFitsWideAndTallViewports },
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
