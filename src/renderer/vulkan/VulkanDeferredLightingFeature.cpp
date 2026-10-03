#include "VulkanDeferredLightingFeature.h"

#include "VulkanClusterLightingFeature.h"
#include "VulkanFrameTargets.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanLocalShadowFeature.h"
#include "VulkanReflectionProbeFeature.h"
#include "VulkanResourceRegistry.h"
#include "VulkanShadowFeature.h"

#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

namespace Iridium {

    void VulkanDeferredLightingFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        pipeline_ = std::make_unique<VkLightingPipeline>(&context.vk,
            VulkanSceneColorFormat, gBufferLayout_);
    }

    void VulkanDeferredLightingFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        lightingPass_ = ids.lighting;
    }

    void VulkanDeferredLightingFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // R4a: dynamic rendering. The executor's transition of scene.color to
        // ColorAttachment (and the G-buffer reads) replaces the render pass's
        // external dependency.
        graph.registerPass(lightingPass_, { this, nullptr, &executeLighting, nullptr,
            GpuRangePlacement::BeforeBarriers, true });
    }

    void VulkanDeferredLightingFeature::destroy() noexcept {
        scene_.cleanup();
        pipeline_.reset();
        environment_ = {};
        neutralCube_ = {};
        neutralBrdfLut_ = {};
        context_ = nullptr;
    }

    void VulkanDeferredLightingFeature::createNeutralEnvironment() {
        if (neutralCube_.isValid() || neutralBrdfLut_.isValid()) {
            throw std::logic_error(
                "Neutral environment products were initialized twice.");
        }

        TextureDesc cubeDesc{};
        cubeDesc.width = 1;
        cubeDesc.height = 1;
        cubeDesc.format = TextureFormat::RGBA16_SFloat;
        cubeDesc.usageClass = TextureUsageClass::Environment;
        cubeDesc.arrayLayers = 6;
        cubeDesc.topology = TextureTopology::Cube;
        cubeDesc.sampler.addressU = SamplerAddressMode::ClampToEdge;
        cubeDesc.sampler.addressV = SamplerAddressMode::ClampToEdge;
        cubeDesc.sampler.addressW = SamplerAddressMode::ClampToEdge;

        // Six layer-major RGBA16F black texels. The same semantic neutral cube
        // is safe for irradiance, prefiltered radiance, and sky radiance.
        const std::array<std::byte, 6u * 4u * sizeof(uint16_t)> blackCube{};
        neutralCube_ = context_->resources.allocateTexture(cubeDesc, blackCube);

        TextureDesc brdfDesc{};
        brdfDesc.width = 1;
        brdfDesc.height = 1;
        brdfDesc.format = TextureFormat::RG16_SFloat;
        brdfDesc.usageClass = TextureUsageClass::Environment;
        brdfDesc.sampler.addressU = SamplerAddressMode::ClampToEdge;
        brdfDesc.sampler.addressV = SamplerAddressMode::ClampToEdge;
        brdfDesc.sampler.addressW = SamplerAddressMode::ClampToEdge;
        // Half-float (1, 0) is the identity split-sum fallback: F0 * 1 + F90 * 0.
        const std::array<uint16_t, 2> brdfIdentity{ 0x3c00u, 0u };
        neutralBrdfLut_ = context_->resources.allocateTexture(
            brdfDesc, std::as_bytes(std::span{ brdfIdentity }));
    }

    void VulkanDeferredLightingFeature::setEnvironment(
        const EnvironmentLightingHandles& environment) {
        if (!environment.isValid())
            throw std::invalid_argument(
                "Environment lighting requires four valid texture handles.");
        VulkanResourceRegistry& resources = context_->resources;
        const VulkanTexturePayload* radiance = resources.textures().get(environment.radiance);
        const VulkanTexturePayload* irradiance = resources.textures().get(environment.irradiance);
        const VulkanTexturePayload* prefiltered =
            resources.textures().get(environment.prefilteredSpecular);
        const VulkanTexturePayload* brdf = resources.textures().get(environment.brdfLut);
        if (radiance == nullptr || irradiance == nullptr || prefiltered == nullptr ||
            brdf == nullptr || radiance->retired || irradiance->retired ||
            prefiltered->retired || brdf->retired ||
            radiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            irradiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            brdf->image.viewType != VK_IMAGE_VIEW_TYPE_2D ||
            radiance->format != TextureFormat::RGBA16_SFloat ||
            irradiance->format != TextureFormat::RGBA16_SFloat ||
            prefiltered->format != TextureFormat::RGBA16_SFloat ||
            brdf->format != TextureFormat::RG16_SFloat) {
            throw std::invalid_argument(
                "Environment lighting textures do not match the cube/LUT contract.");
        }
        // Bind each frame's descriptor set only after its fence completes. View
        // switches must not idle both frames merely to choose a resident HDRI.
        environment_ = environment;
        for (VulkanTexturePayload* payload : {
                resources.textures().get(environment.radiance),
                resources.textures().get(environment.irradiance),
                resources.textures().get(environment.prefilteredSpecular),
                resources.textures().get(environment.brdfLut) })
            context_->allocator.reclassify(payload->image,
                ProfileMemoryCategory::Environment);
    }

    void VulkanDeferredLightingFeature::setEnvironmentSettings(
        const EnvironmentLightingSettings& settings) {
        if (!std::isfinite(settings.lightingIntensity) ||
            settings.lightingIntensity < 0.0f ||
            !std::isfinite(settings.backgroundIntensity) ||
            settings.backgroundIntensity < 0.0f ||
            !std::isfinite(settings.rotationRadians)) {
            throw std::invalid_argument(
                "Environment lighting settings must be finite and nonnegative.");
        }
        environmentSettings_ = settings;
    }

    void VulkanDeferredLightingFeature::bindFrameEnvironment(uint32_t frame) {
        if (frameEnvironments_[frame] != environment_)
            bindEnvironmentProducts(frame);
    }

    void VulkanDeferredLightingFeature::bindEnvironmentProducts(uint32_t frame) {
        const EnvironmentLightingHandles handles = environment_.isValid()
            ? environment_
            : EnvironmentLightingHandles{
                .radiance = neutralCube_,
                .irradiance = neutralCube_,
                .prefilteredSpecular = neutralCube_,
                .brdfLut = neutralBrdfLut_,
            };
        VulkanResourceRegistry& resources = context_->resources;
        const VulkanTexturePayload* radiance = resources.textures().get(handles.radiance);
        const VulkanTexturePayload* irradiance = resources.textures().get(handles.irradiance);
        const VulkanTexturePayload* prefiltered =
            resources.textures().get(handles.prefilteredSpecular);
        const VulkanTexturePayload* brdf = resources.textures().get(handles.brdfLut);
        if (radiance == nullptr || irradiance == nullptr || prefiltered == nullptr ||
            brdf == nullptr || radiance->retired || irradiance->retired ||
            prefiltered->retired || brdf->retired ||
            radiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            irradiance->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            brdf->image.viewType != VK_IMAGE_VIEW_TYPE_2D) {
            throw std::logic_error(
                "Neutral environment products are unavailable or incompatible.");
        }
        const VkDescriptorImageInfo radianceInfo{ radiance->sampler,
            radiance->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo irradianceInfo{ irradiance->sampler,
            irradiance->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo prefilteredInfo{ prefiltered->sampler,
            prefiltered->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo brdfInfo{ brdf->sampler, brdf->image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        scene_.setEnvironmentImages({
            .irradiance = irradianceInfo,
            .prefilteredRadiance = prefilteredInfo,
            .brdfLut = brdfInfo,
            .skyRadiance = radianceInfo,
        }, frame);
        if (frame == UINT32_MAX) frameEnvironments_.fill(environment_);
        else frameEnvironments_[frame] = environment_;
    }

    void VulkanDeferredLightingFeature::rebuildSceneSet() {
        scene_.init(context_->device, context_->descriptors,
            pipeline_->getDescriptorSetLayout());
        bindLightBuffers();
        bindEnvironmentProducts();
        bindShadows();
        bindReflectionProbeBuffers();
        bindReflectionProbeEnvironments();
        scene_.rebuild(context_->frameTargets);
    }

    void VulkanDeferredLightingFeature::bindLightBuffers(uint32_t frame) {
        scene_.setLightBuffers(sources_.clusters->lightRecordDescriptors(), frame);
        scene_.setClusterBuffers(sources_.clusters->sceneClusterDescriptors(), frame);
    }

    void VulkanDeferredLightingFeature::bindShadows() {
        // Directional, spot, then point (binding order of the lighting set).
        std::vector<VkDescriptorBufferInfo> frameData;
        frameData.reserve(FrameCount);
        for (uint32_t frame = 0; frame < FrameCount; ++frame)
            frameData.push_back(sources_.shadows->map().sampleBuffer(frame));
        scene_.setDirectionalShadow({
            sources_.shadows->map().sampleImage(), std::move(frameData) });

        frameData.clear();
        frameData.reserve(FrameCount);
        for (uint32_t frame = 0; frame < FrameCount; ++frame)
            frameData.push_back(sources_.localShadows->spot().sampleBuffer(frame));
        scene_.setSpotShadow({
            sources_.localShadows->spot().sampleImage(), std::move(frameData) });

        frameData.clear();
        frameData.reserve(FrameCount);
        for (uint32_t frame = 0; frame < FrameCount; ++frame)
            frameData.push_back(sources_.localShadows->point().sampleBuffer(frame));
        scene_.setPointShadow({ sources_.localShadows->point().sampleImages(),
            std::move(frameData) });
    }

    void VulkanDeferredLightingFeature::bindReflectionProbeBuffers(uint32_t frame) {
        const VulkanReflectionProbeFeature::BufferDescriptors buffers =
            sources_.probes->bufferDescriptors();
        scene_.setReflectionProbeBuffers(buffers.scene, frame);
        if (frame == UINT32_MAX) {
            sources_.clusters->probeClusterPipeline().rebuildDescriptors(
                buffers.records, buffers.active, buffers.parameters,
                buffers.headers, buffers.indices);
            return;
        }
        // One slot: rewrite its probe-clustering set in place (the other
        // slot's set may still be in use).
        sources_.clusters->probeClusterPipeline().rewriteDescriptors(frame,
            buffers.records[frame], buffers.active[frame],
            buffers.parameters[frame], buffers.headers[frame],
            buffers.indices[frame]);
    }

    void VulkanDeferredLightingFeature::bindReflectionProbeEnvironments(
        uint32_t frame) {
        const VulkanTexturePayload* neutral = context_->resources.textures().get(
            neutralCube_);
        if (neutral == nullptr || neutral->retired ||
            neutral->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE)
            throw std::logic_error(
                "Neutral reflection-probe environment is unavailable");
        const VkDescriptorImageInfo fallback{ neutral->sampler,
            neutral->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        scene_.setReflectionProbeImages(
            sources_.probes->environmentImages(fallback), frame);
    }

    void VulkanDeferredLightingFeature::record(const FrameInputs& inputs) {
        staged_ = inputs;
        context_->graph.drainRegisteredThrough(lightingPass_);
    }

    void VulkanDeferredLightingFeature::executeLighting(void* owner,
        VulkanPassContext& context) {
        static_cast<VulkanDeferredLightingFeature*>(owner)->recordLighting(context);
    }

    void VulkanDeferredLightingFeature::recordLighting(VulkanPassContext& context) {
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frame = context.frame.frameIndex;
        VulkanFrameScheduler& scheduler = context_->scheduler;
        const VulkanFrameTargets& frameTargets = context_->frameTargets;
        VulkanFrameTelemetry& telemetry = context_->telemetry;

        // scene.color: CLEAR (0,0,0,1) / STORE from the graph declaration.
        VulkanRenderingOverrides rendering{};
        rendering.renderArea = { { 0, 0 }, frameTargets.extent() };
        context.beginRendering(rendering);
        VulkanGpuRangeToken deferredGpuRange =
            scheduler.beginGpuRange("gpu.lighting.deferred");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->getPipeline());
        telemetry.recordPipelineBind(pipelineIdentity(FixedPipelineIdentity::DeferredLighting));

        // The lighting set: G-buffer, lights, clusters, environment, shadows, probes.
        const VkDescriptorSet sceneSet = scene_.get(frame);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_->getPipelineLayout(), 0, 1, &sceneSet, 0, nullptr);

        LightingPushConstants push{};
        push.viewPos = glm::vec4(staged_.cameraPosition, 1.0f);
        push.invView = glm::inverse(staged_.view);
        push.invProj = glm::inverse(staged_.projection);
        push.debugView = glm::ivec4(static_cast<int32_t>(staged_.debugView), 0, 0, 0);

        vkCmdPushConstants(cmd, pipeline_->getPipelineLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(LightingPushConstants), &push);

        // Draw the full screen triangle without vertex buffers
        vkCmdDraw(cmd, 3, 1, 0, 0);
        telemetry.recordDraw(telemetry.counters().drawLighting, 1);
        scheduler.endGpuRange(deferredGpuRange);

        context.endRendering();
    }

} // namespace Iridium
