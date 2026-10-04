#include "VulkanShadowCasters.h"

#include "VulkanFrameTelemetry.h"
#include "VulkanPipelineLibrary.h"
#include "VulkanResourceRegistry.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/rhi/GpuSceneLod.h"
#include "renderer/rhi/Mesh.h"

namespace Iridium {

    VulkanIndirectAssetResolver vulkanIndirectAssets(
        const VulkanFeatureContext& context) noexcept {
        return {
            .owner = &context,
            .geometry = [](const void* owner, GeometryHandle handle,
                VulkanIndirectGeometry& geometry) {
                const VulkanGeometryPayload* payload =
                    static_cast<const VulkanFeatureContext*>(owner)->
                        resources.geometries().get(handle);
                if (payload == nullptr) return false;
                geometry = { payload->vertexBuffer.buffer,
                    payload->indexBuffer.buffer, payload->indexFormat,
                    payload->vertexOffset };
                return true;
            },
            .material = [](const void* owner, MaterialHandle handle,
                VulkanIndirectMaterial& material) {
                const VulkanMaterialPayload* payload =
                    static_cast<const VulkanFeatureContext*>(owner)->
                        resources.materials().get(handle);
                if (payload == nullptr) return false;
                material = { payload->packed.alphaMode,
                    payload->packed.doubleSided };
                return true;
            },
            .gbufferIndirectPipeline = [](const void* owner,
                PipelineHandle handle) {
                const VulkanPipelineRecord* record =
                    static_cast<const VulkanFeatureContext*>(owner)->
                        pipelines.get(handle);
                return record != nullptr &&
                    record->gpuSceneIndirectPipeline != VK_NULL_HANDLE &&
                    record->pipelineLayout != VK_NULL_HANDLE &&
                    record->renderPass == RenderPassClass::GBuffer;
            },
        };
    }

    void recordIndirectOracleDraws(const VulkanFeatureContext& context,
        const VulkanCasterScratch& scratch, uint8_t visibilityBit,
        uint64_t& drawCounter, uint64_t& commandCounter,
        uint64_t& alphaMaskCounter) {
        VulkanFrameTelemetry& telemetry = context.telemetry;
        for (size_t casterIndex = 0; casterIndex < scratch.casters.size();
                ++casterIndex) {
            const VulkanResolvedCaster& caster = scratch.casters[casterIndex];
            if (caster.gpuScenePrimitiveIndex == InvalidGpuSceneIndex ||
                (scratch.visibility[casterIndex] & visibilityBit) == 0u)
                continue;
            const VulkanMaterialPayload* material =
                context.resources.materials().get(caster.material);
            telemetry.recordDraw(drawCounter, caster.indexCount / 3u);
            ++commandCounter;
            if (telemetry.collecting() && material != nullptr &&
                material->packed.alphaMode == 1u)
                ++alphaMaskCounter;
        }
    }

    void recordShadowDirectDraws(const VulkanFeatureContext& context,
        VkCommandBuffer commandBuffer, uint32_t frameIndex,
        const VulkanCasterScratch& scratch, const VulkanShadowDirectDraws& draws,
        bool& materialDescriptorsBound) {
        VulkanFrameTelemetry& telemetry = context.telemetry;
        VkPipeline activePipeline = VK_NULL_HANDLE;
        GeometryHandle activeGeometry{};
        for (size_t casterIndex = 0;
            casterIndex < scratch.casters.size(); ++casterIndex) {
            if ((scratch.visibility[casterIndex] & draws.visibilityBit) == 0u)
                continue;
            const VulkanResolvedCaster& caster = scratch.casters[casterIndex];
            if (draws.indirectValid && caster.gpuScenePrimitiveIndex !=
                    InvalidGpuSceneIndex)
                continue;
            VulkanGeometryPayload* geometry =
                context.resources.geometries().get(caster.geometry);
            VulkanMaterialPayload* material =
                context.resources.materials().get(caster.material);
            if (geometry == nullptr || material == nullptr) continue;
            const bool alphaMasked = material->packed.alphaMode == 1u;
            const bool doubleSided = material->packed.doubleSided != 0u;
            const VkPipeline pipeline = draws.pipeline(draws.pipelineOwner,
                alphaMasked, doubleSided);
            if (pipeline != activePipeline) {
                vkCmdBindPipeline(commandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                activePipeline = pipeline;
            }
            if (alphaMasked && !materialDescriptorsBound) {
                context.resources.bindMaterialDescriptors(commandBuffer,
                    frameIndex, draws.layout);
                materialDescriptorsBound = true;
            }
            if (caster.geometry != activeGeometry) {
                const VkDeviceSize offset = geometry->vertexOffset;
                vkCmdBindVertexBuffers(commandBuffer, 0, 1,
                    &geometry->vertexBuffer.buffer, &offset);
                vkCmdBindIndexBuffer(commandBuffer,
                    geometry->indexBuffer.buffer, 0,
                    toVkIndexType(geometry->indexFormat));
                activeGeometry = caster.geometry;
            }
            CanonicalMeshPushConstants push{};
            push.renderMatrix = caster.worldTransform;
            push.materialIndex = caster.material.getIndex();
            push.padding[0] = draws.slotWord;
            vkCmdPushConstants(commandBuffer, draws.layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), &push);
            vkCmdDrawIndexed(commandBuffer, caster.indexCount, 1,
                caster.firstIndex, 0, 0);
            telemetry.recordDraw(*draws.drawCounter, caster.indexCount / 3u);
            if (telemetry.collecting() && alphaMasked)
                ++*draws.alphaMaskCounter;
        }
    }

    IndirectLodMetric densityLodMetric(const DensityLodContext& context) {
        return { &context, [](const void* opaque,
            const VulkanIndirectScene& scene,
            const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
            const auto& lod = *static_cast<const DensityLodContext*>(opaque);
            return selectGpuSceneDensityLodGeometry(scene.geometries,
                primitive.binding.y, scene.instances[primitive.binding.x],
                lod.worldUnitsPerTexel, lod.errorTexels, maximumLod);
        } };
    }

    IndirectLodMetric perspectiveLodMetric(const PerspectiveLodContext& context) {
        return { &context, [](const void* opaque,
            const VulkanIndirectScene& scene,
            const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
            const auto& lod = *static_cast<const PerspectiveLodContext*>(opaque);
            const GpuSceneInstanceRecord& instance =
                scene.instances[primitive.binding.x];
            const glm::mat4 clipFromLocal = lod.worldToClip *
                unpackGpuSceneAffine(scene.transforms[instance.references.x]);
            return selectGpuSceneLodGeometry(scene.geometries,
                primitive.binding.y, clipFromLocal, lod.viewportPixels,
                lod.errorPixels, maximumLod);
        } };
    }

    IndirectLodMetric radialLodMetric(const RadialLodContext& context) {
        return { &context, [](const void* opaque,
            const VulkanIndirectScene& scene,
            const GpuScenePrimitiveRecord& primitive, uint32_t maximumLod) {
            const auto& lod = *static_cast<const RadialLodContext*>(opaque);
            return selectGpuSceneRadialLodGeometry(scene.geometries,
                primitive.binding.y, scene.instances[primitive.binding.x],
                lod.position, lod.resolution, lod.errorPixels, maximumLod);
        } };
    }

} // namespace Iridium
