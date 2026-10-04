#include "VulkanResourceRegistry.h"

#include "VulkanPipelineLibrary.h"
#include "VulkanUploadContext.h"
#include "VulkanVertexUtils.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "profiling/CpuProfiler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium {

    void VulkanResourceRegistry::init(const Services& services) {
        device_ = services.device;
        allocator_ = services.allocator;
        uploads_ = services.uploads;
        scheduler_ = services.scheduler;
        pipelines_ = services.pipelines;
        profiler_ = services.profiler;
        frameOpen_ = services.frameOpen;
    }

    void VulkanResourceRegistry::bindMaterialDescriptors(
        VkCommandBuffer commandBuffer, uint32_t frameIndex,
        VkPipelineLayout layout) const {
        const auto sets = indexedTextureTable_.descriptorSets(frameIndex);
        if (sets[0] == VK_NULL_HANDLE || sets[1] == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "Indexed material descriptor sets are unavailable");
        }
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            layout, 1, static_cast<uint32_t>(sets.size()),
            sets.data(), 0, nullptr);
    }

    GeometryHandle VulkanResourceRegistry::allocateGeometry(const GeometryDesc& desc,
        std::span<const std::byte> vertexBytes, std::span<const std::byte> indexBytes) {
        const uint32_t indexSize = indexElementSize(desc.indexFormat);
        if (desc.vertexStride == 0 || indexSize == 0) {
            throw std::invalid_argument("Geometry format must define nonzero element sizes.");
        }
        if (indexBytes.size_bytes() % indexSize != 0) {
            throw std::invalid_argument("Geometry index data is not aligned to its index format.");
        }

        VulkanGeometryPayload payload{};
        payload.indexCount = static_cast<uint32_t>(indexBytes.size_bytes() / indexSize);
        payload.indexFormat = desc.indexFormat;

        payload.vertexBuffer = allocator_->createBuffer(vertexBytes.size_bytes(),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
            ProfileMemoryCategory::GeometryVertex);
        try {
            payload.indexBuffer = allocator_->createBuffer(indexBytes.size_bytes(),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::GeometryIndex);

            uploads_->enqueueBufferUpload(payload.vertexBuffer, vertexBytes,
                ResourceState::VertexBuffer);
            uploads_->enqueueBufferUpload(payload.indexBuffer, indexBytes,
                ResourceState::IndexBuffer);
        } catch (...) {
            allocator_->destroy(payload.indexBuffer);
            allocator_->destroy(payload.vertexBuffer);
            throw;
        }

        return geometryVault_.allocate(payload);
    }

    void VulkanResourceRegistry::freeGeometry(GeometryHandle handle) {
        auto* payload = geometryVault_.get(handle);
        if (payload) {
            if (payload->arenaAllocation) {
                throw std::logic_error(
                    "Geometry-arena primitive handles must be retired together");
            }
            // R4c.1: retired until every frame that can reference them has
            // completed (no stall).
            scheduler_->retire(payload->vertexBuffer);
            scheduler_->retire(payload->indexBuffer);

            geometryVault_.free(handle);
        }
    }

    GeometryArenaAllocation VulkanResourceRegistry::allocateGeometryArena(
        uint32_t vertexStride,
        std::span<const std::byte> vertexBytes,
        const GeometryArenaData& arena) {
        if (arena.abiVersion != GeometryArenaAbiVersion || vertexStride == 0 ||
            arena.primitives.empty() ||
            arena.vertexCount != vertexBytes.size_bytes() / vertexStride ||
            arena.vertexCount * vertexStride != vertexBytes.size_bytes()) {
            throw std::invalid_argument(
                "Geometry arena has an unsupported ABI or vertex layout");
        }
        for (const GeometryArenaPrimitiveRange& range : arena.primitives) {
            const uint64_t streamSize = range.indexStream ==
                    GeometryArenaIndexStream::UInt16
                ? arena.uint16Indices.size() : arena.uint32Indices.size();
            if (range.indexCount == 0 || range.firstIndex > streamSize ||
                range.indexCount > streamSize - range.firstIndex ||
                range.vertexOffset < 0 ||
                static_cast<uint64_t>(range.vertexOffset) >= arena.vertexCount) {
                throw std::invalid_argument(
                    "Geometry arena primitive range is out of bounds");
            }
        }

        VulkanBufferResource vertexBuffer;
        VulkanBufferResource uint16Buffer;
        VulkanBufferResource uint32Buffer;
        GeometryArenaAllocation allocation;
        allocation.primitiveGeometry.reserve(arena.primitives.size());
        try {
            vertexBuffer = allocator_->createBuffer(vertexBytes.size_bytes(),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                ProfileMemoryCategory::GeometryVertex);
            if (!arena.uint16Indices.empty()) {
                uint16Buffer = allocator_->createBuffer(
                    arena.uint16Indices.size() * sizeof(uint16_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                    ProfileMemoryCategory::GeometryIndex);
            }
            if (!arena.uint32Indices.empty()) {
                uint32Buffer = allocator_->createBuffer(
                    arena.uint32Indices.size() * sizeof(uint32_t),
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                    ProfileMemoryCategory::GeometryIndex);
            }

            for (size_t index = 0; index < arena.primitives.size(); ++index) {
                const GeometryArenaPrimitiveRange& range =
                    arena.primitives[index];
                VulkanGeometryPayload payload{
                    .vertexBuffer = vertexBuffer,
                    .indexBuffer = range.indexStream ==
                            GeometryArenaIndexStream::UInt16
                        ? uint16Buffer : uint32Buffer,
                    .arenaUInt16IndexBuffer = uint16Buffer,
                    .arenaUInt32IndexBuffer = uint32Buffer,
                    .vertexOffset = static_cast<VkDeviceSize>(
                        range.vertexOffset) * vertexStride,
                    .indexCount = range.indexCount,
                    .indexFormat = range.indexStream ==
                            GeometryArenaIndexStream::UInt16
                        ? IndexFormat::UInt16 : IndexFormat::UInt32,
                    .arenaAllocation = true,
                    .ownsArenaBuffers = index == 0,
                };
                allocation.primitiveGeometry.push_back(
                    geometryVault_.allocate(std::move(payload)));
            }

            uploads_->enqueueBufferUpload(vertexBuffer, vertexBytes,
                ResourceState::VertexBuffer);
            if (!arena.uint16Indices.empty()) {
                uploads_->enqueueBufferUpload(uint16Buffer,
                    std::as_bytes(std::span(arena.uint16Indices)),
                    ResourceState::IndexBuffer);
            }
            if (!arena.uint32Indices.empty()) {
                uploads_->enqueueBufferUpload(uint32Buffer,
                    std::as_bytes(std::span(arena.uint32Indices)),
                    ResourceState::IndexBuffer);
            }
        }
        catch (...) {
            for (GeometryHandle handle : allocation.primitiveGeometry)
                geometryVault_.free(handle);
            allocator_->destroy(uint32Buffer);
            allocator_->destroy(uint16Buffer);
            allocator_->destroy(vertexBuffer);
            throw;
        }
        return allocation;
    }

    void VulkanResourceRegistry::freeGeometryArena(
        std::span<const GeometryHandle> primitiveGeometry) {
        if (primitiveGeometry.empty()) return;
        VulkanGeometryPayload* first = geometryVault_.get(
            primitiveGeometry.front());
        if (!first || !first->arenaAllocation) {
            throw std::invalid_argument(
                "Geometry arena retirement requires live arena handles");
        }
        for (GeometryHandle handle : primitiveGeometry) {
            VulkanGeometryPayload* payload = geometryVault_.get(handle);
            if (!payload || !payload->arenaAllocation ||
                payload->vertexBuffer.buffer != first->vertexBuffer.buffer ||
                payload->arenaUInt16IndexBuffer.buffer !=
                    first->arenaUInt16IndexBuffer.buffer ||
                payload->arenaUInt32IndexBuffer.buffer !=
                    first->arenaUInt32IndexBuffer.buffer) {
                throw std::invalid_argument(
                    "Geometry arena retirement cannot mix allocations");
            }
        }
        scheduler_->retire(first->vertexBuffer);
        scheduler_->retire(first->arenaUInt16IndexBuffer);
        scheduler_->retire(first->arenaUInt32IndexBuffer);
        for (GeometryHandle handle : primitiveGeometry)
            geometryVault_.free(handle);
    }

    TextureHandle VulkanResourceRegistry::allocateTexture(const TextureDesc& desc,
        std::span<const std::byte> pixelBytes) {
        if (!validTextureTopology(desc)) {
            throw std::invalid_argument(
                "Texture dimensions, layers, mips, or topology are invalid");
        }
        if (pixelBytes.empty()) {
            throw std::invalid_argument("Texture pixel data must be nonempty");
        }

        const size_t expectedBytes = static_cast<size_t>(textureDataSize(desc));
        if (bytesPerBlock(desc.format) == 0 || pixelBytes.size() != expectedBytes) {
            throw std::invalid_argument("Texture pixel data size does not match the descriptor");
        }

        VulkanTexturePayload payload{};
        payload.format = desc.format;
        payload.width = desc.width;
        payload.height = desc.height;

        VkFormat format = VK_FORMAT_UNDEFINED;
        switch (desc.format) {
        case TextureFormat::RGBA8_UNorm:
            format = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case TextureFormat::RGBA8_sRGB:
            format = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        case TextureFormat::RGBA16_SFloat:
            format = VK_FORMAT_R16G16B16A16_SFLOAT;
            break;
        case TextureFormat::RGBA32_SFloat:
            format = VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        case TextureFormat::RG16_SFloat:
            format = VK_FORMAT_R16G16_SFLOAT;
            break;
        case TextureFormat::BC4_UNorm:
            format = VK_FORMAT_BC4_UNORM_BLOCK;
            break;
        case TextureFormat::BC5_UNorm:
            format = VK_FORMAT_BC5_UNORM_BLOCK;
            break;
        case TextureFormat::BC6H_UFloat:
            format = VK_FORMAT_BC6H_UFLOAT_BLOCK;
            break;
        case TextureFormat::BC7_UNorm:
            format = VK_FORMAT_BC7_UNORM_BLOCK;
            break;
        case TextureFormat::BC7_sRGB:
            format = VK_FORMAT_BC7_SRGB_BLOCK;
            break;
        }

        const bool cube = desc.topology == TextureTopology::Cube;
        const VkImageViewType viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE :
            (desc.topology == TextureTopology::Texture2DArray
                ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        payload.image = allocator_->createImage2D({ desc.width, desc.height }, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, desc.usageClass == TextureUsageClass::Environment
                ? ProfileMemoryCategory::Environment
                : ProfileMemoryCategory::Texture, desc.mipLevels,
            desc.arrayLayers,
            cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, viewType);

        try {
            payload.samplerCacheIndex = acquireSampler(desc.sampler);
            payload.sampler =
                samplerCache_[payload.samplerCacheIndex].sampler;
        } catch (...) {
            allocator_->destroy(payload.image);
            throw;
        }

        try {
            CpuScope uploadScheduleScope(
                profiler_, "cpu.texture.upload_schedule");
            uploads_->enqueueImageUpload(
                payload.image, pixelBytes, ResourceState::ShaderResource);
        } catch (...) {
            releaseSampler(payload.samplerCacheIndex);
            allocator_->destroy(payload.image);
            throw;
        }

        const TextureHandle handle = textureVault_.allocate(payload);
        if (indexedTextureTable_.active()) {
            if (handle.getIndex() >= indexedTextureTable_.maximumCapacity()) {
                uploads_->flush();
                textureVault_.free(handle);
                releaseSampler(payload.samplerCacheIndex);
                allocator_->destroy(payload.image);
                throw std::runtime_error(
                    "Indexed material texture table capacity was exhausted");
            }
            const uint32_t frameIndex = scheduler_->currentFrameIndex();
            if (handle.getIndex() >=
                    indexedTextureTable_.frameCapacity(frameIndex) &&
                frameOpen()) {
                uploads_->flush();
                textureVault_.free(handle);
                releaseSampler(payload.samplerCacheIndex);
                allocator_->destroy(payload.image);
                throw std::runtime_error(
                    "Indexed material texture-table growth must occur at a "
                    "frame boundary");
            }
            // Between frames the current slot's last submission may still be
            // executing (its wait is in the next beginFrame): growing would
            // destroy a descriptor pool in use. The write below raises the
            // logical capacity, and beginFrame grows the slot after its wait,
            // as it does for the other slot.
            if (!scheduler_->slotInFlight(frameIndex))
                indexedTextureTable_.ensureFrameCapacity(
                    frameIndex, handle.getIndex() + 1);
            indexedTextureTable_.write(handle.getIndex(), payload.image.view,
                handle.getIndex(), payload.sampler);
            // Pre-frame publication can batch descriptor synchronization into
            // beginFrame. Mid-frame publication must make the current
            // fence-owned set visible before draw submission.
            if (frameOpen()) {
                indexedTextureTable_.synchronizeFrame(
                    frameIndex);
            }
        }
        return handle;
    }



    void VulkanResourceRegistry::freeTexture(TextureHandle handle) {
        auto* payload = textureVault_.get(handle);
        if (payload && !payload->retired) {
            indexedTextureTable_.writeFallback(
                handle.getIndex(), handle.getIndex());
            if (indexedTextureTable_.active() &&
                frameOpen()) {
                indexedTextureTable_.synchronizeFrame(
                    scheduler_->currentFrameIndex());
            }
            const uint32_t samplerCacheIndex = payload->samplerCacheIndex;
            VkDescriptorSet editorDescriptor = payload->editorDescriptor;
            VulkanImageResource image = payload->image;
            payload->retired = true;
            ++retiredTextureCount_;

            // R4c.1: once the texture's last frame has completed, the editor
            // descriptor and the vault slot are released and the image is
            // destroyed.
            scheduler_->retireCallback({ &VulkanResourceRegistry::releaseRetiredTexture,
                this, { handle.id,
                    reinterpret_cast<uint64_t>(editorDescriptor), 0, 0 } });
            scheduler_->retire(image);

            releaseSampler(samplerCacheIndex);
        }
    }

    void VulkanResourceRegistry::releaseRetiredTexture(void* user,
        const VulkanDeletionArguments& arguments) {
        auto& self = *static_cast<VulkanResourceRegistry*>(user);
        const TextureHandle handle{ static_cast<uint32_t>(arguments[0]) };
        const auto editorDescriptor =
            reinterpret_cast<VkDescriptorSet>(arguments[1]);
        if (editorDescriptor != VK_NULL_HANDLE && self.editorRelease_ != nullptr)
            self.editorRelease_(self.editorReleaseOwner_, editorDescriptor);
        self.textureVault_.free(handle);
        if (self.retiredTextureCount_ != 0) --self.retiredTextureCount_;
    }

    MaterialBinding VulkanResourceRegistry::allocateCanonicalMaterial(
        const CanonicalMaterialAsset& asset) {
        const bool deferred = asset.packed.closureClass ==
                static_cast<uint32_t>(MaterialClosureClass::StandardDeferred) &&
            asset.pipelineState.shaderProgram == ShaderProgram::CanonicalPbrGBuffer &&
            asset.pipelineState.renderPass == RenderPassClass::GBuffer;
        const bool complexProgram =
            asset.pipelineState.shaderProgram ==
                ShaderProgram::CanonicalComplexOpaqueForward ||
            asset.pipelineState.shaderProgram ==
                ShaderProgram::CanonicalComplexForward;
        const bool forward = asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::StandardDeferred) &&
            asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::Invalid) &&
            complexProgram &&
            asset.pipelineState.renderPass == RenderPassClass::Forward;
        const bool transparent = asset.packed.closureClass !=
                static_cast<uint32_t>(MaterialClosureClass::Invalid) &&
            complexProgram &&
            asset.pipelineState.renderPass == RenderPassClass::Transparent;
        if (asset.packed.schemaVersion != PackedGpuMaterial::SchemaVersion ||
            (!deferred && !forward && !transparent))
            throw std::invalid_argument("canonical material asset has an incompatible contract");

        std::array<VulkanTexturePayload*, PackedGpuMaterial::MaxTextureUses> textures{};
        for (size_t index = 0; index < textures.size(); ++index) {
            textures[index] = textureVault_.get(asset.textures[index]);
            if (!textures[index] || textures[index]->retired)
                throw std::invalid_argument("canonical material has an invalid texture handle");
        }

        VulkanMaterialPayload materialPayload{};
        materialPayload.pipeline = pipelines_->getOrCreatePipeline(asset.pipelineState);
        PipelineStateDesc mirroredPipelineState = asset.pipelineState;
        if (mirroredPipelineState.renderPass ==
                RenderPassClass::Transparent &&
            mirroredPipelineState.cullMode != CullMode::None) {
            mirroredPipelineState.frontFace =
                mirroredPipelineState.frontFace == FrontFace::Clockwise
                ? FrontFace::CounterClockwise : FrontFace::Clockwise;
            materialPayload.mirroredPipeline =
                pipelines_->getOrCreatePipeline(mirroredPipelineState);
        }
        else {
            materialPayload.mirroredPipeline = materialPayload.pipeline;
        }
        materialPayload.renderQueue = forward || transparent
            ? (asset.pipelineState.blendMode == BlendMode::Opaque
                ? RenderQueue::ForwardOpaque : RenderQueue::Transparent)
            : RenderQueue::Opaque;
        materialPayload.packed = asset.packed;
        materialPayload.packedRevision = 1;

        const MaterialHandle material = materialVault_.allocate(materialPayload);
        try {
            ensureCanonicalMaterialCapacity(
                material.getIndex() + 1u);
            for (uint32_t frame = 0; frame <
                VulkanFrameScheduler::FramesInFlight; ++frame) {
                const auto sets = indexedTextureTable_.descriptorSets(frame);
                if (sets[0] == VK_NULL_HANDLE ||
                    sets[1] == VK_NULL_HANDLE) {
                    throw std::runtime_error(
                        "Indexed material descriptor sets are unavailable");
                }
            }
        }
        catch (...) {
            materialVault_.free(material);
            throw;
        }
        ++materialRevision_;
        VulkanMaterialPayload* stored =
            materialVault_.get(material);
        return { material, stored->pipeline, stored->renderQueue,
            makeOpaqueSortKey(stored->pipeline, material) };
    }

    void VulkanResourceRegistry::updateCanonicalMaterial(MaterialHandle handle,
        const PackedGpuMaterial& material) {
        VulkanMaterialPayload* payload = materialVault_.get(handle);
        if (!payload)
            throw std::invalid_argument("canonical material update handle is invalid");
        if (material.schemaVersion != PackedGpuMaterial::SchemaVersion ||
            material.closureClass != payload->packed.closureClass)
            throw std::invalid_argument("canonical material update changes its schema or closure");
        payload->packed = material;
        ++payload->packedRevision;
        if (payload->packedRevision == 0) payload->packedRevision = 1;
        ++materialRevision_;
    }

    void VulkanResourceRegistry::freeMaterial(MaterialHandle handle) {
        auto* payload = materialVault_.get(handle);
        if (!payload) {
            return;
        }

        materialVault_.free(handle);
        ++materialRevision_;
    }

    void VulkanResourceRegistry::createCanonicalMaterialBuffers(
        uint32_t capacity) {
        if (capacity == 0 ||
            capacity > canonicalMaterialMaximumCapacity_) {
            throw std::invalid_argument(
                "canonical material buffer capacity is outside the device limit");
        }
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(capacity) *
            sizeof(PackedGpuMaterial);
        std::array<VulkanBufferResource,
            VulkanFrameScheduler::FramesInFlight>
            replacement{};
        try {
            for (VulkanBufferResource& buffer :
                replacement) {
                buffer = allocator_->createBuffer(bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    true,
                    ProfileMemoryCategory::MaterialGpu);
            }
        }
        catch (...) {
            for (VulkanBufferResource& buffer :
                replacement) {
                allocator_->destroy(buffer);
            }
            throw;
        }

        if (canonicalMaterialCapacity_ != 0 && frameOpen()) {
            for (VulkanBufferResource& buffer :
                replacement) {
                allocator_->destroy(buffer);
            }
            throw std::logic_error(
                "canonical material buffers may grow only at a frame boundary");
        }
        // R4c.2: no drain. A slot that is not in flight swaps now; an
        // in-flight slot parks its replacement until its retirement (its
        // next upload follows the swap).
        for (uint32_t frame = 0;
            frame < VulkanFrameScheduler::FramesInFlight;
            ++frame) {
            allocator_->destroy(pendingMaterialBuffers_[frame]);
            pendingMaterialSlots_[frame] = scheduler_->slotInFlight(frame);
            if (pendingMaterialSlots_[frame]) {
                pendingMaterialBuffers_[frame] = replacement[frame];
                continue;
            }
            allocator_->destroy(canonicalMaterialBuffers_[frame]);
            canonicalMaterialBuffers_[frame] = replacement[frame];
            indexedTextureTable_.bindMaterialBuffer(
                frame,
                canonicalMaterialBuffers_[frame].buffer,
                canonicalMaterialBuffers_[frame].size);
        }
        canonicalMaterialCapacity_ = capacity;
        materialVault_.forEach(
            [](VulkanMaterialPayload& material) {
                material.uploadedPackedRevisions.fill(0);
            });
    }

    bool VulkanResourceRegistry::swapRetiredSlot(uint32_t slot) {
        if (!pendingMaterialSlots_[slot]) return false;
        allocator_->destroy(canonicalMaterialBuffers_[slot]);
        canonicalMaterialBuffers_[slot] = pendingMaterialBuffers_[slot];
        pendingMaterialBuffers_[slot] = {};
        pendingMaterialSlots_[slot] = false;
        indexedTextureTable_.bindMaterialBuffer(slot,
            canonicalMaterialBuffers_[slot].buffer,
            canonicalMaterialBuffers_[slot].size);
        return true;
    }

    void VulkanResourceRegistry::ensureCanonicalMaterialCapacity(
        uint32_t requiredCapacity) {
        if (requiredCapacity <=
            canonicalMaterialCapacity_) {
            return;
        }
        if (requiredCapacity >
            canonicalMaterialMaximumCapacity_) {
            throw std::overflow_error(
                "canonical material table exhausted the device storage-buffer limit");
        }
        createCanonicalMaterialBuffers(
            nextMaterialTableCapacity(
                canonicalMaterialCapacity_,
                requiredCapacity,
                canonicalMaterialMaximumCapacity_));
    }

    void VulkanResourceRegistry::uploadCanonicalMaterialsForFrame(uint32_t frameIndex) {
        if (frameIndex >= canonicalMaterialBuffers_.size())
            throw std::out_of_range("canonical material frame index is invalid");
        VulkanBufferResource& buffer = canonicalMaterialBuffers_[frameIndex];
        materialVault_.forEachIndexed([&](MaterialHandle handle,
            VulkanMaterialPayload& material) {
            if (handle.getIndex() >=
                canonicalMaterialCapacity_)
                throw std::overflow_error("canonical material table capacity exceeded");
            uint64_t& uploadedRevision =
                material.uploadedPackedRevisions[frameIndex];
            uint64_t nextUploadedRevision = uploadedRevision;
            if (!consumeMaterialUploadRevision(material.packedRevision,
                nextUploadedRevision)) return;
            allocator_->write(buffer,
                static_cast<VkDeviceSize>(handle.getIndex()) * sizeof(PackedGpuMaterial),
                std::as_bytes(std::span(&material.packed, size_t{ 1 })));
            uploadedRevision = nextUploadedRevision;
        });
    }

    uint32_t VulkanResourceRegistry::acquireSampler(const SamplerDesc& desc) {
        for (uint32_t index = 0; index < samplerCache_.size(); ++index) {
            CachedSampler& cached = samplerCache_[index];
            if (cached.desc == desc) {
                ++cached.referenceCount;
                return index;
            }
        }

        const auto toVkFilter = [](FilterMode mode) {
            return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        const auto toVkAddressMode = [](SamplerAddressMode mode) {
            switch (mode) {
            case SamplerAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerAddressMode::MirroredRepeat:
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SamplerAddressMode::ClampToEdge:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        const auto toVkCompareOp = [](SamplerCompareOp operation) {
            switch (operation) {
            case SamplerCompareOp::Never: return VK_COMPARE_OP_NEVER;
            case SamplerCompareOp::Less: return VK_COMPARE_OP_LESS;
            case SamplerCompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case SamplerCompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case SamplerCompareOp::GreaterOrEqual:
                return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case SamplerCompareOp::Always: return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_NEVER;
        };

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = toVkFilter(desc.magFilter);
        samplerInfo.minFilter = toVkFilter(desc.minFilter);
        samplerInfo.mipmapMode = desc.mipmapFilter == MipmapFilterMode::Nearest
            ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = toVkAddressMode(desc.addressU);
        samplerInfo.addressModeV = toVkAddressMode(desc.addressV);
        samplerInfo.addressModeW = toVkAddressMode(desc.addressW);
        samplerInfo.minLod = static_cast<float>(desc.minLod);
        samplerInfo.maxLod = static_cast<float>(desc.maxLod);
        samplerInfo.anisotropyEnable = desc.maxAnisotropy > 1
            ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy =
            static_cast<float>(std::max<uint8_t>(1, desc.maxAnisotropy));
        samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
        samplerInfo.compareOp = toVkCompareOp(desc.compareOp);

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(device_, &samplerInfo, nullptr,
                &sampler) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create texture sampler.");
        }
        samplerCache_.push_back(CachedSampler{
            .desc = desc,
            .sampler = sampler,
            .referenceCount = 1,
        });
        return static_cast<uint32_t>(samplerCache_.size() - 1);
    }

    void VulkanResourceRegistry::releaseSampler(uint32_t cacheIndex) noexcept {
        if (cacheIndex >= samplerCache_.size()) {
            return;
        }
        CachedSampler& cached = samplerCache_[cacheIndex];
        if (cached.referenceCount > 0) {
            --cached.referenceCount;
        }
    }

    void VulkanResourceRegistry::cleanupSamplerCache() noexcept {
        if (device_ == VK_NULL_HANDLE) {
            samplerCache_.clear();
            return;
        }
        for (CachedSampler& cached : samplerCache_) {
            if (cached.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device_, cached.sampler, nullptr);
                cached.sampler = VK_NULL_HANDLE;
            }
        }
        samplerCache_.clear();
    }

    uint64_t VulkanResourceRegistry::liveSamplerCount() const noexcept {
        return static_cast<uint64_t>(std::count_if(
            samplerCache_.begin(), samplerCache_.end(),
            [](const CachedSampler& cached) {
                return cached.referenceCount > 0;
            }));
    }

    void VulkanResourceRegistry::destroyResources() noexcept {
        geometryVault_.forEach([this](VulkanGeometryPayload& payload) {
            if (payload.arenaAllocation) {
                if (!payload.ownsArenaBuffers) return;
                allocator_->destroy(payload.vertexBuffer);
                allocator_->destroy(payload.arenaUInt16IndexBuffer);
                allocator_->destroy(payload.arenaUInt32IndexBuffer);
            }
            else {
                allocator_->destroy(payload.vertexBuffer);
                allocator_->destroy(payload.indexBuffer);
            }
            });

        textureVault_.forEach([this](VulkanTexturePayload& payload) {
            if (!payload.retired) allocator_->destroy(payload.image);
            });
        cleanupSamplerCache();
        for (VulkanBufferResource& buffer : canonicalMaterialBuffers_)
            allocator_->destroy(buffer);
        for (VulkanBufferResource& buffer : pendingMaterialBuffers_)
            allocator_->destroy(buffer);
        pendingMaterialSlots_ = {};
    }

    void VulkanResourceRegistry::reset() noexcept {
        canonicalMaterialCapacity_ = 0;
        canonicalMaterialMaximumCapacity_ = 0;
        retiredTextureCount_ = 0;
    }

} // namespace Iridium
