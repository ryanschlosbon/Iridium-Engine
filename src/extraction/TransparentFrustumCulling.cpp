// M7.10.1: conservative main-view frustum culling of transparent work (see
// TransparentFrustumCulling.h).
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "extraction/TransparentFrustumCulling.h"

#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/RenderFrame.h"
#include "renderer/rhi/TransparencyQualityOverride.h"
#include "scene/components/RenderInstanceBatchComponent.h"

#include <cmath>
#include <limits>

namespace Iridium {

    namespace {
        [[nodiscard]] bool finite3(const glm::vec3& value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] TransparentLocalBounds emptyBounds() noexcept {
            return { glm::vec3(std::numeric_limits<float>::max()),
                glm::vec3(std::numeric_limits<float>::lowest()), true };
        }

        void accumulate(TransparentLocalBounds& bounds,
            const glm::vec3& minimum, const glm::vec3& maximum) noexcept {
            if (!bounds.valid) return;
            if (!finite3(minimum) || !finite3(maximum) ||
                glm::any(glm::greaterThan(minimum, maximum))) {
                bounds.valid = false;
                return;
            }
            bounds.minimum = glm::min(bounds.minimum, minimum);
            bounds.maximum = glm::max(bounds.maximum, maximum);
        }
    }

    uint32_t TransparentCullRecord::transparentPacketDemand(
        const DrawPacket& packet) noexcept {
        return transparentResidencyDemand(packet.transparencyExecutionMode,
            packet.transparency);
    }

    TransparentLocalBounds transparentSubmeshBounds(
        const ModelAsset& model, std::span<const uint32_t> submeshes) noexcept {
        TransparentLocalBounds bounds = emptyBounds();
        bounds.valid = !submeshes.empty();
        for (const uint32_t subMeshIndex : submeshes) {
            const SubMesh& subMesh = model.subMeshes[subMeshIndex];
            accumulate(bounds, subMesh.boundsMin, subMesh.boundsMax);
        }
        return bounds;
    }

    TransparentLocalBounds instanceBatchTransparentBounds(
        const RenderInstanceBatchComponent& batch, const ModelAsset& model,
        std::span<const uint32_t> submeshes) noexcept {
        TransparentLocalBounds bounds = emptyBounds();
        bounds.valid = !submeshes.empty();
        for (const uint32_t subMeshIndex : submeshes) {
            if (subMeshIndex >= batch.subMeshBounds.size() ||
                !batch.subMeshBounds[subMeshIndex].valid ||
                model.transparencyExecutionMode !=
                    TransparencyExecutionMode::Classified ||
                model.subMeshes[subMeshIndex].transparency.resolvedClass !=
                    TransparencyClass::WeightedOit) {
                bounds.valid = false;
                return bounds;
            }
            accumulate(bounds, batch.subMeshBounds[subMeshIndex].minimum,
                batch.subMeshBounds[subMeshIndex].maximum);
        }
        return bounds;
    }

    bool transparentModelFrustumRejected(
        const GpuSceneFrustum& frustum, const ModelAsset& model,
        const RenderInstanceBatchComponent* batch,
        std::span<const uint32_t> submeshes,
        const TransparentLocalBounds& listBounds,
        const glm::mat4& world) noexcept {
        if (!frustum.valid || submeshes.empty()) return false;
        return transparentLocalBoundsFrustumRejected(frustum, batch
            ? instanceBatchTransparentBounds(*batch, model, submeshes)
            : listBounds, world);
    }

    bool transparentLocalBoundsFrustumRejected(
        const GpuSceneFrustum& frustum, const TransparentLocalBounds& local,
        const glm::mat4& world) noexcept {
        const glm::vec3& localMin = local.minimum;
        const glm::vec3& localMax = local.maximum;
        if (!frustum.valid || !local.valid || !finite3(localMin) ||
            !finite3(localMax) ||
            glm::any(glm::greaterThan(localMin, localMax))) {
            return false;
        }
        glm::vec3 worldMin(std::numeric_limits<float>::max());
        glm::vec3 worldMax(std::numeric_limits<float>::lowest());
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{
                (corner & 1u) != 0 ? localMax.x : localMin.x,
                (corner & 2u) != 0 ? localMax.y : localMin.y,
                (corner & 4u) != 0 ? localMax.z : localMin.z,
            };
            const glm::vec4 transformed = world * glm::vec4(local, 1.0f);
            if (!std::isfinite(transformed.w) ||
                std::abs(transformed.w) <= 1.0e-8f) {
                return false;
            }
            const glm::vec3 point = glm::vec3(transformed) / transformed.w;
            worldMin = glm::min(worldMin, point);
            worldMax = glm::max(worldMax, point);
        }
        return gpuSceneFrustumRejectsAabb(frustum, worldMin, worldMax);
    }

    uint32_t replayCulledTransparentWork(
        const TransparentCullRecord& record, PreviousTransformCache& cache,
        bool deterministicContent, unsigned layeredInterfaceOverride) {
        for (const TransparentCullRecord::Packet& packet : record.packets)
            cache.touch(packet.key, packet.world);
        uint32_t demand = record.packetDemand;
        for (const TransparentCullRecord::Model& rejected : record.models) {
            const ModelAsset& model = *rejected.model;
            for (const uint32_t subMeshIndex : rejected.submeshes) {
                const SubMesh& subMesh = model.subMeshes[subMeshIndex];
                cache.touch({ rejected.owner, subMesh.primitiveGuid },
                    rejected.world);
                demand |= transparentResidencyDemand(
                    model.transparencyExecutionMode, deterministicContent
                        ? subMesh.transparency
                        : withLayeredInterfaceBudget(subMesh.transparency,
                            layeredInterfaceOverride));
            }
        }
        return demand;
    }

} // namespace Iridium
