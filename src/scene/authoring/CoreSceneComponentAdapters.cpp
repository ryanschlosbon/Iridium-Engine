#include "scene/authoring/CoreSceneComponentAdapters.h"

#include "assets/AssetGuid.h"
#include "ecs/Registry.h"
#include "renderer/rhi/Mesh.h"
#include "scene/Components.h"
#include "scene/SceneIdentityMap.h"
#include "scene/runtime/CookedComponentIO.h"
#include "scene/runtime/CoreComponentIds.h"
#include "scene/runtime/SceneReferenceState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

namespace Iridium {
    namespace {

        template <typename T>
        [[nodiscard]] const T* component(const Registry& registry, Entity entity) {
            const auto* pool = registry.findPool<T>();
            return pool && pool->has(entity) ? &pool->get(entity) : nullptr;
        }

        [[nodiscard]] bool objectData(const SourceJson& data, std::string& error) {
            if (data.is_object()) return true;
            error = "Component data must be an object";
            return false;
        }

        [[nodiscard]] bool finite(glm::vec3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] glm::vec3 vectorValue(const SourceJson& data,
            std::string_view name, glm::vec3 fallback) {
            const auto found = data.find(std::string(name));
            if (found == data.end()) return fallback;
            return { found->at(0).get<float>(), found->at(1).get<float>(),
                found->at(2).get<float>() };
        }

        void ensureObject(SourceJson& data) {
            if (!data.is_object()) data = SourceJson::object();
        }

        bool serializeName(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<NameComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["value"] = value->name;
            return true;
        }

        bool deserializeName(Registry& registry, Entity entity,
            const SourceJson& data, std::string&) {
            registry.addComponent<NameComponent>(entity,
                data.at("value").get<std::string>());
            return true;
        }

        bool serializeTransform(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<TransformComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["position"] = { value->position.x, value->position.y,
                value->position.z };
            data["rotation"] = { value->rotation.x, value->rotation.y,
                value->rotation.z };
            data["scale"] = { value->scale.x, value->scale.y, value->scale.z };
            return true;
        }

        bool deserializeTransform(Registry& registry, Entity entity,
            const SourceJson& data, std::string&) {
            auto& value = registry.addComponent<TransformComponent>(entity);
            value.position = vectorValue(data, "position", {});
            value.rotation = vectorValue(data, "rotation", {});
            value.scale = vectorValue(data, "scale", { 1.0f, 1.0f, 1.0f });
            value.isDirty = true;
            return true;
        }

        bool serializeRelationship(const Registry& registry, Entity entity,
            const SceneIdentityMap& identities, SourceJson& data,
            std::string& error) {
            const auto* value = component<RelationshipComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            if (value->parent == NULL_ENTITY) data["parent"] = nullptr;
            else {
                const auto parent = identities.persistentId(value->parent);
                if (!parent) {
                    error = "Relationship parent has no persistent scene UUID";
                    return false;
                }
                data["parent"] = parent->toString();
            }
            if (value->siblingOrder < 0) {
                error = "Relationship sibling order cannot be negative";
                return false;
            }
            data["siblingOrder"] = static_cast<uint32_t>(value->siblingOrder);
            return true;
        }

        bool deserializeRelationship(Registry& registry, Entity entity,
            const SourceJson& data, std::string&) {
            auto& value = registry.addComponent<RelationshipComponent>(entity);
            value.parent = NULL_ENTITY;
            value.children.clear();
            value.depth = 0;
            value.siblingOrder = data.value("siblingOrder", 0);
            return true;
        }

        bool serializeMesh(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<MeshComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["enabled"] = value->enabled;
            const AssetGuid model = !value->requestedAssetGuid.isNil()
                ? value->requestedAssetGuid
                : !value->assetGuid.isNil()
                    ? value->assetGuid
                    : value->model ? value->model->assetGuid : AssetGuid{};
            data["model"] = model.isNil()
                ? SourceJson(nullptr)
                : SourceJson{ { "assetGuid", model.toString() } };

            std::vector<MeshComponent::MaterialOverride> overrides =
                value->materialOverrides;
            std::ranges::sort(overrides,
                [](const auto& lhs, const auto& rhs) {
                    return lhs.sourceMaterialGuid < rhs.sourceMaterialGuid;
                });
            SourceJson serialized = SourceJson::array();
            for (const auto& entry : overrides) {
                if (entry.sourceMaterialGuid.isNil() ||
                    entry.materialGuid.isNil()) continue;
                serialized.push_back({
                    { "source", {{ "subassetGuid",
                        entry.sourceMaterialGuid.toString() }} },
                    { "replacement", {{ "subassetGuid",
                        entry.materialGuid.toString() }} },
                });
            }
            data["materialOverrides"] = std::move(serialized);
            return true;
        }

        bool deserializeMesh(Registry& registry, Entity entity,
            const SourceJson& data, std::string& error) {
            auto& value = registry.addComponent<MeshComponent>(entity);
            value.enabled = data.value("enabled", true);
            value.model.reset();
            value.assetGuid = {};
            value.requestedAssetGuid = {};
            value.materialOverrides.clear();
            if (const auto model = data.find("model");
                model != data.end() && !model->is_null()) {
                const auto guid = AssetGuid::parse(
                    model->at("assetGuid").get<std::string>());
                if (!guid) { error = "Mesh model asset GUID is invalid"; return false; }
                value.assetGuid = *guid;
                value.requestedAssetGuid = *guid;
            }
            if (const auto overrides = data.find("materialOverrides");
                overrides != data.end()) {
                for (const SourceJson& entry : *overrides) {
                    const auto source = AssetGuid::parse(entry.at("source")
                        .at("subassetGuid").get<std::string>());
                    const auto replacement = AssetGuid::parse(entry.at("replacement")
                        .at("subassetGuid").get<std::string>());
                    if (!source || !replacement) {
                        error = "Mesh material override GUID is invalid";
                        return false;
                    }
                    value.materialOverrides.push_back({ *source, *replacement });
                }
            }
            return true;
        }

        bool validateMeshSource(const SourceJson& data, std::string& error) {
            if (!objectData(data, error)) return false;
            constexpr std::array removedPaths{
                "meshPath", "mesh_path", "currentMeshPath",
                "requestedMeshPath", "requestedAssetSourcePath",
            };
            for (std::string_view field : removedPaths) {
                if (data.contains(std::string(field))) {
                    error = "Removed path-based mesh field is not supported: " +
                        std::string(field);
                    return false;
                }
            }
            return true;
        }

        bool serializeLight(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<LightComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["type"] = static_cast<int32_t>(value->type);
            data["colorLinearRec709"] = { value->colorLinearRec709.x,
                value->colorLinearRec709.y, value->colorLinearRec709.z };
            data["illuminanceLux"] = value->illuminanceLux;
            data["luminousIntensityCandela"] = value->luminousIntensityCandela;
            data["rangeMeters"] = value->rangeMeters;
            data["sourceRadiusMeters"] = value->sourceRadiusMeters;
            data["innerConeDegrees"] = value->innerConeDegrees;
            data["outerConeDegrees"] = value->outerConeDegrees;
            data["castsShadows"] = value->castsShadows;
            data["shadowQuality"] = static_cast<int32_t>(value->shadowQuality);
            data["priority"] = value->priority;
            return true;
        }

        bool deserializeLight(Registry& registry, Entity entity,
            const SourceJson& data, std::string&) {
            auto& value = registry.addComponent<LightComponent>(entity);
            value.type = static_cast<LightType>(data.value("type", int32_t{ 0 }));
            value.colorLinearRec709 = vectorValue(data,
                "colorLinearRec709", { 1.0f, 1.0f, 1.0f });
            value.illuminanceLux = data.value("illuminanceLux", 1.0f);
            value.luminousIntensityCandela = data.value(
                "luminousIntensityCandela", 1.0f);
            value.rangeMeters = data.value("rangeMeters", 10.0f);
            value.sourceRadiusMeters = data.value("sourceRadiusMeters", 0.5f);
            value.innerConeDegrees = data.value("innerConeDegrees", 12.5f);
            value.outerConeDegrees = data.value("outerConeDegrees", 45.0f);
            value.castsShadows = data.value("castsShadows", true);
            value.shadowQuality = static_cast<LightShadowQuality>(
                data.value("shadowQuality", int32_t{ 2 }));
            value.priority = data.value("priority", int32_t{ 0 });
            return true;
        }

        SourceJson assetReference(AssetGuid guid) {
            return guid.isNil()
                ? SourceJson(nullptr)
                : SourceJson{ { "assetGuid", guid.toString() } };
        }

        bool readAssetReference(const SourceJson& data, std::string_view name,
            AssetGuid& result, std::string& error) {
            result = {};
            const auto found = data.find(std::string(name));
            if (found == data.end() || found->is_null()) return true;
            const auto guid = AssetGuid::parse(
                found->at("assetGuid").get<std::string>());
            if (!guid) {
                error = std::string(name) + " asset GUID is invalid";
                return false;
            }
            result = *guid;
            return true;
        }

        bool serializeSky(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<SkyComponent>(registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["enabled"] = value->enabled;
            data["mode"] = static_cast<int32_t>(value->mode);
            data["skyboxAsset"] = assetReference(value->skybox.cubemapAssetGuid);
            data["skyboxIntensity"] = value->skybox.intensity;
            data["skyboxRotationDegrees"] = value->skybox.rotationDegrees;
            data["skyboxVisibleToCamera"] = value->skybox.visibleToCamera;
            const AssetGuid hdriGuid = !value->requestedEnvironmentAssetGuid.isNil()
                ? value->requestedEnvironmentAssetGuid
                : value->hdri.environmentAssetGuid;
            data["hdriEnvironment"] = assetReference(hdriGuid);
            data["hdriLightingIntensity"] = value->hdri.lightingIntensity;
            data["hdriBackgroundIntensity"] = value->hdri.backgroundIntensity;
            data["hdriRotationDegrees"] = value->hdri.rotationDegrees;
            data["hdriVisibleToCamera"] = value->hdri.visibleToCamera;
            data["hdriAffectsLighting"] = value->hdri.affectsLighting;
            data["simulatedTurbidity"] = value->simulated.turbidity;
            data["simulatedOzone"] = value->simulated.ozone;
            data["simulatedGroundAlbedo"] = value->simulated.groundAlbedo;
            data["simulatedAtmosphereHeightKm"] =
                value->simulated.atmosphereHeightKilometers;
            data["simulatedSunDisk"] = value->simulated.sunDisk;
            data["simulatedAerialPerspective"] =
                value->simulated.aerialPerspective;
            data["priority"] = value->priority;
            return true;
        }

        bool deserializeSky(Registry& registry, Entity entity,
            const SourceJson& data, std::string& error) {
            auto& value = registry.addComponent<SkyComponent>(entity);
            value.enabled = data.value("enabled", true);
            value.mode = static_cast<SkyMode>(data.value("mode", int32_t{ 1 }));
            if (!readAssetReference(data, "skyboxAsset",
                    value.skybox.cubemapAssetGuid, error) ||
                !readAssetReference(data, "hdriEnvironment",
                    value.hdri.environmentAssetGuid, error)) return false;
            value.skybox.intensity = data.value("skyboxIntensity", 1.0f);
            value.skybox.rotationDegrees = data.value(
                "skyboxRotationDegrees", 0.0f);
            value.skybox.visibleToCamera = data.value(
                "skyboxVisibleToCamera", true);
            value.hdri.lightingIntensity = data.value(
                "hdriLightingIntensity", 1.0f);
            value.hdri.backgroundIntensity = data.value(
                "hdriBackgroundIntensity", 1.0f);
            value.hdri.rotationDegrees = data.value(
                "hdriRotationDegrees", 0.0f);
            value.hdri.visibleToCamera = data.value(
                "hdriVisibleToCamera", true);
            value.hdri.affectsLighting = data.value(
                "hdriAffectsLighting", true);
            value.simulated.turbidity = data.value("simulatedTurbidity", 2.0f);
            value.simulated.ozone = data.value("simulatedOzone", 0.35f);
            value.simulated.groundAlbedo = data.value(
                "simulatedGroundAlbedo", 0.30f);
            value.simulated.atmosphereHeightKilometers = data.value(
                "simulatedAtmosphereHeightKm", 100.0f);
            value.simulated.sunDisk = data.value("simulatedSunDisk", true);
            value.simulated.aerialPerspective = data.value(
                "simulatedAerialPerspective", true);
            value.priority = data.value("priority", int32_t{ 0 });
            value.requestedEnvironmentAssetGuid =
                value.hdri.environmentAssetGuid;
            return true;
        }

        bool validSky(const SkyComponent& value) {
            const int32_t mode = static_cast<int32_t>(value.mode);
            return mode >= static_cast<int32_t>(SkyMode::Skybox) &&
                mode <= static_cast<int32_t>(SkyMode::Simulated) &&
                std::isfinite(value.skybox.intensity) &&
                value.skybox.intensity >= 0.0f &&
                std::isfinite(value.skybox.rotationDegrees) &&
                std::isfinite(value.hdri.lightingIntensity) &&
                value.hdri.lightingIntensity >= 0.0f &&
                std::isfinite(value.hdri.backgroundIntensity) &&
                value.hdri.backgroundIntensity >= 0.0f &&
                std::isfinite(value.hdri.rotationDegrees) &&
                std::isfinite(value.simulated.turbidity) &&
                value.simulated.turbidity >= 1.0f &&
                std::isfinite(value.simulated.ozone) &&
                value.simulated.ozone >= 0.0f && value.simulated.ozone <= 1.0f &&
                std::isfinite(value.simulated.groundAlbedo) &&
                value.simulated.groundAlbedo >= 0.0f &&
                value.simulated.groundAlbedo <= 1.0f &&
                std::isfinite(value.simulated.atmosphereHeightKilometers) &&
                value.simulated.atmosphereHeightKilometers > 0.0f;
        }

        bool validateSkySource(const SourceJson& data, std::string& error) {
            if (!objectData(data, error)) return false;
            Registry scratch;
            const Entity entity = scratch.createEntity();
            if (!deserializeSky(scratch, entity, data, error)) return false;
            if (!validSky(scratch.getComponent<SkyComponent>(entity))) {
                error = "Sky settings are outside their finite physical domains";
                return false;
            }
            return true;
        }

        [[nodiscard]] AssetGuid reflectionProbeEnvironmentGuid(
            const ReflectionProbeComponent& value) {
            return !value.requestedEnvironmentAssetGuid.isNil()
                ? value.requestedEnvironmentAssetGuid
                : value.environmentAssetGuid;
        }

        bool serializeReflectionProbe(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<ReflectionProbeComponent>(
                registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["enabled"] = value->enabled;
            data["shape"] = static_cast<int32_t>(value->shape);
            data["sphereRadiusMeters"] = value->sphereRadiusMeters;
            data["boxExtentsMeters"] = { value->boxExtentsMeters.x,
                value->boxExtentsMeters.y, value->boxExtentsMeters.z };
            data["blendDistanceMeters"] = value->blendDistanceMeters;
            data["intensity"] = value->intensity;
            data["priority"] = value->priority;
            data["updateMode"] = static_cast<int32_t>(value->updateMode);
            data["parallaxMode"] = static_cast<int32_t>(value->parallaxMode);
            data["captureResolution"] = value->captureResolution;
            data["captureNearMeters"] = value->captureNearMeters;
            data["captureFarMeters"] = value->captureFarMeters;
            data["captureSky"] = value->captureSky;
            data["environment"] = assetReference(
                reflectionProbeEnvironmentGuid(*value));
            return true;
        }

        bool deserializeReflectionProbe(Registry& registry, Entity entity,
            const SourceJson& data, std::string& error) {
            auto& value = registry.addComponent<ReflectionProbeComponent>(entity);
            value.enabled = data.value("enabled", true);
            value.shape = static_cast<ReflectionProbeShape>(
                data.value("shape", int32_t{ 1 }));
            value.sphereRadiusMeters = data.value("sphereRadiusMeters", 5.0f);
            value.boxExtentsMeters = vectorValue(data, "boxExtentsMeters",
                { 5.0f, 5.0f, 5.0f });
            value.blendDistanceMeters = data.value("blendDistanceMeters", 1.0f);
            value.intensity = data.value("intensity", 1.0f);
            value.priority = data.value("priority", int32_t{ 0 });
            value.updateMode = static_cast<ReflectionProbeUpdateMode>(
                data.value("updateMode", int32_t{ 1 }));
            value.parallaxMode = static_cast<ReflectionProbeParallaxMode>(
                data.value("parallaxMode", int32_t{ 1 }));
            value.captureResolution = data.value(
                "captureResolution", int32_t{ 512 });
            value.captureNearMeters = data.value("captureNearMeters", 0.1f);
            value.captureFarMeters = data.value("captureFarMeters", 100.0f);
            value.captureSky = data.value("captureSky", true);
            if (!readAssetReference(data, "environment",
                    value.environmentAssetGuid, error)) return false;
            value.requestedEnvironmentAssetGuid = value.environmentAssetGuid;
            return true;
        }

        [[nodiscard]] bool validReflectionProbe(
            const ReflectionProbeComponent& value) {
            const int32_t shape = static_cast<int32_t>(value.shape);
            const int32_t updateMode = static_cast<int32_t>(value.updateMode);
            const int32_t parallaxMode = static_cast<int32_t>(value.parallaxMode);
            const bool resolutionSupported = value.captureResolution == 128 ||
                value.captureResolution == 256 ||
                value.captureResolution == 512 ||
                value.captureResolution == 1024 ||
                value.captureResolution == 2048 ||
                value.captureResolution == 4096;
            return shape >= static_cast<int32_t>(ReflectionProbeShape::Sphere) &&
                shape <= static_cast<int32_t>(ReflectionProbeShape::Box) &&
                updateMode >= static_cast<int32_t>(
                    ReflectionProbeUpdateMode::Baked) &&
                updateMode <= static_cast<int32_t>(
                    ReflectionProbeUpdateMode::Realtime) &&
                parallaxMode >= static_cast<int32_t>(
                    ReflectionProbeParallaxMode::None) &&
                parallaxMode <= static_cast<int32_t>(
                    ReflectionProbeParallaxMode::BoxProjection) &&
                std::isfinite(value.sphereRadiusMeters) &&
                value.sphereRadiusMeters > 0.0f &&
                finite(value.boxExtentsMeters) &&
                glm::all(glm::greaterThan(value.boxExtentsMeters,
                    glm::vec3(0.0f))) &&
                std::isfinite(value.blendDistanceMeters) &&
                value.blendDistanceMeters >= 0.0f &&
                std::isfinite(value.intensity) && value.intensity >= 0.0f &&
                resolutionSupported &&
                std::isfinite(value.captureNearMeters) &&
                value.captureNearMeters > 0.0f &&
                std::isfinite(value.captureFarMeters) &&
                value.captureFarMeters > value.captureNearMeters;
        }

        bool validateReflectionProbeSource(const SourceJson& data,
            std::string& error) {
            if (!objectData(data, error)) return false;
            Registry scratch;
            const Entity entity = scratch.createEntity();
            if (!deserializeReflectionProbe(scratch, entity, data, error))
                return false;
            if (!validReflectionProbe(
                    scratch.getComponent<ReflectionProbeComponent>(entity))) {
                error = "Reflection probe settings are outside their supported finite domains";
                return false;
            }
            return true;
        }

        [[nodiscard]] AssetGuid bakedLightingAssetGuid(
            const BakedLightingSetComponent& value) {
            return !value.requestedLightingAssetGuid.isNil()
                ? value.requestedLightingAssetGuid
                : value.lightingAssetGuid;
        }

        bool serializeBakedLightingSet(const Registry& registry, Entity entity,
            const SceneIdentityMap&, SourceJson& data, std::string&) {
            const auto* value = component<BakedLightingSetComponent>(
                registry, entity);
            if (!value) { data = nullptr; return true; }
            ensureObject(data);
            data["enabled"] = value->enabled;
            data["lightingAsset"] = assetReference(
                bakedLightingAssetGuid(*value));
            data["diffuseIntensity"] = value->diffuseIntensity;
            data["specularIntensity"] = value->specularIntensity;
            data["applyLightmaps"] = value->applyLightmaps;
            data["applyProbeVolumes"] = value->applyProbeVolumes;
            data["applyVisibility"] = value->applyVisibility;
            return true;
        }

        bool deserializeBakedLightingSet(Registry& registry, Entity entity,
            const SourceJson& data, std::string& error) {
            auto& value = registry.addComponent<BakedLightingSetComponent>(entity);
            value.enabled = data.value("enabled", true);
            if (!readAssetReference(data, "lightingAsset",
                    value.lightingAssetGuid, error)) return false;
            value.requestedLightingAssetGuid = value.lightingAssetGuid;
            value.diffuseIntensity = data.value("diffuseIntensity", 1.0f);
            value.specularIntensity = data.value("specularIntensity", 1.0f);
            value.applyLightmaps = data.value("applyLightmaps", true);
            value.applyProbeVolumes = data.value("applyProbeVolumes", true);
            value.applyVisibility = data.value("applyVisibility", true);
            return true;
        }

        [[nodiscard]] bool validBakedLightingSet(
            const BakedLightingSetComponent& value) {
            return std::isfinite(value.diffuseIntensity) &&
                value.diffuseIntensity >= 0.0f &&
                std::isfinite(value.specularIntensity) &&
                value.specularIntensity >= 0.0f;
        }

        bool validateBakedLightingSetSource(const SourceJson& data,
            std::string& error) {
            if (!objectData(data, error)) return false;
            Registry scratch;
            const Entity entity = scratch.createEntity();
            if (!deserializeBakedLightingSet(scratch, entity, data, error))
                return false;
            if (!validBakedLightingSet(
                    scratch.getComponent<BakedLightingSetComponent>(entity))) {
                error = "Baked-lighting intensities must be finite and nonnegative";
                return false;
            }
            return true;
        }

        bool migrateLightV1(const SourceJson& input, SourceJson& output,
            std::vector<SourceMigrationNotice>& notices, std::string& error) {
            if (!input.is_object()) {
                error = "Light v1 data must be an object";
                return false;
            }
            const int32_t type = input.value("type", int32_t{ 0 });
            if (type < static_cast<int32_t>(LightType::Directional) ||
                type > static_cast<int32_t>(LightType::Area)) {
                error = "Light v1 type is outside the supported enum domain";
                return false;
            }
            const auto intensity = input.find("intensity");
            if (intensity != input.end() && !intensity->is_number()) {
                error = "Light v1 intensity must be numeric";
                return false;
            }
            const float adoptedIntensity = input.value("intensity", 1.0f);
            if (!std::isfinite(adoptedIntensity) || adoptedIntensity < 0.0f) {
                error = "Light v1 intensity must be finite and nonnegative";
                return false;
            }

            output = input;
            output.erase("color");
            output.erase("intensity");
            output.erase("range");
            output.erase("radius");
            output.erase("innerCone");
            output.erase("outerCone");
            output["colorLinearRec709"] = input.value("color",
                SourceJson::array({ 1.0f, 1.0f, 1.0f }));
            output["illuminanceLux"] = type ==
                static_cast<int32_t>(LightType::Directional)
                ? adoptedIntensity : 1.0f;
            output["luminousIntensityCandela"] = type ==
                static_cast<int32_t>(LightType::Directional)
                ? 1.0f : adoptedIntensity;
            output["rangeMeters"] = input.value("range", 10.0f);
            output["sourceRadiusMeters"] = input.value("radius", 0.5f);
            output["innerConeDegrees"] = input.value("innerCone", 12.5f);
            output["outerConeDegrees"] = input.value("outerCone", 45.0f);
            output["shadowQuality"] = static_cast<int32_t>(
                LightShadowQuality::High);
            output["priority"] = int32_t{ 0 };
            notices.push_back({ "light.v1_color_assumed_linear_rec709",
                "/colorLinearRec709",
                "Light v1 RGB was preserved numerically and adopted as linear Rec.709/D65" });
            notices.push_back({ "light.v1_intensity_unit_adopted",
                type == static_cast<int32_t>(LightType::Directional)
                    ? "/illuminanceLux" : "/luminousIntensityCandela",
                type == static_cast<int32_t>(LightType::Directional)
                    ? "Light v1 intensity was preserved numerically and adopted as lux"
                    : "Light v1 intensity was preserved numerically and adopted as candela" });
            if (type == static_cast<int32_t>(LightType::Area)) {
                notices.push_back({ "light.area_unsupported", "/type",
                    "Legacy Area light remains readable but strict cooking rejects it" });
            }
            return true;
        }

        bool validateLightSource(const SourceJson& data, std::string& error) {
            if (!objectData(data, error)) return false;
            const int32_t type = data.value("type", int32_t{ 0 });
            if (type < static_cast<int32_t>(LightType::Directional) ||
                type > static_cast<int32_t>(LightType::Area)) {
                error = "Light type is outside the supported enum domain";
                return false;
            }
            const int32_t quality = data.value("shadowQuality", int32_t{ 2 });
            if (quality < static_cast<int32_t>(LightShadowQuality::Low) ||
                quality > static_cast<int32_t>(LightShadowQuality::Ultra)) {
                error = "Light shadow quality is outside the supported enum domain";
                return false;
            }
            const glm::vec3 color = vectorValue(data, "colorLinearRec709",
                { 1.0f, 1.0f, 1.0f });
            const float lux = data.value("illuminanceLux", 1.0f);
            const float candela = data.value("luminousIntensityCandela", 1.0f);
            const float range = data.value("rangeMeters", 10.0f);
            const float radius = data.value("sourceRadiusMeters", 0.5f);
            if (!finite(color) || glm::any(glm::lessThan(color, glm::vec3(0.0f))) ||
                !std::isfinite(lux) || lux < 0.0f ||
                !std::isfinite(candela) || candela < 0.0f ||
                !std::isfinite(range) || range < 0.0f ||
                !std::isfinite(radius) || radius < 0.0f) {
                error = "Light physical color, intensities, range, and radius must be finite and nonnegative";
                return false;
            }
            const float inner = data.value("innerConeDegrees", 12.5f);
            const float outer = data.value("outerConeDegrees", 45.0f);
            if (inner < 0.0f || outer < inner || outer > 90.0f) {
                error = "Light cone angles must satisfy 0 <= inner <= outer <= 90";
                return false;
            }
            return true;
        }

        bool resolveNone(Registry&, Entity, const SceneIdentityMap&,
            SceneReferenceState&) { return true; }

        bool resolveRelationship(Registry& registry, Entity entity,
            const SceneIdentityMap& identities, SceneReferenceState& references) {
            auto* relationships = registry.findPool<RelationshipComponent>();
            if (!relationships || !relationships->has(entity)) return false;
            auto& value = relationships->get(entity);
            const auto owner = identities.persistentId(entity);
            const auto id = ComponentTypeId::parse(
                CoreRelationshipComponentId);
            if (!owner || !id) return false;
            const SceneReferenceRecord* reference = references.find({
                *owner, *id, "parent" });
            if (!reference) return true;
            if (reference->resolution != StableReferenceResolution::Resolved) {
                return !reference->required;
            }
            const auto parent = identities.resolve(SceneEntityUuid(reference->target));
            if (!parent || *parent == entity || !relationships->has(*parent)) {
                return false;
            }
            value.parent = *parent;
            auto& parentRelationship = relationships->get(*parent);
            parentRelationship.children.push_back(entity);
            value.depth = parentRelationship.depth + 1;
            return true;
        }

        bool validateNameRuntime(const Registry& registry, Entity entity) {
            return component<NameComponent>(registry, entity) != nullptr;
        }
        bool validateTransformRuntime(const Registry& registry, Entity entity) {
            const auto* value = component<TransformComponent>(registry, entity);
            return value && finite(value->position) && finite(value->rotation) &&
                finite(value->scale);
        }
        bool validateRelationshipRuntime(const Registry& registry, Entity entity) {
            const auto* value = component<RelationshipComponent>(registry, entity);
            if (!value || value->depth < 0 || value->siblingOrder < 0) return false;
            return value->parent == NULL_ENTITY
                ? value->depth == 0
                : registry.isAlive(value->parent) && value->parent != entity;
        }
        bool validateMeshRuntime(const Registry& registry, Entity entity) {
            const auto* value = component<MeshComponent>(registry, entity);
            if (!value) return false;
            std::set<AssetGuid> sources;
            for (const auto& entry : value->materialOverrides) {
                if (entry.sourceMaterialGuid.isNil() || entry.materialGuid.isNil() ||
                    !sources.insert(entry.sourceMaterialGuid).second) return false;
            }
            return true;
        }
        bool validateLightRuntime(const Registry& registry, Entity entity) {
            const auto* value = component<LightComponent>(registry, entity);
            const int32_t quality = value
                ? static_cast<int32_t>(value->shadowQuality) : -1;
            return value && finite(value->colorLinearRec709) &&
                !glm::any(glm::lessThan(value->colorLinearRec709, glm::vec3(0.0f))) &&
                std::isfinite(value->illuminanceLux) && value->illuminanceLux >= 0.0f &&
                std::isfinite(value->luminousIntensityCandela) && value->luminousIntensityCandela >= 0.0f &&
                std::isfinite(value->rangeMeters) && value->rangeMeters >= 0.0f &&
                std::isfinite(value->sourceRadiusMeters) && value->sourceRadiusMeters >= 0.0f &&
                std::isfinite(value->innerConeDegrees) &&
                std::isfinite(value->outerConeDegrees) &&
                value->innerConeDegrees >= 0.0f &&
                value->outerConeDegrees >= value->innerConeDegrees &&
                value->outerConeDegrees <= 90.0f && quality >= 0 && quality <= 3;
        }
        bool validateSkyRuntime(const Registry& registry, Entity entity) {
            const auto* value = component<SkyComponent>(registry, entity);
            return value && validSky(*value);
        }
        bool validateReflectionProbeRuntime(const Registry& registry,
            Entity entity) {
            const auto* value = component<ReflectionProbeComponent>(
                registry, entity);
            return value && validReflectionProbe(*value);
        }
        bool validateBakedLightingSetRuntime(const Registry& registry,
            Entity entity) {
            const auto* value = component<BakedLightingSetComponent>(
                registry, entity);
            return value && validBakedLightingSet(*value);
        }

        bool encodeName(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<NameComponent>(registry, entity);
            return value && writer.writeString(value->name);
        }

        bool decodeName(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            std::string name;
            if (!reader.readString(name) || !reader.finish()) return false;
            registry.addComponent<NameComponent>(entity, std::move(name));
            return true;
        }

        bool encodeTransform(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<TransformComponent>(registry, entity);
            if (!value) return false;
            for (float field : { value->position.x, value->position.y,
                    value->position.z, value->rotation.x, value->rotation.y,
                    value->rotation.z, value->scale.x, value->scale.y,
                    value->scale.z }) {
                if (!writer.writeFloat32(field)) return false;
            }
            return true;
        }

        bool decodeTransform(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            std::array<float, 9> fields{};
            for (float& field : fields) {
                if (!reader.readFloat32(field)) return false;
            }
            if (!reader.finish()) return false;
            auto& value = registry.addComponent<TransformComponent>(entity);
            value.position = { fields[0], fields[1], fields[2] };
            value.rotation = { fields[3], fields[4], fields[5] };
            value.scale = { fields[6], fields[7], fields[8] };
            value.isDirty = true;
            return true;
        }

        bool encodeRelationship(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<RelationshipComponent>(registry, entity);
            return value && writer.writeEntityReference(value->parent) &&
                writer.writeInt32(value->siblingOrder);
        }

        bool decodeRelationship(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            std::optional<SceneEntityUuid> parent;
            int32_t siblingOrder = 0;
            if (!reader.readEntityReference("parent", false, parent) ||
                !reader.readInt32(siblingOrder) || siblingOrder < 0 ||
                !reader.finish()) return false;
            auto& value = registry.addComponent<RelationshipComponent>(entity);
            value.parent = NULL_ENTITY;
            value.children.clear();
            value.depth = 0;
            value.siblingOrder = siblingOrder;
            return true;
        }

        [[nodiscard]] AssetGuid meshModelGuid(const MeshComponent& value) {
            return !value.requestedAssetGuid.isNil()
                ? value.requestedAssetGuid
                : !value.assetGuid.isNil()
                    ? value.assetGuid
                    : value.model ? value.model->assetGuid : AssetGuid{};
        }

        bool encodeMesh(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<MeshComponent>(registry, entity);
            if (!value || !writer.writeBoolean(value->enabled) ||
                !writer.writeAssetReference(meshModelGuid(*value))) return false;
            std::vector<MeshComponent::MaterialOverride> overrides =
                value->materialOverrides;
            std::ranges::sort(overrides,
                [](const auto& lhs, const auto& rhs) {
                    return lhs.sourceMaterialGuid < rhs.sourceMaterialGuid;
                });
            if (overrides.size() > (std::numeric_limits<uint32_t>::max)() ||
                !writer.writeUInt32(static_cast<uint32_t>(overrides.size()))) {
                return false;
            }
            AssetGuid previous;
            for (const auto& entry : overrides) {
                if (entry.sourceMaterialGuid.isNil() || entry.materialGuid.isNil() ||
                    entry.sourceMaterialGuid == previous ||
                    !writer.writeAssetReference(entry.sourceMaterialGuid) ||
                    !writer.writeAssetReference(entry.materialGuid)) return false;
                previous = entry.sourceMaterialGuid;
            }
            return true;
        }

        bool decodeMesh(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            bool enabled = true;
            AssetGuid model;
            uint32_t count = 0;
            if (!reader.readBoolean(enabled) ||
                !reader.readAssetReference("model", false,
                    StableReferenceKind::Asset, model) ||
                !reader.readUInt32(count) || count > reader.remaining() / 8) {
                return false;
            }
            std::vector<MeshComponent::MaterialOverride> overrides;
            overrides.reserve(count);
            AssetGuid previous;
            for (uint32_t index = 0; index < count; ++index) {
                AssetGuid source;
                AssetGuid replacement;
                const std::string base = "material_overrides/" +
                    std::to_string(index);
                if (!reader.readAssetReference(base + "/source", true,
                        StableReferenceKind::Subasset, source) ||
                    !reader.readAssetReference(base + "/replacement", true,
                        StableReferenceKind::Subasset, replacement) ||
                    source.isNil() || replacement.isNil() || source == previous) {
                    return false;
                }
                previous = source;
                overrides.push_back({ source, replacement });
            }
            if (!reader.finish()) return false;
            auto& value = registry.addComponent<MeshComponent>(entity);
            value.enabled = enabled;
            value.model.reset();
            value.assetGuid = model;
            value.requestedAssetGuid = model;
            value.materialOverrides = std::move(overrides);
            return true;
        }

        bool encodeLight(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<LightComponent>(registry, entity);
            return value && value->type != LightType::Area &&
                writer.writeInt32(static_cast<int32_t>(value->type)) &&
                writer.writeFloat32(value->colorLinearRec709.x) &&
                writer.writeFloat32(value->colorLinearRec709.y) &&
                writer.writeFloat32(value->colorLinearRec709.z) &&
                writer.writeFloat32(value->illuminanceLux) &&
                writer.writeFloat32(value->luminousIntensityCandela) &&
                writer.writeFloat32(value->rangeMeters) &&
                writer.writeFloat32(value->sourceRadiusMeters) &&
                writer.writeFloat32(value->innerConeDegrees) &&
                writer.writeFloat32(value->outerConeDegrees) &&
                writer.writeBoolean(value->castsShadows) &&
                writer.writeInt32(static_cast<int32_t>(value->shadowQuality)) &&
                writer.writeInt32(value->priority);
        }

        bool decodeLight(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            int32_t type = 0;
            std::array<float, 9> fields{};
            bool castsShadows = true;
            int32_t quality = 0;
            int32_t priority = 0;
            if (!reader.readInt32(type) ||
                type < static_cast<int32_t>(LightType::Directional) ||
                type > static_cast<int32_t>(LightType::Spot)) return false;
            for (float& field : fields) {
                if (!reader.readFloat32(field)) return false;
            }
            if (!reader.readBoolean(castsShadows) || !reader.readInt32(quality) ||
                quality < static_cast<int32_t>(LightShadowQuality::Low) ||
                quality > static_cast<int32_t>(LightShadowQuality::Ultra) ||
                !reader.readInt32(priority) || !reader.finish()) return false;
            auto& value = registry.addComponent<LightComponent>(entity);
            value.type = static_cast<LightType>(type);
            value.colorLinearRec709 = { fields[0], fields[1], fields[2] };
            value.illuminanceLux = fields[3];
            value.luminousIntensityCandela = fields[4];
            value.rangeMeters = fields[5];
            value.sourceRadiusMeters = fields[6];
            value.innerConeDegrees = fields[7];
            value.outerConeDegrees = fields[8];
            value.castsShadows = castsShadows;
            value.shadowQuality = static_cast<LightShadowQuality>(quality);
            value.priority = priority;
            return true;
        }

        bool encodeSky(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<SkyComponent>(registry, entity);
            return value &&
                writer.writeBoolean(value->enabled) &&
                writer.writeInt32(static_cast<int32_t>(value->mode)) &&
                writer.writeAssetReference(value->skybox.cubemapAssetGuid) &&
                writer.writeFloat32(value->skybox.intensity) &&
                writer.writeFloat32(value->skybox.rotationDegrees) &&
                writer.writeBoolean(value->skybox.visibleToCamera) &&
                writer.writeAssetReference(value->hdri.environmentAssetGuid) &&
                writer.writeFloat32(value->hdri.lightingIntensity) &&
                writer.writeFloat32(value->hdri.backgroundIntensity) &&
                writer.writeFloat32(value->hdri.rotationDegrees) &&
                writer.writeBoolean(value->hdri.visibleToCamera) &&
                writer.writeBoolean(value->hdri.affectsLighting) &&
                writer.writeFloat32(value->simulated.turbidity) &&
                writer.writeFloat32(value->simulated.ozone) &&
                writer.writeFloat32(value->simulated.groundAlbedo) &&
                writer.writeFloat32(value->simulated.atmosphereHeightKilometers) &&
                writer.writeBoolean(value->simulated.sunDisk) &&
                writer.writeBoolean(value->simulated.aerialPerspective) &&
                writer.writeInt32(value->priority);
        }

        bool decodeSky(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            auto& value = registry.addComponent<SkyComponent>(entity);
            int32_t mode = 0;
            if (!reader.readBoolean(value.enabled) ||
                !reader.readInt32(mode) ||
                mode < static_cast<int32_t>(SkyMode::Skybox) ||
                mode > static_cast<int32_t>(SkyMode::Simulated) ||
                !reader.readAssetReference("skybox_asset", false,
                    StableReferenceKind::Asset,
                    value.skybox.cubemapAssetGuid) ||
                !reader.readFloat32(value.skybox.intensity) ||
                !reader.readFloat32(value.skybox.rotationDegrees) ||
                !reader.readBoolean(value.skybox.visibleToCamera) ||
                !reader.readAssetReference("hdri_environment", false,
                    StableReferenceKind::Asset,
                    value.hdri.environmentAssetGuid) ||
                !reader.readFloat32(value.hdri.lightingIntensity) ||
                !reader.readFloat32(value.hdri.backgroundIntensity) ||
                !reader.readFloat32(value.hdri.rotationDegrees) ||
                !reader.readBoolean(value.hdri.visibleToCamera) ||
                !reader.readBoolean(value.hdri.affectsLighting) ||
                !reader.readFloat32(value.simulated.turbidity) ||
                !reader.readFloat32(value.simulated.ozone) ||
                !reader.readFloat32(value.simulated.groundAlbedo) ||
                !reader.readFloat32(
                    value.simulated.atmosphereHeightKilometers) ||
                !reader.readBoolean(value.simulated.sunDisk) ||
                !reader.readBoolean(value.simulated.aerialPerspective) ||
                !reader.readInt32(value.priority) || !reader.finish()) return false;
            value.mode = static_cast<SkyMode>(mode);
            value.requestedEnvironmentAssetGuid =
                value.hdri.environmentAssetGuid;
            return validSky(value);
        }

        bool encodeReflectionProbe(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<ReflectionProbeComponent>(
                registry, entity);
            return value && validReflectionProbe(*value) &&
                writer.writeBoolean(value->enabled) &&
                writer.writeInt32(static_cast<int32_t>(value->shape)) &&
                writer.writeFloat32(value->sphereRadiusMeters) &&
                writer.writeFloat32(value->boxExtentsMeters.x) &&
                writer.writeFloat32(value->boxExtentsMeters.y) &&
                writer.writeFloat32(value->boxExtentsMeters.z) &&
                writer.writeFloat32(value->blendDistanceMeters) &&
                writer.writeFloat32(value->intensity) &&
                writer.writeInt32(value->priority) &&
                writer.writeInt32(static_cast<int32_t>(value->updateMode)) &&
                writer.writeInt32(static_cast<int32_t>(value->parallaxMode)) &&
                writer.writeInt32(value->captureResolution) &&
                writer.writeFloat32(value->captureNearMeters) &&
                writer.writeFloat32(value->captureFarMeters) &&
                writer.writeBoolean(value->captureSky) &&
                writer.writeAssetReference(reflectionProbeEnvironmentGuid(*value));
        }

        bool decodeReflectionProbe(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            auto& value = registry.addComponent<ReflectionProbeComponent>(entity);
            int32_t shape = 0;
            int32_t updateMode = 0;
            int32_t parallaxMode = 0;
            if (!reader.readBoolean(value.enabled) ||
                !reader.readInt32(shape) ||
                !reader.readFloat32(value.sphereRadiusMeters) ||
                !reader.readFloat32(value.boxExtentsMeters.x) ||
                !reader.readFloat32(value.boxExtentsMeters.y) ||
                !reader.readFloat32(value.boxExtentsMeters.z) ||
                !reader.readFloat32(value.blendDistanceMeters) ||
                !reader.readFloat32(value.intensity) ||
                !reader.readInt32(value.priority) ||
                !reader.readInt32(updateMode) ||
                !reader.readInt32(parallaxMode) ||
                !reader.readInt32(value.captureResolution) ||
                !reader.readFloat32(value.captureNearMeters) ||
                !reader.readFloat32(value.captureFarMeters) ||
                !reader.readBoolean(value.captureSky) ||
                !reader.readAssetReference("environment", false,
                    StableReferenceKind::Asset,
                    value.environmentAssetGuid) ||
                !reader.finish()) return false;
            value.shape = static_cast<ReflectionProbeShape>(shape);
            value.updateMode = static_cast<ReflectionProbeUpdateMode>(updateMode);
            value.parallaxMode = static_cast<ReflectionProbeParallaxMode>(
                parallaxMode);
            value.requestedEnvironmentAssetGuid = value.environmentAssetGuid;
            return validReflectionProbe(value);
        }

        bool encodeBakedLightingSet(const Registry& registry, Entity entity,
            CookedComponentWriter& writer) {
            const auto* value = component<BakedLightingSetComponent>(
                registry, entity);
            return value && validBakedLightingSet(*value) &&
                writer.writeBoolean(value->enabled) &&
                writer.writeAssetReference(bakedLightingAssetGuid(*value)) &&
                writer.writeFloat32(value->diffuseIntensity) &&
                writer.writeFloat32(value->specularIntensity) &&
                writer.writeBoolean(value->applyLightmaps) &&
                writer.writeBoolean(value->applyProbeVolumes) &&
                writer.writeBoolean(value->applyVisibility);
        }

        bool decodeBakedLightingSet(Registry& registry, Entity entity,
            CookedComponentReader& reader) {
            auto& value = registry.addComponent<BakedLightingSetComponent>(entity);
            if (!reader.readBoolean(value.enabled) ||
                !reader.readAssetReference("lighting_asset", false,
                    StableReferenceKind::Asset, value.lightingAssetGuid) ||
                !reader.readFloat32(value.diffuseIntensity) ||
                !reader.readFloat32(value.specularIntensity) ||
                !reader.readBoolean(value.applyLightmaps) ||
                !reader.readBoolean(value.applyProbeVolumes) ||
                !reader.readBoolean(value.applyVisibility) ||
                !reader.finish()) return false;
            value.requestedLightingAssetGuid = value.lightingAssetGuid;
            return validBakedLightingSet(value);
        }
        [[nodiscard]] RuntimeComponentCallbacks runtimeCallbacks(
            ResolveComponentReferencesFn resolve,
            ValidateRuntimeComponentFn validate,
            EncodeCookedComponentFn encode,
            DecodeCookedComponentFn decode) {
            return { resolve, validate, encode, decode };
        }

    } // namespace

    CoreSceneRegistryBundle createCoreSceneRegistryBundle() {
        const CoreRuntimeComponentCallbacks runtimeCallbacksValue{
            .name = runtimeCallbacks(resolveNone, validateNameRuntime,
                encodeName, decodeName),
            .transform = runtimeCallbacks(resolveNone, validateTransformRuntime,
                encodeTransform, decodeTransform),
            .relationship = runtimeCallbacks(resolveRelationship,
                validateRelationshipRuntime, encodeRelationship,
                decodeRelationship),
            .mesh = runtimeCallbacks(resolveNone, validateMeshRuntime,
                encodeMesh, decodeMesh),
            .light = runtimeCallbacks(resolveNone, validateLightRuntime,
                encodeLight, decodeLight),
            .sky = runtimeCallbacks(resolveNone, validateSkyRuntime,
                encodeSky, decodeSky),
            .reflectionProbe = runtimeCallbacks(resolveNone,
                validateReflectionProbeRuntime, encodeReflectionProbe,
                decodeReflectionProbe),
            .bakedLightingSet = runtimeCallbacks(resolveNone,
                validateBakedLightingSetRuntime, encodeBakedLightingSet,
                decodeBakedLightingSet),
        };
        CoreRuntimeRegistryResult runtime =
            createRuntimeSceneComponentRegistry(runtimeCallbacksValue);
        if (!runtime) {
            return { std::move(runtime.registry), {}, runtime.status.message };
        }
        CoreSourceComponentCallbacks sourceCallbacks{
            .name = { serializeName, deserializeName, objectData },
            .transform = { serializeTransform, deserializeTransform, objectData },
            .relationship = { serializeRelationship, deserializeRelationship,
                objectData },
            .mesh = { serializeMesh, deserializeMesh, validateMeshSource },
            .light = { serializeLight, deserializeLight, validateLightSource },
            .sky = { serializeSky, deserializeSky, validateSkySource },
            .reflectionProbe = { serializeReflectionProbe,
                deserializeReflectionProbe, validateReflectionProbeSource },
            .bakedLightingSet = { serializeBakedLightingSet,
                deserializeBakedLightingSet, validateBakedLightingSetSource },
        };
        sourceCallbacks.light.currentSourceVersion = 2;
        sourceCallbacks.light.migrations = { { 1, 2, migrateLightV1 } };
        CoreSourceRegistryResult source =
            createSourceComponentSerializerRegistry(runtime.registry,
                sourceCallbacks);
        if (!source) {
            return { std::move(runtime.registry), std::move(source.registry),
                source.status.message };
        }
        return { std::move(runtime.registry), std::move(source.registry), {} };
    }

} // namespace Iridium
