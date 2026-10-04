#pragma once

// M7R R5a.3 (design section 3.1): render extraction. It turns the scene, the
// editor view state and the asset bindings into one RenderFrame per frame:
// GPU-scene publication, lights and reflection probes, the main-view
// classification and draw-packet extraction, sorts, and the shadow and probe
// capture schedules. No ImGui, editor or GLFW dependency.

#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ecs/Entity.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/scene/GpuScenePublisher.h"
#include "scene/SceneWorld.h"

namespace Iridium {

    class AssetManager;
    class CpuProfiler;
    class IRenderBackend;
    struct ModelAsset;

    class RenderExtractor final {
    public:
        RenderExtractor(CpuProfiler& profiler, SceneWorld& scene);
        ~RenderExtractor();

        RenderExtractor(const RenderExtractor&) = delete;
        RenderExtractor& operator=(const RenderExtractor&) = delete;

        // After the backend exists: the GPU-scene publisher and its capacity.
        void attachBackend(IRenderBackend& backend);
        // After the asset manager exists (material override bindings).
        void attachAssets(AssetManager& assets);

        // --- GPU-scene publication (before beginFrame) ---
        // Observes the scene's mesh instances, synchronizes the persistent
        // publication and grows backend capacity; the tables are published
        // right after beginFrame.
        void prepareGpuScenePublication(Entity selectedEntity);
        [[nodiscard]] const GpuScenePackedTables* gpuSceneFrame() const noexcept {
            return gpuSceneFrame_;
        }
        [[nodiscard]] const GpuScenePublisher* gpuScenePublisher() const noexcept {
            return gpuScenePublisher_.get();
        }

    private:
        CpuProfiler& cpuProfiler_;
        SceneWorld& sceneWorld_;
        Registry& registry;
        IRenderBackend* renderBackend = nullptr;
        AssetManager* assetManager_ = nullptr;

        std::unique_ptr<GpuScenePublisher> gpuScenePublisher_;
        std::vector<GpuSceneObservedInstance> gpuSceneObservations_;
        struct GpuSceneObservationMetadata {
            SceneEntityUuid owner;
            const ModelAsset* model = nullptr;
            GeometryHandle geometry;
            std::string cookKey;
            uint64_t materialOverrideSignature = 0;
            uint64_t observationRevision = 0;
            glm::vec3 localMinimum{ 0.0f };
            glm::vec3 localMaximum{ 0.0f };
            uint32_t baseConsumerMask = 0;
            bool valid = false;
        };
        std::vector<GpuSceneObservationMetadata>
            gpuSceneObservationMetadata_;
        const GpuScenePackedTables* gpuSceneFrame_ = nullptr;
        uint32_t gpuSceneDirectFallbackCount_ = 0;
    };

} // namespace Iridium
