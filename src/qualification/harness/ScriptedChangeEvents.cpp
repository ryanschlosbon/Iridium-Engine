// M7R R4c.0: applies a --qualification-scripted-changes scenario to the running
// benchmark (FrameBeginPhase::PreSceneUpdate, between frames) and records the
// per-frame drain timeline that tools/m7r/Analyze-Hitches.py reads. Without the
// flag every entry point returns immediately and the harness behaves exactly as
// before. Everything created here uses fixed, ordinal-derived positions and
// identities, so a scenario replays identically in every process.

#include "qualification/harness/QualificationHarness.h"
#include "qualification/harness/ScriptedChanges.h"

#include <glm/glm.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "assets/AssetManager.h"
#include "assets/cooker/CookTypes.h"
#include "assets/environment/EnvironmentConvolution.h"
#include "profiling/CpuProfiler.h"
#include "renderer/rhi/Mesh.h"
#include "scene/SceneWorld.h"
#include "scene/components/LightComponent.h"
#include "scene/components/MeshComponent.h"
#include "scene/components/NameComponent.h"
#include "scene/components/ReflectionProbeComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "utils/Sha256.h"

namespace Iridium {

    struct ScriptedChangeRun {
        std::filesystem::path path;
        ScriptedChangeScenario scenario;
        // Views scenario.events; the run is heap-allocated and never moves.
        ScriptedChangeSchedule schedule;
        ScriptedFrameTimeline timeline;
        std::vector<AppliedScriptedChange> applied;
        AssetManager* assets = nullptr;
        AssetGuid startupModelGuid;
        uint64_t warmupFrames = 0;
        // Fixture instances (never removed by the scenario).
        size_t fixtureInstanceCount = 0;
        std::vector<Entity> lights;
        uint32_t nextLightOrdinal = 0;
        std::vector<Entity> environmentProbes;
        uint32_t nextProbeOrdinal = 0;
        TextureHandle materialTexture{};
        std::vector<MaterialHandle> materials;
        std::optional<ReflectionProbeComponent> removedCaptureProbe;
        // One per PublishConstantEnvironment event, built at Ready.
        std::vector<LoadedEnvironmentAsset> environments;
        size_t publishedEnvironments = 0;
    };

    void ScriptedChangeRunDeleter::operator()(
        ScriptedChangeRun* run) const noexcept {
        delete run;
    }

    namespace {

        // UUIDv7 fields for scripted entities: distinct from the fixture's
        // generated lights (1'775'000'300'000 + i) and probes (…410'000).
        constexpr uint64_t kScriptedLightMilliseconds = 1'775'000'700'000ull;
        constexpr uint64_t kScriptedProbeMilliseconds = 1'775'000'800'000ull;
        constexpr uint64_t kScriptedEnvironmentMilliseconds =
            1'775'000'900'000ull;

        std::array<uint8_t, 10> ordinalBytes(uint32_t ordinal, uint8_t tag) {
            std::array<uint8_t, 10> bytes{};
            for (size_t byte = 0; byte < 4u; ++byte)
                bytes[byte] = static_cast<uint8_t>(ordinal >> (byte * 8u));
            bytes[8] = 0x52;  // 'R'
            bytes[9] = tag;
            return bytes;
        }

        // Scripted instances and lights sit on a lattice behind the camera's
        // near plane (the fixture cameras are static), so they change GPU
        // capacities and per-frame CPU/compute work but not the rendered
        // image or its shading cost. The steady frame then stays comparable
        // across the run and the 2x-median hitch threshold stays meaningful.
        struct BehindCamera {
            glm::vec3 origin{ 0.0f };
            glm::vec3 back{ 0.0f, 0.0f, 1.0f };
            glm::vec3 right{ 1.0f, 0.0f, 0.0f };
            glm::vec3 up{ 0.0f, 1.0f, 0.0f };

            BehindCamera(glm::vec3 position, glm::vec3 front, glm::vec3 worldUp)
                : origin(position) {
                const glm::vec3 forward = glm::normalize(front);
                back = -forward;
                right = glm::normalize(glm::cross(forward, worldUp));
                up = glm::normalize(glm::cross(right, forward));
            }
            // `depth` meters behind the camera along its view axis.
            [[nodiscard]] glm::vec3 at(float depth, float lateral,
                float lift) const {
                return origin + back * depth + right * lateral + up * lift;
            }
        };
        constexpr float kBehindCameraDepthMeters = 25.0f;

        bool requiresInstances(ScriptedChangeAction action) {
            return action == ScriptedChangeAction::AddInstances ||
                action == ScriptedChangeAction::RemoveInstances;
        }

        bool requiresProbes(ScriptedChangeAction action) {
            switch (action) {
            case ScriptedChangeAction::AddEnvironmentProbes:
            case ScriptedChangeAction::RemoveEnvironmentProbes:
            case ScriptedChangeAction::RemoveCaptureProbe:
            case ScriptedChangeAction::AddCaptureProbe:
            case ScriptedChangeAction::SetCaptureProbeResolution:
                return true;
            default:
                return false;
            }
        }

    } // namespace

    void QualificationHarness::loadScriptedChanges(AppStartupContext&) {
        if (options_.scriptedChanges.empty()) return;
        if (options_.cpuProfileOutput.empty())
            throw std::invalid_argument(
                "--qualification-scripted-changes requires --profile-cpu-output");
        std::unique_ptr<ScriptedChangeRun, ScriptedChangeRunDeleter> run(
            new ScriptedChangeRun());
        run->path = options_.scriptedChanges;
        run->scenario = loadScriptedChangeScenario(run->path);
        run->schedule = ScriptedChangeSchedule(run->scenario.events);
        scripted_ = std::move(run);
    }

    void QualificationHarness::prepareScriptedChanges(
        AppStartupContext& context) {
        if (!scripted_) return;
        ScriptedChangeRun& run = *scripted_;
        const ApplicationConfig& config = context.config;
        if (config.frameLimit == 0)
            throw std::invalid_argument(
                "--qualification-scripted-changes requires a bounded --frame-limit");
        if (!context.profiler.isEnabled())
            throw std::invalid_argument(
                "--qualification-scripted-changes requires CPU profiling");
        for (const ScriptedChangeEvent& event : run.scenario.events) {
            if (event.measuredFrame >= config.frameLimit)
                throw std::invalid_argument(
                    "scripted changes: an event is scheduled at measured frame " +
                    std::to_string(event.measuredFrame) +
                    ", beyond the frame limit");
            if (requiresInstances(event.action) &&
                (!benchmark_ || !context.mainModel ||
                    benchmark_->sceneFactory.renderInstanceBatch))
                throw std::invalid_argument(
                    "scripted changes: instance events require an instanced-grid "
                    "benchmark with a startup model");
            if (requiresProbes(event.action) &&
                (environmentProbeEntity_ == NULL_ENTITY ||
                    captureProbeEntity_ == NULL_ENTITY))
                throw std::invalid_argument(
                    "scripted changes: probe events require "
                    "--validate-reflection-probes");
            if (event.action ==
                    ScriptedChangeAction::PublishConstantEnvironment &&
                context.assets == nullptr)
                throw std::logic_error(
                    "scripted changes: no AssetManager for environment events");
        }
        run.assets = context.assets;
        run.startupModelGuid = context.startupModelGuid;
        run.warmupFrames = config.warmupFrameCount;
        run.fixtureInstanceCount = benchmarkInstances_.size();
        run.applied.reserve(run.scenario.events.size());
        // Environment products are built and uploaded now; the event only
        // publishes them (the renderer-side change R4c targets, without the
        // CPU convolution of a freshly cooked product).
        for (const ScriptedChangeEvent& event : run.scenario.events) {
            if (event.action != ScriptedChangeAction::PublishConstantEnvironment)
                continue;
            const uint32_t ordinal =
                static_cast<uint32_t>(run.environments.size());
            const AssetGuid guid = AssetGuid::fromUuidV7Fields(
                kScriptedEnvironmentMilliseconds + ordinal,
                ordinalBytes(ordinal, 0x45));
            const glm::vec3 color{ event.color[0], event.color[1],
                event.color[2] };
            const CookProduct product =
                makeConstantEnvironmentProduct(guid, color);
            if (hasCookErrors(product.diagnostics))
                throw std::runtime_error(
                    "scripted changes: constant environment is invalid");
            const std::string recipe =
                "iridium.qualification.scripted_constant_environment.v1/" +
                std::to_string(std::bit_cast<uint32_t>(color.x)) + "/" +
                std::to_string(std::bit_cast<uint32_t>(color.y)) + "/" +
                std::to_string(std::bit_cast<uint32_t>(color.z));
            run.environments.push_back(
                context.assets->loadEnvironmentFromCookedArtifact({
                    .assetGuid = guid,
                    .artifactType = product.artifactType,
                    .artifactSchemaVersion = product.artifactSchemaVersion,
                    .cookKey = sha256(std::as_bytes(std::span(recipe))),
                    .sections = product.sections,
                }));
        }
        // One sample per measured frame, plus slack for the final frame.
        run.timeline.reserve(static_cast<size_t>(config.frameLimit) + 2u);
        std::cout << "IRIDIUM_SCRIPTED_CHANGES {\"scenario_id\":\""
            << run.scenario.id << "\",\"events\":"
            << run.scenario.events.size() << "}\n" << std::flush;
    }

    void QualificationHarness::updateScriptedChanges(
        AppFrameContext& context) {
        if (!scripted_) return;
        ScriptedChangeRun& run = *scripted_;
        // The profiler's latest completed frame is the previous frame here.
        (void)run.timeline.observe(context.profiler);
        if (!context.measuredFrameIndex) return;
        const uint64_t measured = *context.measuredFrameIndex;
        const std::span<const ScriptedChangeEvent> due =
            run.schedule.take(measured);
        if (due.empty()) return;
        const size_t first = run.schedule.issuedCount() - due.size();
        for (size_t offset = 0; offset < due.size(); ++offset) {
            const size_t eventIndex = first + offset;
            const auto start = std::chrono::steady_clock::now();
            const uint64_t result = applyScriptedChange(eventIndex, context);
            const AppliedScriptedChange applied{
                .eventIndex = eventIndex,
                .measuredFrame = measured,
                .applicationFrame = context.applicationFrameIndex,
                .applyNanoseconds = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start).count()),
                .result = result,
            };
            run.applied.push_back(applied);
            std::cout << "IRIDIUM_SCRIPTED_CHANGE "
                << scriptedChangeJson(run.scenario.events[eventIndex], applied)
                << '\n' << std::flush;
        }
    }

    uint64_t QualificationHarness::applyScriptedChange(size_t eventIndex,
        AppFrameContext& context) {
        ScriptedChangeRun& run = *scripted_;
        const ScriptedChangeEvent& event = run.scenario.events[eventIndex];
        SceneWorld& scene = context.scene;
        Registry& registry = scene.registry();
        IRenderBackend& backend = context.backend;

        const BehindCamera behind = benchmark_
            ? BehindCamera(benchmark_->camera.position,
                benchmark_->camera.target - benchmark_->camera.position,
                benchmark_->camera.up)
            : BehindCamera(context.camera.position, context.camera.front,
                context.camera.up);

        switch (event.action) {
        case ScriptedChangeAction::AddInstances: {
            // Rows of 16 startup-model copies (3 m apart, rows 5 m apart)
            // behind the camera. The slot follows the scripted ordinal, so the
            // next addition reuses a removed instance's slot.
            const BenchmarkSceneFactory& factory = benchmark_->sceneFactory;
            benchmarkInstances_.reserve(benchmarkInstances_.size() + event.count);
            for (uint32_t added = 0; added < event.count; ++added) {
                const uint32_t ordinal =
                    static_cast<uint32_t>(benchmarkInstances_.size());
                const uint32_t scripted = ordinal -
                    static_cast<uint32_t>(run.fixtureInstanceCount);
                const glm::vec3 position = behind.at(
                    kBehindCameraDepthMeters +
                        5.0f * static_cast<float>(scripted / 16u),
                    3.0f * (static_cast<float>(scripted % 16u) - 7.5f), 0.0f);
                const Entity entity = registry.createEntity();
                registry.addComponent<NameComponent>(entity).name =
                    "Benchmark Model (" + std::to_string(ordinal + 1u) + ")";
                auto& transform =
                    registry.addComponent<TransformComponent>(entity);
                transform.position = position;
                transform.rotation = glm::vec3(0.0f);
                transform.scale = factory.instanceScale;
                transform.worldMatrix = glm::mat4(1.0f);
                transform.isDirty = true;
                registry.addComponent<RelationshipComponent>(entity)
                    .siblingOrder = static_cast<int32_t>(ordinal);
                auto& mesh = registry.addComponent<MeshComponent>(entity);
                mesh.model = context.mainModel;
                mesh.assetGuid = run.startupModelGuid;
                mesh.requestedAssetGuid = run.startupModelGuid;
                mesh.enabled = true;
                benchmarkInstances_.push_back({ entity, position });
            }
            return benchmarkInstances_.size();
        }
        case ScriptedChangeAction::RemoveInstances: {
            if (benchmarkInstances_.size() - run.fixtureInstanceCount <
                event.count)
                throw std::logic_error(
                    "scripted changes: remove_instances exceeds the scripted instances");
            for (uint32_t removed = 0; removed < event.count; ++removed) {
                (void)scene.destroyEntity(benchmarkInstances_.back().entity);
                benchmarkInstances_.pop_back();
            }
            return benchmarkInstances_.size();
        }
        case ScriptedChangeAction::AddLights: {
            // Unshadowed 6 m point lights behind the camera (rows of 32), out
            // of reach of the view frustum.
            run.lights.reserve(run.lights.size() + event.count);
            for (uint32_t added = 0; added < event.count; ++added) {
                const uint32_t ordinal = run.nextLightOrdinal++;
                const Entity entity = scene.createEntity(
                    SceneEntityUuid::fromUuidV7Fields(
                        kScriptedLightMilliseconds + ordinal,
                        ordinalBytes(ordinal, 0x4c)));
                registry.addComponent<NameComponent>(entity).name =
                    "Scripted Light " + std::to_string(ordinal);
                auto& transform =
                    registry.addComponent<TransformComponent>(entity);
                transform.position = behind.at(
                    kBehindCameraDepthMeters +
                        2.5f * static_cast<float>(ordinal / 32u),
                    1.5f * (static_cast<float>(ordinal % 32u) - 15.5f), 1.5f);
                transform.isDirty = true;
                registry.addComponent<RelationshipComponent>(entity)
                    .siblingOrder = static_cast<int32_t>(20'000u + ordinal);
                auto& light = registry.addComponent<LightComponent>(entity);
                light.type = LightType::Point;
                light.colorLinearRec709 = { 1.0f, 0.82f, 0.6f };
                light.luminousIntensityCandela = 2'000.0f;
                light.rangeMeters = 6.0f;
                light.castsShadows = false;
                run.lights.push_back(entity);
            }
            return run.lights.size();
        }
        case ScriptedChangeAction::RemoveLights: {
            if (run.lights.size() < event.count)
                throw std::logic_error(
                    "scripted changes: remove_lights exceeds the scripted lights");
            for (uint32_t removed = 0; removed < event.count; ++removed) {
                (void)scene.destroyEntity(run.lights.back());
                run.lights.pop_back();
            }
            return run.lights.size();
        }
        case ScriptedChangeAction::AddMaterials: {
            if (!run.materialTexture.isValid()) {
                constexpr std::array<std::byte, 4> pixel{ std::byte{ 0xc0 },
                    std::byte{ 0xc0 }, std::byte{ 0xc0 }, std::byte{ 0xff } };
                run.materialTexture = backend.allocateTexture(TextureDesc{
                    .width = 1,
                    .height = 1,
                    .format = TextureFormat::RGBA8_UNorm,
                }, pixel);
            }
            CanonicalMaterialAsset material{};
            material.name = "m7r-r4c0-scripted-material";
            material.packed.closureClass = static_cast<uint32_t>(
                MaterialClosureClass::StandardDeferred);
            material.packed.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
            material.packed.metallicRoughnessIorSpecular = {
                0.0f, 0.5f, 1.5f, 1.0f };
            material.packed.specularColorNormalScale = {
                1.0f, 1.0f, 1.0f, 1.0f };
            material.packed.diffuseFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
            material.packed.specularGlossinessFactorGloss = {
                1.0f, 1.0f, 1.0f, 1.0f };
            material.packed.emissiveFactorStrength = {
                0.0f, 0.0f, 0.0f, 1.0f };
            material.packed.surfaceParameters = { 1.0f, 0.5f, 0.0f, 0.0f };
            material.textures.fill(run.materialTexture);
            material.packed.textureIndices.fill(
                run.materialTexture.getIndex());
            run.materials.reserve(run.materials.size() + event.count);
            for (uint32_t added = 0; added < event.count; ++added)
                run.materials.push_back(
                    backend.allocateCanonicalMaterial(material).material);
            return run.materials.size();
        }
        case ScriptedChangeAction::AddEnvironmentProbes: {
            const ReflectionProbeComponent& source =
                registry.getComponent<ReflectionProbeComponent>(
                    environmentProbeEntity_);
            const AssetGuid environment = source.environmentAssetGuid;
            run.environmentProbes.reserve(
                run.environmentProbes.size() + event.count);
            for (uint32_t added = 0; added < event.count; ++added) {
                const uint32_t ordinal = run.nextProbeOrdinal++;
                const Entity entity = scene.createEntity(
                    SceneEntityUuid::fromUuidV7Fields(
                        kScriptedProbeMilliseconds + ordinal,
                        ordinalBytes(ordinal, 0x50)));
                registry.addComponent<NameComponent>(entity).name =
                    "Scripted Reflection Probe " + std::to_string(ordinal);
                auto& transform =
                    registry.addComponent<TransformComponent>(entity);
                transform.position = {
                    (static_cast<float>(ordinal % 8u) - 3.5f) * 1.5f,
                    1.0f,
                    (static_cast<float>((ordinal / 8u) % 8u) - 3.5f) * 1.5f,
                };
                transform.isDirty = true;
                registry.addComponent<RelationshipComponent>(entity)
                    .siblingOrder = static_cast<int32_t>(30'000u + ordinal);
                auto& probe =
                    registry.addComponent<ReflectionProbeComponent>(entity);
                probe.shape = ReflectionProbeShape::Sphere;
                probe.sphereRadiusMeters = 2.0f;
                probe.blendDistanceMeters = 0.5f;
                probe.parallaxMode = ReflectionProbeParallaxMode::None;
                probe.environmentAssetGuid = environment;
                probe.resolvedEnvironmentAssetGuid = environment;
                run.environmentProbes.push_back(entity);
            }
            return run.environmentProbes.size();
        }
        case ScriptedChangeAction::RemoveEnvironmentProbes: {
            if (run.environmentProbes.size() < event.count)
                throw std::logic_error(
                    "scripted changes: remove_environment_probes exceeds the scripted probes");
            for (uint32_t removed = 0; removed < event.count; ++removed) {
                (void)scene.destroyEntity(run.environmentProbes.back());
                run.environmentProbes.pop_back();
            }
            return run.environmentProbes.size();
        }
        case ScriptedChangeAction::RemoveCaptureProbe: {
            auto* probes = registry.findPool<ReflectionProbeComponent>();
            if (run.removedCaptureProbe || probes == nullptr ||
                !probes->has(captureProbeEntity_))
                throw std::logic_error(
                    "scripted changes: the capture probe is already removed");
            run.removedCaptureProbe = probes->get(captureProbeEntity_);
            probes->remove(captureProbeEntity_);
            return 0;
        }
        case ScriptedChangeAction::AddCaptureProbe: {
            if (!run.removedCaptureProbe)
                throw std::logic_error(
                    "scripted changes: add_capture_probe without a removed capture probe");
            registry.addComponent<ReflectionProbeComponent>(captureProbeEntity_,
                *run.removedCaptureProbe);
            run.removedCaptureProbe.reset();
            return 1;
        }
        case ScriptedChangeAction::SetCaptureProbeResolution: {
            auto* probes = registry.findPool<ReflectionProbeComponent>();
            if (probes == nullptr || !probes->has(captureProbeEntity_))
                throw std::logic_error(
                    "scripted changes: set_capture_probe_resolution needs the capture probe");
            probes->get(captureProbeEntity_).captureResolution =
                static_cast<int32_t>(event.count);
            return event.count;
        }
        case ScriptedChangeAction::PublishConstantEnvironment: {
            // The environment prebuilt at Ready becomes the scene environment
            // and a new loaded environment (the probe environment table
            // grows); the Application owns it from here.
            if (run.publishedEnvironments >= run.environments.size())
                throw std::logic_error(
                    "scripted changes: no prebuilt environment to publish");
            context.control.publishStartupEnvironment(
                std::move(run.environments[run.publishedEnvironments]));
            return ++run.publishedEnvironments;
        }
        }
        throw std::logic_error("scripted changes: unknown action");
    }

    void QualificationHarness::finishScriptedChanges(
        AppShutdownContext& context) {
        if (!scripted_) return;
        // The last measured frame completed after its final PreSceneUpdate.
        (void)scripted_->timeline.observe(context.profiler);
        if (!scripted_->schedule.finished())
            std::cerr << "IRIDIUM_SCRIPTED_CHANGES_INCOMPLETE {\"issued\":"
                << scripted_->schedule.issuedCount() << ",\"scheduled\":"
                << scripted_->scenario.events.size() << "}\n";
    }

    void QualificationHarness::releaseScriptedChangeResources(
        AppShutdownContext& context) {
        if (!scripted_ || context.backend == nullptr) return;
        // Environments never published (a run that ended early) are still
        // the harness's.
        for (size_t index = scripted_->publishedEnvironments;
            index < scripted_->environments.size(); ++index)
            if (scripted_->assets != nullptr)
                scripted_->assets->releaseEnvironment(
                    scripted_->environments[index].lighting);
        scripted_->environments.clear();
        scripted_->publishedEnvironments = 0;
        for (MaterialHandle material : scripted_->materials)
            context.backend->freeMaterial(material);
        scripted_->materials.clear();
        if (scripted_->materialTexture.isValid()) {
            context.backend->freeTexture(scripted_->materialTexture);
            scripted_->materialTexture = {};
        }
    }

    void QualificationHarness::appendScriptedChangeRecords(
        const AppShutdownContext&) const {
        if (!scripted_ || options_.cpuProfileOutput.empty()) return;
        std::ofstream output(options_.cpuProfileOutput,
            std::ios::binary | std::ios::app);
        if (!output)
            throw std::runtime_error(
                "Unable to append scripted-change records to the CPU profile");
        writeScriptedChangeJsonLines(output, scripted_->scenario,
            scripted_->path.generic_string(), scripted_->warmupFrames,
            scripted_->applied, scripted_->timeline.samples(),
            scripted_->timeline.slowFrames());
        if (!output)
            throw std::runtime_error(
                "Unable to append scripted-change records to the CPU profile");
    }

} // namespace Iridium
