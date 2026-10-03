#include "VulkanReflectionProbeFeature.h"

#include "VkContext.h"
#include "VulkanClusterLightingFeature.h"
#include "VulkanFrameTelemetry.h"
#include "VulkanGpuSceneState.h"
#include "VulkanMeshLayouts.h"
#include "VulkanResourceRegistry.h"
#include "renderer/lighting/ClusteredReflectionProbes.h"
#include "renderer/lighting/ShadowCasterCulling.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ReflectionProbeTypes.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Iridium {

    void VulkanReflectionProbeFeature::configureCaptures(
        const ProjectReflectionProbeSettings& settings) {
        if (settings.prefilterSampleCount < 64u ||
            settings.prefilterSampleCount > 1024u)
            throw std::invalid_argument(
                "Reflection-probe prefilter sample count is invalid");
        prefilterSampleCount_ = settings.prefilterSampleCount;
    }

    void VulkanReflectionProbeFeature::create(const VulkanFeatureContext& context) {
        context_ = &context;
        capturePass_.init(context.device, context.vk.getPhysicalDevice(),
            context.allocator, context.descriptors,
            context.resources.textureTable().materialViewLayout(),
            context.resources.textureTable().samplerLayout(),
            lightingSetLayout_, context.meshLayouts.getGpuSceneSetLayout());
        captureTargets_.init(context.device, context.vk.getPhysicalDevice(),
            context.allocator);
        captureTargets_.setDeferredDestruction(&context.scheduler);
    }

    void VulkanReflectionProbeFeature::createCuller(const VulkanIndirectViewSetup& setup) {
        if (setup.indirectLayout == VK_NULL_HANDLE ||
            capturePass_.captureSetLayout() == VK_NULL_HANDLE)
            throw std::logic_error(
                "reflection-probe indirect resources require capture and command layouts");
        culler_.init(setup.services, IndirectViewKind::ReflectionProbe,
            createIndirectViewPipeline(context_->device,
                IndirectViewKind::ReflectionProbe, capturePass_.captureSetLayout(),
                context_->meshLayouts.getGpuSceneSetLayout(), setup.indirectLayout),
            setup.indirectLayout,
            activeIndirectOracle(context_->extensions,
                VulkanIndirectOracleView::ReflectionProbe));
        culler_.resize(512u, context_->frameOpen);
    }

    void VulkanReflectionProbeFeature::createInitialBuffers(VkExtent2D sceneExtent) {
        recordMaximumCapacity_ = (std::min)(
            static_cast<uint32_t>(
                context_->vk.getPhysicalDeviceProperties()
                    .limits.maxStorageBufferRange /
                sizeof(PackedGpuReflectionProbe)),
            kMaximumGpuReflectionProbeCapacity);
        if (recordMaximumCapacity_ == 0)
            throw std::runtime_error(
                "Vulkan storage-buffer range cannot hold one reflection probe");
        const ClusterGridDimensions initialProbeGrid = clusterGridDimensions(
            clusterConfig_, { sceneExtent.width, sceneExtent.height,
                0.1f, 100.0f, glm::mat4(1.0f), glm::mat4(1.0f) });
        const uint32_t initialProbeClusters = static_cast<uint32_t>(
            initialProbeGrid.clusterCount());
        const uint32_t initialProbeReferences = static_cast<uint32_t>(
            (std::min)(initialProbeGrid.clusterCount() *
                kMaximumReflectionProbesPerCluster,
                static_cast<uint64_t>(kMaximumClusterProbeReferences)));
        createBuffers((std::min)(kInitialGpuReflectionProbeCapacity,
            recordMaximumCapacity_), initialProbeClusters, initialProbeReferences);
    }

    void VulkanReflectionProbeFeature::onGraphRebuilt(const VulkanProductionGraphIds& ids) {
        capturePassId_ = ids.probeCapture;
    }

    void VulkanReflectionProbeFeature::registerPasses(VulkanRenderGraphExecutor& graph) {
        // R3b.8: reads the shadow maps; the staging and the per-face
        // compaction keep their barriers inside the pass. Its range opens
        // inside the callback (after the culler's host -> compute barrier).
        graph.registerPass(capturePassId_, { this, &captureActive, &executeCapture });
    }

    void VulkanReflectionProbeFeature::destroy() noexcept {
        if (context_ != nullptr) {
            culler_.destroy(context_->device);
            for (PendingCapture& pending : pendingCaptures_) {
                capturePass_.releaseDescriptors(pending.filterDescriptors);
                context_->allocator.destroy(pending.bakedReadback.buffer);
            }
            for (auto* buffers : { &recordBuffers_, &activeSlotBuffers_,
                    &parameterBuffers_, &clusterHeaderBuffers_, &clusterIndexBuffers_,
                    &pendingRecordBuffers_, &pendingActiveSlotBuffers_,
                    &pendingParameterBuffers_, &pendingClusterHeaderBuffers_,
                    &pendingClusterIndexBuffers_ })
                for (VulkanBufferResource& buffer : *buffers)
                    context_->allocator.destroy(buffer);
        }
        pendingSlots_ = {};
        pendingCaptures_.clear();
        captureTargets_.cleanup();
        capturePass_.cleanup();
        environments_.clear();
        capturedSlots_.clear();
        telemetry_ = {};
        recordCapacity_ = 0;
        recordMaximumCapacity_ = 0;
        clusterCapacity_ = 0;
        referenceCapacity_ = 0;
        context_ = nullptr;
    }

    void VulkanReflectionProbeFeature::growIndirectCapacity(uint32_t primitiveCapacity) {
        if (primitiveCapacity > culler_.primitiveCapacity())
            culler_.resize(primitiveCapacity, context_->frameOpen);
    }

    void VulkanReflectionProbeFeature::notifyBuffersReplaced(
        uint32_t swappedSlots) const {
        if (bindings_.buffersReplaced != nullptr)
            bindings_.buffersReplaced(bindings_.owner, swappedSlots);
    }

    void VulkanReflectionProbeFeature::notifyEnvironmentsChanged() const {
        if (bindings_.environmentsChanged != nullptr)
            bindings_.environmentsChanged(bindings_.owner);
    }

    void VulkanReflectionProbeFeature::createBuffers(uint32_t recordCapacity,
        uint32_t clusterCapacity, uint32_t referenceCapacity) {
        if (recordCapacity == 0 ||
            recordCapacity > recordMaximumCapacity_ ||
            clusterCapacity == 0 || referenceCapacity == 0 ||
            referenceCapacity > kMaximumClusterProbeReferences)
            throw std::invalid_argument(
                "Reflection-probe GPU capacity is invalid");
        if (context_->frameOpen)
            throw std::logic_error(
                "Reflection-probe buffers may grow only at a frame boundary");
        VulkanResourceAllocator& allocator = context_->allocator;
        const VkDeviceSize recordBytes = static_cast<VkDeviceSize>(
            recordCapacity) * sizeof(PackedGpuReflectionProbe);
        const VkDeviceSize activeBytes = static_cast<VkDeviceSize>(
            recordCapacity) * sizeof(uint32_t);
        const VkDeviceSize headerBytes = static_cast<VkDeviceSize>(
            clusterCapacity) * sizeof(ClusterLightHeader);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(
            referenceCapacity) * sizeof(uint32_t);
        FrameBuffers records{};
        FrameBuffers active{};
        FrameBuffers parameters{};
        FrameBuffers headers{};
        FrameBuffers indices{};
        try {
            for (uint32_t frame = 0; frame < FrameCount; ++frame) {
                records[frame] = allocator.createBuffer(recordBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                active[frame] = allocator.createBuffer(activeBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                parameters[frame] = allocator.createBuffer(
                    sizeof(PackedGpuReflectionProbeClusterParameters),
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true, ProfileMemoryCategory::Environment);
                headers[frame] = allocator.createBuffer(headerBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    false, ProfileMemoryCategory::Environment);
                indices[frame] = allocator.createBuffer(indexBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    false, ProfileMemoryCategory::Environment);
                std::memset(records[frame].mapped, 0,
                    static_cast<size_t>(recordBytes));
                std::memset(active[frame].mapped, 0,
                    static_cast<size_t>(activeBytes));
                std::memset(parameters[frame].mapped, 0,
                    sizeof(PackedGpuReflectionProbeClusterParameters));
            }
        }
        catch (...) {
            for (uint32_t frame = 0; frame < FrameCount; ++frame) {
                allocator.destroy(records[frame]);
                allocator.destroy(active[frame]);
                allocator.destroy(parameters[frame]);
                allocator.destroy(headers[frame]);
                allocator.destroy(indices[frame]);
            }
            throw;
        }
        // R4c.2: no drain. A slot that is not in flight swaps now; an
        // in-flight slot parks its replacement (an older parked one was never
        // used) until its retirement. With every slot idle (startup) the
        // probe-clustering sets are rebuilt as before.
        const std::array<FrameBuffers*, 5> current{ &recordBuffers_,
            &activeSlotBuffers_, &parameterBuffers_, &clusterHeaderBuffers_,
            &clusterIndexBuffers_ };
        const std::array<FrameBuffers*, 5> pending{ &pendingRecordBuffers_,
            &pendingActiveSlotBuffers_, &pendingParameterBuffers_,
            &pendingClusterHeaderBuffers_, &pendingClusterIndexBuffers_ };
        const std::array<FrameBuffers*, 5> replacement{ &records, &active,
            &parameters, &headers, &indices };
        uint32_t swappedSlots = 0;
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            for (FrameBuffers* buffers : pending) allocator.destroy((*buffers)[frame]);
            pendingSlots_[frame] = context_->scheduler.slotInFlight(frame);
            if (!pendingSlots_[frame]) swappedSlots |= 1u << frame;
        }
        // The probe-clustering sets reference the replaced buffers.
        if (swappedSlots == AllSlots)
            clusters_->probeClusterPipeline().clearDescriptors();
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            for (size_t table = 0; table < current.size(); ++table) {
                FrameBuffers& target = pendingSlots_[frame]
                    ? *pending[table] : *current[table];
                if (!pendingSlots_[frame]) allocator.destroy(target[frame]);
                target[frame] = (*replacement[table])[frame];
            }
        }
        recordCapacity_ = recordCapacity;
        clusterCapacity_ = clusterCapacity;
        referenceCapacity_ = referenceCapacity;
        for (auto& revisions : uploadedRevisions_)
            revisions.assign(recordCapacity, uint64_t{ 0 });
        uploadedActiveListRevisions_.fill(0);
        uploadRanges_.reserve(recordCapacity);
        notifyBuffersReplaced(swappedSlots);
    }

    bool VulkanReflectionProbeFeature::swapRetiredSlot(uint32_t slot) {
        if (!pendingSlots_[slot]) return false;
        for (auto [target, source] : { std::pair{ &recordBuffers_, &pendingRecordBuffers_ },
                std::pair{ &activeSlotBuffers_, &pendingActiveSlotBuffers_ },
                std::pair{ &parameterBuffers_, &pendingParameterBuffers_ },
                std::pair{ &clusterHeaderBuffers_, &pendingClusterHeaderBuffers_ },
                std::pair{ &clusterIndexBuffers_, &pendingClusterIndexBuffers_ } }) {
            context_->allocator.destroy((*target)[slot]);
            (*target)[slot] = (*source)[slot];
            (*source)[slot] = {};
        }
        pendingSlots_[slot] = false;
        return true;
    }

    VulkanReflectionProbeFeature::BufferDescriptors
        VulkanReflectionProbeFeature::bufferDescriptors() const noexcept {
        BufferDescriptors descriptors{};
        const auto info = [](const VulkanBufferResource& buffer) {
            return VkDescriptorBufferInfo{ buffer.buffer, 0, buffer.size };
        };
        for (uint32_t frame = 0; frame < FrameCount; ++frame) {
            descriptors.records[frame] = info(recordBuffers_[frame]);
            descriptors.active[frame] = info(activeSlotBuffers_[frame]);
            descriptors.parameters[frame] = info(parameterBuffers_[frame]);
            descriptors.headers[frame] = info(clusterHeaderBuffers_[frame]);
            descriptors.indices[frame] = info(clusterIndexBuffers_[frame]);
            descriptors.scene[frame] = { descriptors.records[frame],
                descriptors.headers[frame], descriptors.indices[frame] };
        }
        return descriptors;
    }

    std::array<VkDescriptorImageInfo, kMaximumGpuReflectionProbeEnvironments>
        VulkanReflectionProbeFeature::environmentImages(
            const VkDescriptorImageInfo& fallback) const {
        std::array<VkDescriptorImageInfo,
            kMaximumGpuReflectionProbeEnvironments> images{};
        images.fill(fallback);
        for (size_t index = 0; index < environments_.size(); ++index) {
            const EnvironmentLightingHandles& environment = environments_[index];
            const VulkanTexturePayload* prefiltered = context_->resources.textures().get(
                environment.prefilteredSpecular);
            if (prefiltered == nullptr || prefiltered->retired ||
                prefiltered->image.viewType != VK_IMAGE_VIEW_TYPE_CUBE ||
                prefiltered->format != TextureFormat::RGBA16_SFloat)
                throw std::invalid_argument(
                    "Local reflection-probe environment is incompatible");
            images[index] = { prefiltered->sampler,
                prefiltered->image.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        for (const auto& [owner, slot] : capturedSlots_) {
            if (slot >= images.size())
                throw std::logic_error(
                    "Captured reflection-probe table slot is invalid");
            const VulkanImageResource* published = captureTargets_.published(owner);
            if (published == nullptr || !published->isValid())
                throw std::logic_error(
                    "Captured reflection-probe product is unavailable");
            images[slot] = { capturePass_.sampler(),
                published->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        return images;
    }

    std::optional<uint32_t> VulkanReflectionProbeFeature::capturedEnvironmentSlot(
        SceneEntityUuid owner) const noexcept {
        const auto found = capturedSlots_.find(owner);
        return found == capturedSlots_.end()
            ? std::optional<uint32_t>{}
            : std::optional<uint32_t>{ found->second };
    }

    void VulkanReflectionProbeFeature::synchronizeCaptureOwners(
        std::span<const SceneEntityUuid> owners) {
        if (context_->frameOpen)
            throw std::logic_error(
                "Reflection-probe owners must synchronize before beginFrame");
        const auto retained = [&](SceneEntityUuid owner) {
            return std::ranges::find(owners, owner) != owners.end();
        };
        const bool hasRemoved = std::ranges::any_of(capturedSlots_,
            [&](const auto& entry) { return !retained(entry.first); });
        if (!hasRemoved) return;
        // R4c.3: no drain. A removed owner's targets go to the deletion
        // queue (frames in flight may still sample them); a capture of it
        // that has not been promoted yet is dropped the same way.
        for (auto pending = pendingCaptures_.begin(); pending != pendingCaptures_.end();) {
            if (retained(pending->owner)) { ++pending; continue; }
            for (VkDescriptorSet set : pending->filterDescriptors)
                context_->scheduler.retireDescriptorSet(context_->descriptors, set);
            context_->scheduler.retire(pending->bakedReadback.buffer);
            pending = pendingCaptures_.erase(pending);
        }
        for (auto current = capturedSlots_.begin(); current != capturedSlots_.end();) {
            if (retained(current->first)) { ++current; continue; }
            captureTargets_.remove(current->first);
            current = capturedSlots_.erase(current);
        }
        telemetry_.publishedLogicalBytes = captureTargets_.publishedLogicalBytes();
        notifyEnvironmentsChanged();
    }

    std::vector<ReflectionProbeCaptureCompletion>
        VulkanReflectionProbeFeature::finalizeCaptures() {
        if (context_->frameOpen)
            throw std::logic_error(
                "Reflection-probe captures must finalize before beginFrame");
        std::vector<ReflectionProbeCaptureCompletion> completed;
        if (pendingCaptures_.empty()) return completed;
        // R4c.3: no drain. Only captures whose recording frame has completed
        // are promoted (their prefilter and readback are then CPU-visible);
        // the rest stay pending for a later frame.
        context_->scheduler.refreshCompletedSerial();
        const uint64_t completedSerial = context_->scheduler.completedSerial();
        const auto ready = [completedSerial](const PendingCapture& pending) {
            return pending.recordSerial <= completedSerial;
        };
        if (std::ranges::none_of(pendingCaptures_, ready)) return completed;
        completed.reserve(pendingCaptures_.size());
        for (PendingCapture& pending : pendingCaptures_) {
            if (!ready(pending)) continue;
            capturePass_.releaseDescriptors(pending.filterDescriptors);
            captureTargets_.promote(pending.owner, pending.captureTicket);
            auto found = capturedSlots_.find(pending.owner);
            if (found == capturedSlots_.end()) {
                std::array<bool, kMaximumGpuReflectionProbeEnvironments> used{};
                for (const auto& [owner, slot] : capturedSlots_) {
                    (void)owner;
                    if (slot < used.size()) used[slot] = true;
                }
                uint32_t slot = kInvalidEnvironmentTableSlot;
                for (uint32_t candidate = kMaximumGpuReflectionProbeEnvironments;
                    candidate-- > 0u;) {
                    if (!used[candidate]) { slot = candidate; break; }
                }
                if (slot == kInvalidEnvironmentTableSlot)
                    throw std::overflow_error(
                        "Captured reflection-probe table is exhausted");
                found = capturedSlots_.emplace(pending.owner, slot).first;
            }
            ReflectionProbeCaptureCompletion completion{
                .owner = pending.owner,
                .captureTicket = pending.captureTicket,
                .environmentSlot = found->second,
            };
            if (pending.bakedReadback.buffer.isValid()) {
                if (pending.bakedReadback.buffer.mapped == nullptr)
                    throw std::logic_error(
                        "Reflection-probe baked readback is not mapped");
                ReflectionProbeCaptureCompletion::Product product{
                    .resolution = pending.resolution,
                    .mipLevels = pending.mipLevels,
                };
                const auto* bytes = static_cast<const std::byte*>(
                    pending.bakedReadback.buffer.mapped);
                product.radiance.assign(bytes,
                    bytes + pending.bakedReadback.radianceBytes);
                product.prefilteredSpecular.assign(
                    bytes + pending.bakedReadback.radianceBytes,
                    bytes + pending.bakedReadback.radianceBytes +
                        pending.bakedReadback.prefilteredBytes);
                completion.bakedProduct = std::move(product);
                context_->allocator.destroy(pending.bakedReadback.buffer);
            }
            completed.push_back(std::move(completion));
        }
        std::erase_if(pendingCaptures_, ready);
        telemetry_.capturesPublished += static_cast<uint32_t>(completed.size());
        telemetry_.capturesInFlight = captureTargets_.capturesInFlight();
        telemetry_.stagingLogicalBytes = captureTargets_.stagingLogicalBytes();
        telemetry_.publishedLogicalBytes = captureTargets_.publishedLogicalBytes();
        notifyEnvironmentsChanged();
        return completed;
    }

    void VulkanReflectionProbeFeature::prepare(uint32_t requiredCapacity,
        std::span<const EnvironmentLightingHandles> environments,
        VkExtent2D sceneExtent) {
        if (requiredCapacity > recordMaximumCapacity_)
            throw std::overflow_error(
                "GPU reflection-probe records exhausted the device limit");
        if (environments.size() > kMaximumGpuReflectionProbeEnvironments)
            throw std::overflow_error(
                "Reflection-probe environment table exhausted its capacity");
        for (const auto& [owner, slot] : capturedSlots_) {
            (void)owner;
            if (slot < environments.size())
                throw std::overflow_error(
                    "Asset and captured reflection-probe table slots overlap");
        }
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_, { sceneExtent.width, sceneExtent.height,
                0.1f, 100.0f, glm::mat4(1.0f), glm::mat4(1.0f) });
        if (dimensions.clusterCount() > (std::numeric_limits<uint32_t>::max)())
            throw std::overflow_error(
                "Reflection-probe cluster grid exceeds 32-bit addressing");
        const uint32_t clusterCapacity = static_cast<uint32_t>(
            dimensions.clusterCount());
        const uint32_t referenceCapacity = static_cast<uint32_t>((std::min)(
            dimensions.clusterCount() * kMaximumReflectionProbesPerCluster,
            static_cast<uint64_t>(kMaximumClusterProbeReferences)));
        uint32_t recordCapacity = recordCapacity_;
        if (requiredCapacity > recordCapacity)
            recordCapacity = nextMaterialTableCapacity(recordCapacity,
                requiredCapacity, recordMaximumCapacity_);
        if (recordCapacity != recordCapacity_ ||
            clusterCapacity != clusterCapacity_ ||
            referenceCapacity != referenceCapacity_)
            createBuffers(recordCapacity, clusterCapacity, referenceCapacity);

        const bool environmentsChanged =
            environments.size() != environments_.size() ||
            !std::equal(environments.begin(), environments.end(),
                environments_.begin(), environments_.end());
        if (environmentsChanged) {
            for (const EnvironmentLightingHandles& environment : environments)
                if (!environment.isValid())
                    throw std::invalid_argument(
                        "Reflection-probe table contains an invalid environment");
            // R4c.3: no drain; each slot rebinds the table before its next
            // frame (the lighting-set owner), and textures dropped from it
            // are retired through the deletion queue by freeTexture.
            environments_.assign(environments.begin(), environments.end());
            notifyEnvironmentsChanged();
        }
    }

    void VulkanReflectionProbeFeature::uploadFrame(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection, float nearPlane,
        float farPlane, const ReflectionProbeGpuFramePacket& probes,
        VkExtent2D sceneExtent) {
        uploadRecords(frameIndex, probes);
        updateParameters(frameIndex, view, projection, nearPlane, farPlane,
            probes.stats.activeProbeCount, sceneExtent);
    }

    void VulkanReflectionProbeFeature::uploadRecords(uint32_t frameIndex,
        const ReflectionProbeGpuFramePacket& probes) {
        CpuProfiler* const profiler = context_->profiler;
        CpuScope uploadScope(profiler, "cpu.probe.upload");
        if (frameIndex >= recordBuffers_.size() ||
            probes.records.size() > recordCapacity_ ||
            probes.recordRevisions.size() < probes.records.size() ||
            probes.activeSlots.size() > recordCapacity_)
            throw std::out_of_range(
                "Reflection-probe packet is outside prepared capacity");
        std::vector<uint64_t>& uploaded = uploadedRevisions_[frameIndex];
        uploadRanges_.clear();
        uint32_t index = 0;
        while (index < probes.records.size()) {
            if (probes.recordRevisions[index] == uploaded[index]) {
                ++index;
                continue;
            }
            const uint32_t first = index++;
            while (index < probes.records.size() &&
                probes.recordRevisions[index] != uploaded[index]) ++index;
            uploadRanges_.push_back({ first, index - first });
        }
        uint64_t uploadedBytes = 0;
        for (const ReflectionProbeRecordRange range : uploadRanges_) {
            const auto records = probes.records.subspan(
                range.firstRecord, range.recordCount);
            context_->allocator.write(recordBuffers_[frameIndex],
                static_cast<VkDeviceSize>(range.firstRecord) *
                    sizeof(PackedGpuReflectionProbe),
                std::as_bytes(records));
            for (uint32_t slot = range.firstRecord;
                slot < range.firstRecord + range.recordCount; ++slot)
                uploaded[slot] = probes.recordRevisions[slot];
            uploadedBytes += static_cast<uint64_t>(range.recordCount) *
                sizeof(PackedGpuReflectionProbe);
        }
        if (uploadedActiveListRevisions_[frameIndex] != probes.activeListRevision) {
            if (!probes.activeSlots.empty())
                context_->allocator.write(activeSlotBuffers_[frameIndex], 0,
                    std::as_bytes(probes.activeSlots));
            uploadedActiveListRevisions_[frameIndex] = probes.activeListRevision;
            uploadedBytes += probes.activeSlots.size() * sizeof(uint32_t);
        }
        if (profiler != nullptr) {
            profiler->recordCounter("probe.gpu_upload_bytes",
                uploadedBytes, ProfileCounterStatus::Exact,
                ProfileCounterUnit::Bytes);
            profiler->recordCounter("probe.gpu_upload_ranges",
                uploadRanges_.size());
        }
    }

    void VulkanReflectionProbeFeature::updateParameters(uint32_t frameIndex,
        const glm::mat4& view, const glm::mat4& projection,
        float nearPlane, float farPlane, uint32_t activeProbeCount,
        VkExtent2D sceneExtent) {
        if (frameIndex >= parameterBuffers_.size() ||
            !(nearPlane > 0.0f) || !(farPlane > nearPlane))
            throw std::invalid_argument(
                "Invalid reflection-probe cluster frame");
        const ClusterFrameParameters frame{ sceneExtent.width,
            sceneExtent.height, nearPlane, farPlane, view, projection };
        const ClusterGridDimensions dimensions = clusterGridDimensions(
            clusterConfig_, frame);
        PackedGpuReflectionProbeClusterParameters parameters{};
        parameters.view = view;
        parameters.projection = projection;
        parameters.inverseView = glm::inverse(view);
        parameters.grid = { sceneExtent.width, sceneExtent.height,
            dimensions.tilesX, dimensions.tilesY };
        parameters.depth = { nearPlane, farPlane,
            static_cast<float>(clusterConfig_.depthSlices) /
                std::log(farPlane / nearPlane), 0.0f };
        parameters.limits = { clusterConfig_.depthSlices,
            kMaximumReflectionProbesPerCluster,
            referenceCapacity_, activeProbeCount };
        parameters.tiles = { clusterConfig_.tileWidth,
            clusterConfig_.tileHeight, 0u, 0u };
        context_->allocator.write(parameterBuffers_[frameIndex],
            0, std::as_bytes(std::span(&parameters, size_t{ 1 })));
    }

    void VulkanReflectionProbeFeature::submitCaptures(
        const ReflectionProbeCasterSubmission& casters,
        std::span<const ReflectionProbeCaptureScheduleEntry> captures,
        const LightingFramePacket& lights, VkDescriptorSet sceneSet) {
        const bool hasWork = std::ranges::any_of(captures,
            [](const ReflectionProbeCaptureScheduleEntry& capture) {
                return capture.scheduledFaceMask != 0u;
            });
        captureHandled_ = true;
        stagedActive_ = false;
        if (!hasWork) {
            // "probe.capture" is skipped.
            context_->graph.drainRegisteredThrough(capturePassId_);
            return;
        }
        CpuScope recordScope(context_->profiler, "cpu.render.record.probe_capture");
        stagedCasters_ = casters;
        stagedCaptures_ = captures;
        stagedLights_ = &lights;
        stagedSceneSet_ = sceneSet;
        stagedActive_ = true;
        context_->graph.drainRegisteredThrough(capturePassId_);
        stagedActive_ = false;
    }

    void VulkanReflectionProbeFeature::skipCaptureIfUnhandled() {
        if (captureHandled_) return;
        // Frames that never submit probe captures (asset preview) skip the
        // declared pass before the opaque compaction.
        stagedActive_ = false;
        context_->graph.drainRegisteredThrough(capturePassId_);
        captureHandled_ = true;
    }

    bool VulkanReflectionProbeFeature::prepareIndirect(VkCommandBuffer commandBuffer,
        uint32_t frameIndex) {
        if (!culler_.plan({
                .primitiveIndices = stagedCasters_.gpuScenePrimitiveIndices,
                .membershipRevision = stagedCasters_.membershipRevision,
                .lodErrorThreshold = settings_.lodErrorThreshold,
                .lodMaximumLevel = settings_.lodMaximumLevel,
                .forceDirectGBufferReference = settings_.forceDirectGBufferReference,
                .forceDirectShadowReference = settings_.forceDirectShadowReference,
                .scene = context_->gpuScene.indirectScene(frameIndex),
                .assets = vulkanIndirectAssets(*context_),
            }, reflectionProbeWork(stagedCaptures_), frameIndex))
            return false;
        // Per-face dispatches follow (recordWorkItem).
        context_->telemetry.counters().dispatchRecorded +=
            culler_.recordCompaction(commandBuffer, frameIndex, {});
        return true;
    }

    bool VulkanReflectionProbeFeature::captureActive(void* owner,
        const VulkanFrameRecordContext&) {
        return static_cast<const VulkanReflectionProbeFeature*>(owner)->stagedActive_;
    }

    void VulkanReflectionProbeFeature::executeCapture(void* owner,
        VulkanPassContext& context) {
        auto& self = *static_cast<VulkanReflectionProbeFeature*>(owner);
        const VulkanFeatureContext& feature = *self.context_;
        VulkanCasterScratch& scratch = *self.scratch_;
        VulkanReflectionProbeCapturePass& capturePass = self.capturePass_;
        const VkCommandBuffer cmd = context.commandBuffer;
        const uint32_t frameIndex = context.frame.frameIndex;
        const LightingFramePacket& lights = *self.stagedLights_;
        const ReflectionProbeCasterSubmission& probeCasters = self.stagedCasters_;
        auto& counters = feature.telemetry.counters();
        ReflectionProbeCaptureTelemetry& captureTelemetry = self.telemetry_;
        self.clusters_->uploadLights(frameIndex, lights);
        const VkDescriptorSet sceneSet = self.stagedSceneSet_;
        const VkPipelineLayout layout = capturePass.graphicsLayout();
        const VkPipelineLayout gpuSceneLayout = capturePass.gpuSceneGraphicsLayout();
        resolveCasters(feature.gpuScene.indirectScene(frameIndex), probeCasters,
            GpuSceneConsumerProbe, scratch);
        const bool indirectValid = self.prepareIndirect(cmd, frameIndex);
        IVulkanIndirectOracle* const shadowOracle = activeIndirectOracle(
            feature.extensions, VulkanIndirectOracleView::ReflectionProbe);
        const bool probeQualificationOracle = shadowOracle != nullptr;
        const uint64_t resolvedGpuSceneCasters =
            std::ranges::count_if(scratch.casters,
                [](const VulkanResolvedCaster& caster) {
                    return caster.gpuScenePrimitiveIndex != InvalidGpuSceneIndex;
                });
        const uint64_t resolvedDirectCasters =
            scratch.casters.size() - resolvedGpuSceneCasters;
        uint64_t casterFaceTests = 0;
        uint64_t casterFacesCulled = 0;
        uint64_t casterFaceDraws = 0;
        uint64_t gpuSceneFaceDraws = 0;
        uint64_t directFaceDraws = 0;
        uint64_t ownerFaceExclusions = 0;
        uint32_t faceRecord = 0;
        VulkanGpuRangeToken captureRange =
            feature.scheduler.beginGpuRange("gpu.probe.capture");
        for (const ReflectionProbeCaptureScheduleEntry& capture : self.stagedCaptures_) {
            if (capture.scheduledFaceMask == 0u) continue;
            uint32_t excludedInstanceIndex = InvalidGpuSceneIndex;
            uint32_t gpuOwnerPrimitiveCount = 0u;
            if (indirectValid) {
                const VulkanGpuSceneState::CpuMirror& scene =
                    feature.gpuScene.mirror(frameIndex);
                for (uint32_t primitiveIndex : probeCasters.gpuScenePrimitiveIndices) {
                    if (primitiveIndex >= scene.primitives.size() ||
                        primitiveIndex >= scene.primitiveIdentities.size() ||
                        scene.primitiveIdentities[primitiveIndex].owner != capture.owner)
                        continue;
                    const uint32_t instanceIndex =
                        scene.primitives[primitiveIndex].binding.x;
                    if (excludedInstanceIndex != InvalidGpuSceneIndex &&
                        excludedInstanceIndex != instanceIndex)
                        throw std::logic_error(
                            "Reflection-probe owner spans multiple GPU-scene instances");
                    excludedInstanceIndex = instanceIndex;
                    ++gpuOwnerPrimitiveCount;
                }
                if (!probeQualificationOracle)
                    ownerFaceExclusions +=
                        static_cast<uint64_t>(gpuOwnerPrimitiveCount) *
                        std::popcount(static_cast<uint32_t>(capture.scheduledFaceMask));
            }
            const VulkanReflectionProbeCaptureStaging& target =
                self.captureTargets_.acquire(capture.owner,
                    capture.captureTicket, capture.resolution);
            for (uint32_t face = 0; face < kReflectionProbeCaptureFaceCount; ++face) {
                const uint8_t bit = static_cast<uint8_t>(1u << face);
                if ((capture.scheduledFaceMask & bit) == 0u) continue;
                if (faceRecord >= VulkanReflectionProbeCapturePass::MaximumFaceRecords)
                    throw std::overflow_error(
                        "Reflection-probe capture face records are exhausted");
                capturePass.writeFace(frameIndex, faceRecord,
                    capture.faces[face], capture.position,
                    capture.nearPlane, lights.stats.activeLightCount,
                    capture.captureSky, capture.resolution);
                if (indirectValid)
                    counters.dispatchRecorded += self.culler_.recordWorkItem(
                        cmd, frameIndex, {
                            .set0 = capturePass.faceComputeDescriptor(frameIndex),
                            .set0DynamicOffset =
                                capturePass.faceComputeDynamicOffset(faceRecord),
                            .gpuScene = feature.gpuScene.descriptorSets()[frameIndex],
                        }, { excludedInstanceIndex, faceRecord, 0u });
                scratch.visibility.assign(scratch.casters.size(), 0u);
                const size_t cpuVisibilityBegin = indirectValid &&
                        !probeQualificationOracle
                    ? static_cast<size_t>(resolvedGpuSceneCasters) : 0u;
                for (size_t casterIndex = cpuVisibilityBegin;
                        casterIndex < scratch.casters.size(); ++casterIndex) {
                    const VulkanResolvedCaster& caster = scratch.casters[casterIndex];
                    if (caster.owner == capture.owner) {
                        ++ownerFaceExclusions;
                        continue;
                    }
                    ++casterFaceTests;
                    const bool visible = shadowCasterSphereIntersectsClipVolume(
                        capture.faces[face].worldToClip,
                        caster.boundsSphereCenterWorld,
                        caster.boundsSphereRadiusWorld);
                    scratch.visibility[casterIndex] = visible ? 1u : 0u;
                    casterFacesCulled += visible ? 0u : 1u;
                }
                capturePass.beginFace(cmd, target, face, frameIndex, faceRecord, sceneSet);
                capturePass.bindFaceDescriptors(cmd, frameIndex, faceRecord, sceneSet);
                // Both capture layouts have an identical set 0-3 prefix. Bind
                // the shared tables through the longer layout once so compact
                // GPU-scene draws and explicit fallback packets can interleave.
                feature.resources.bindMaterialDescriptors(cmd, frameIndex, gpuSceneLayout);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    gpuSceneLayout, 4u, 1u,
                    &feature.gpuScene.descriptorSets()[frameIndex], 0u, nullptr);
                VkPipeline activePipeline = VK_NULL_HANDLE;
                GeometryHandle activeGeometry{};
                if (indirectValid) {
                    (void)self.culler_.recordDraws(cmd, frameIndex, faceRecord, {
                        .owner = &capturePass,
                        .pipeline = [](const void* owner, bool alphaMasked,
                            bool doubleSided) {
                            return static_cast<
                                const VulkanReflectionProbeCapturePass*>(owner)->
                                pipeline(alphaMasked, doubleSided, true);
                        },
                    });
                    if (probeQualificationOracle) {
                        const RadialLodContext lodContext{ capture.position,
                            static_cast<float>(capture.resolution),
                            self.settings_.lodErrorThreshold };
                        self.culler_.emitExpectations(*shadowOracle, frameIndex,
                            faceRecord,
                            std::span(scratch.casters).first(
                                static_cast<size_t>(resolvedGpuSceneCasters)),
                            std::span(scratch.visibility).first(
                                static_cast<size_t>(resolvedGpuSceneCasters)),
                            1u, radialLodMetric(lodContext));
                        for (size_t casterIndex = 0;
                                casterIndex < resolvedGpuSceneCasters; ++casterIndex) {
                            if (scratch.visibility[casterIndex] == 0u) continue;
                            ++gpuSceneFaceDraws;
                            ++casterFaceDraws;
                        }
                    }
                    activePipeline = VK_NULL_HANDLE;
                    activeGeometry = {};
                }
                const size_t directDrawBegin = indirectValid
                    ? static_cast<size_t>(resolvedGpuSceneCasters) : 0u;
                for (size_t casterIndex = directDrawBegin;
                        casterIndex < scratch.casters.size(); ++casterIndex) {
                    if (scratch.visibility[casterIndex] == 0u) continue;
                    const VulkanResolvedCaster& caster = scratch.casters[casterIndex];
                    VulkanGeometryPayload* geometry =
                        feature.resources.geometries().get(caster.geometry);
                    VulkanMaterialPayload* material =
                        feature.resources.materials().get(caster.material);
                    if (geometry == nullptr || material == nullptr) continue;
                    const bool gpuScene = caster.gpuScenePrimitiveIndex !=
                        InvalidGpuSceneIndex;
                    const VkPipeline pipeline = capturePass.pipeline(
                        material->packed.alphaMode == 1u,
                        material->packed.doubleSided != 0u, gpuScene);
                    if (pipeline != activePipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                        activePipeline = pipeline;
                    }
                    if (caster.geometry != activeGeometry) {
                        const VkDeviceSize offset = geometry->vertexOffset;
                        vkCmdBindVertexBuffers(cmd, 0, 1,
                            &geometry->vertexBuffer.buffer, &offset);
                        vkCmdBindIndexBuffer(cmd, geometry->indexBuffer.buffer, 0,
                            toVkIndexType(geometry->indexFormat));
                        activeGeometry = caster.geometry;
                    }
                    if (gpuScene) {
                        vkCmdDrawIndexed(cmd, caster.indexCount, 1,
                            caster.firstIndex, 0, caster.gpuScenePrimitiveIndex);
                        ++gpuSceneFaceDraws;
                    }
                    else {
                        CanonicalMeshPushConstants push{};
                        push.renderMatrix = caster.worldTransform;
                        push.materialIndex = caster.material.getIndex();
                        vkCmdPushConstants(cmd, layout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(push), &push);
                        vkCmdDrawIndexed(cmd, caster.indexCount, 1,
                            caster.firstIndex, 0, 0);
                        ++directFaceDraws;
                    }
                    ++casterFaceDraws;
                }
                capturePass.endFace(cmd, target, face);
                ++faceRecord;
                ++captureTelemetry.facesRendered;
                captureTelemetry.renderedTexels +=
                    static_cast<uint64_t>(capture.resolution) * capture.resolution;
            }
            const uint8_t completedMask = static_cast<uint8_t>(
                capture.capturedFaceMask | capture.scheduledFaceMask);
            if (completedMask == kReflectionProbeCaptureCompleteMask) {
                const auto duplicate = std::ranges::find_if(self.pendingCaptures_,
                    [&](const PendingCapture& pending) {
                        return pending.owner == capture.owner;
                    });
                if (duplicate != self.pendingCaptures_.end())
                    throw std::logic_error(
                        "Reflection-probe capture publication is duplicated");
                PendingCapture pending{
                    .owner = capture.owner,
                    .captureTicket = capture.captureTicket,
                    .filterDescriptors = capturePass.recordPrefilter(
                        cmd, target, self.prefilterSampleCount_),
                    .resolution = target.resolution,
                    .mipLevels = target.mipLevels,
                };
                if (capture.updateMode == ReflectionProbeUpdateMode::Baked)
                    pending.bakedReadback = capturePass.recordReadback(cmd, target);
                pending.recordSerial = feature.scheduler.retireValue();
                self.pendingCaptures_.push_back(std::move(pending));
                ++captureTelemetry.capturesFiltered;
            }
        }
        feature.scheduler.endGpuRange(captureRange);
        captureTelemetry.capturesInFlight = self.captureTargets_.capturesInFlight();
        captureTelemetry.stagingLogicalBytes = self.captureTargets_.stagingLogicalBytes();
        captureTelemetry.publishedLogicalBytes =
            self.captureTargets_.publishedLogicalBytes();
        if (CpuProfiler* profiler = feature.profiler) {
            profiler->recordCounter("probe.capture.casters.resolved_gpu_scene",
                resolvedGpuSceneCasters);
            profiler->recordCounter("probe.capture.casters.resolved_direct",
                resolvedDirectCasters);
            profiler->recordCounter("probe.capture.casters.invalid_gpu_scene",
                probeCasters.gpuScenePrimitiveIndices.size() - resolvedGpuSceneCasters);
            const ProfileCounterStatus cpuVisibilityStatus = indirectValid &&
                    !probeQualificationOracle
                ? ProfileCounterStatus::Unavailable
                : ProfileCounterStatus::Exact;
            profiler->recordCounter("probe.capture.caster_face_tests",
                casterFaceTests, cpuVisibilityStatus);
            profiler->recordCounter("probe.capture.caster_faces_culled",
                casterFacesCulled, cpuVisibilityStatus);
            profiler->recordCounter("probe.capture.caster_face_draws",
                casterFaceDraws, cpuVisibilityStatus);
            profiler->recordCounter("probe.capture.gpu_scene_face_draws",
                gpuSceneFaceDraws, cpuVisibilityStatus);
            profiler->recordCounter("probe.capture.direct_face_draws", directFaceDraws);
            profiler->recordCounter("probe.capture.owner_face_exclusions",
                ownerFaceExclusions);
            profiler->recordCounter("probe.capture.indirect.enabled",
                indirectValid ? 1u : 0u);
            profiler->recordCounter("probe.capture.indirect.bins",
                indirectValid ? self.culler_.bins().size() : 0u);
            profiler->recordCounter("probe.capture.indirect.fallback_reason",
                static_cast<uint32_t>(self.culler_.fallbackReason()));
        }
    }

} // namespace Iridium
