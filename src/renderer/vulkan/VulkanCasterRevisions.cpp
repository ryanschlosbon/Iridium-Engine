#include "VulkanCasterRevisions.h"

#include "VulkanResourceRegistry.h"
#include "VulkanShadowCasters.h"
#include "renderer/lighting/DirectionalShadow.h"

#include <cstring>

namespace Iridium {

    namespace {
        constexpr uint32_t AllCascades =
            (1u << kDirectionalShadowCascadeCount) - 1u;

        bool registryMaterial(const void* owner, MaterialHandle handle,
            uint64_t& packedRevision, uint32_t& alphaMode,
            uint32_t& doubleSided) {
            const VulkanMaterialPayload* material =
                static_cast<const VulkanResourceRegistry*>(owner)->
                    materials().get(handle);
            if (material == nullptr) return false;
            packedRevision = material->packedRevision;
            alphaMode = material->packed.alphaMode;
            doubleSided = material->packed.doubleSided;
            return true;
        }

        void setMaterial(VulkanCasterContent& content, MaterialHandle handle,
            const VulkanCasterMaterialSource& materials) noexcept {
            content.materialPresent = materials.lookup != nullptr &&
                materials.lookup(materials.owner, handle,
                    content.materialPackedRevision, content.materialAlphaMode,
                    content.materialDoubleSided) ? 1u : 0u;
            if (content.materialPresent == 0u) {
                content.materialPackedRevision = 0;
                content.materialAlphaMode = 0;
                content.materialDoubleSided = 0;
            }
        }

        // The packet fields of a direct caster (its content apart from the
        // material state, which the material revision covers) and bounds.
        bool sameDirect(const DrawPacket& packet,
            const VulkanCasterContent& content) noexcept {
            return std::memcmp(&packet.worldTransform, &content.worldTransform,
                    sizeof(glm::mat4)) == 0 &&
                packet.geometry.id == content.geometry &&
                packet.material.id == content.material &&
                packet.pipeline.id == content.pipeline &&
                packet.indexCount == content.indexCount &&
                packet.firstIndex == content.firstIndex;
        }

        glm::vec4 packetSphere(const DrawPacket& packet) noexcept {
            return glm::vec4(packet.boundsSphereCenterWorld,
                packet.boundsSphereRadiusWorld);
        }
    }

    VulkanCasterMaterialSource vulkanCasterMaterials(
        const VulkanResourceRegistry& resources) noexcept {
        return { &resources, &registryMaterial, resources.materialRevision() };
    }

    void makeCasterContent(VulkanCasterContent& content,
        const VulkanResolvedCaster& caster,
        const VulkanCasterMaterialSource& materials) noexcept {
        content.worldTransform = caster.worldTransform;
        content.geometry = caster.geometry.id;
        content.material = caster.material.id;
        content.pipeline = caster.pipeline.id;
        content.indexCount = caster.indexCount;
        content.firstIndex = caster.firstIndex;
        setMaterial(content, caster.material, materials);
    }

    void makeCasterContent(VulkanCasterContent& content,
        const DrawPacket& packet,
        const VulkanCasterMaterialSource& materials) noexcept {
        content.worldTransform = packet.worldTransform;
        content.geometry = packet.geometry.id;
        content.material = packet.material.id;
        content.pipeline = packet.pipeline.id;
        content.indexCount = packet.indexCount;
        content.firstIndex = packet.firstIndex;
        setMaterial(content, packet.material, materials);
    }

    bool sameCasterContent(const VulkanCasterContent& lhs,
        const VulkanCasterContent& rhs) noexcept {
        return std::memcmp(&lhs, &rhs, sizeof(VulkanCasterContent)) == 0;
    }

    // ---------------------------------------------------------------------
    // Shadow-caster submission
    // ---------------------------------------------------------------------

    void VulkanShadowCasterRevisions::publishScene(
        const GpuScenePackedTables& scene) noexcept {
        published_ = {
            .valid = true,
            .membershipRevision = scene.shadowConsumerMembershipRevision,
            .contentWatermark = scene.shadowConsumerContentWatermark,
            .size = scene.shadowConsumerPrimitiveIndices.size(),
        };
    }

    bool VulkanShadowCasterRevisions::directPacketsChanged(
        std::span<const DrawPacket> packets) const noexcept {
        if (contents_.size() - directStart_ != packets.size()) return true;
        for (size_t index = 0; index < packets.size(); ++index) {
            const size_t position = directStart_ + index;
            const glm::vec4 sphere = packetSphere(packets[index]);
            if (!sameDirect(packets[index], contents_[position]) ||
                std::memcmp(&sphere, &spheres_[position], sizeof(glm::vec4)) != 0)
                return true;
        }
        return false;
    }

    void VulkanShadowCasterRevisions::refresh(const VulkanIndirectScene& scene,
        const ShadowCasterSubmission& casters,
        const VulkanCasterMaterialSource& materials) {
        ++stats_.evaluations;
        // The application submits the published shadow list with its
        // membership revision; any other list is compared on every call.
        const size_t gpuCount = casters.gpuScenePrimitiveIndices.size();
        const bool identified = gpuCount != 0u && published_.valid &&
            casters.membershipRevision != 0u &&
            casters.membershipRevision == published_.membershipRevision &&
            gpuCount == published_.size;
        const Trigger trigger{
            .valid = true,
            .identified = identified,
            .membershipRevision = casters.membershipRevision,
            .contentWatermark = identified ? published_.contentWatermark : 0u,
            .gpuCount = gpuCount,
            .materialRevision = materials.revision,
        };
        if (trigger == trigger_ && (identified || gpuCount == 0u) &&
            !directPacketsChanged(casters.directPackets))
            return;
        trigger_ = trigger;
        ++stats_.rebuilds;

        // The same resolution, order and fields as the retired hash.
        scratchContents_.clear();
        scratchSpheres_.clear();
        scratchContents_.reserve(casters.size());
        scratchSpheres_.reserve(casters.size());
        size_t gpuResolved = 0;
        visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
            [&](const VulkanResolvedCaster& caster) {
                VulkanCasterContent& content = scratchContents_.emplace_back();
                makeCasterContent(content, caster, materials);
                scratchSpheres_.emplace_back(caster.boundsSphereCenterWorld,
                    caster.boundsSphereRadiusWorld);
                if (caster.gpuScenePrimitiveIndex != InvalidGpuSceneIndex)
                    ++gpuResolved;
            });

        const size_t count = scratchContents_.size();
        const bool sameSize = count == contents_.size();
        bool contentChanged = !sameSize;
        bool boundsChanged = !sameSize;
        for (size_t index = 0; sameSize && index < count; ++index) {
            contentChanged = contentChanged ||
                !sameCasterContent(scratchContents_[index], contents_[index]);
            boundsChanged = boundsChanged || std::memcmp(&scratchSpheres_[index],
                &spheres_[index], sizeof(glm::vec4)) != 0;
        }
        // Equal contents leave directStart_ equal as well; keep it exact.
        directStart_ = gpuResolved;
        if (!contentChanged && !boundsChanged) {
            // The scratch buffers no longer hold the previous sequence.
            previousAvailable_ = false;
            return;
        }
        contents_.swap(scratchContents_);
        spheres_.swap(scratchSpheres_);
        previousAvailable_ = true;
        ++sequenceRevision_;
        ++stats_.sequenceChanges;
        if (contentChanged) revision_ = nextRevision_++;
    }

    uint64_t VulkanShadowCasterRevisions::casterRevision(
        const VulkanIndirectScene& scene, const ShadowCasterSubmission& casters,
        const VulkanCasterMaterialSource& materials) {
        refresh(scene, casters, materials);
        return revision_;
    }

    std::array<uint64_t, kDirectionalShadowCascadeCount>
        VulkanShadowCasterRevisions::directionalRevisions(
            const VulkanIndirectScene& scene,
            const ShadowCasterSubmission& casters,
            const VulkanCasterMaterialSource& materials,
            const DirectionalShadowCascadePlan& plan) {
        refresh(scene, casters, materials);
        const uint32_t ordinal = directionalOrdinal_++;
        lastDirectionalOrdinal_ = ordinal;
        if (directional_.size() <= ordinal) directional_.resize(ordinal + 1u);
        Directional& state = directional_[ordinal];

        uint32_t matrixChanged = 0;
        for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
            ++cascade) {
            if (!state.valid || std::memcmp(&state.matrices[cascade],
                    &plan.cascades[cascade].worldToShadowClip,
                    sizeof(glm::mat4)) != 0)
                matrixChanged |= 1u << cascade;
        }
        const uint64_t lag = sequenceRevision_ - state.sequenceRevision;
        if (state.valid && lag == 0u && matrixChanged == 0u)
            return state.revisions;

        // Cascade membership is a function of the cascade's clip matrix and
        // the caster's bounds; recompute only what can have moved.
        ++stats_.directionalMaskPasses;
        const size_t count = contents_.size();
        const bool masksAligned = state.valid && state.masks.size() == count;
        const uint32_t candidates = masksAligned && lag == 0u
            ? matrixChanged : AllCascades;
        maskScratch_.resize(count);
        for (size_t index = 0; index < count; ++index) {
            const uint8_t previous = masksAligned ? state.masks[index] : 0u;
            const glm::vec4& sphere = spheres_[index];
            maskScratch_[index] = static_cast<uint8_t>(
                (previous & ~candidates) | directionalShadowCasterCascadeMask(
                    plan, glm::vec3(sphere), sphere.w, candidates));
        }

        // Each candidate cascade's caster sequence is compared with the one
        // this light last saw, exactly as the retired hash consumed it.
        uint32_t changedCascades = 0;
        if (!state.valid) {
            changedCascades = AllCascades;
        } else if (lag == 0u) {
            // Same casters: only a cascade whose membership moved can differ.
            uint32_t moved = 0;
            for (size_t index = 0; index < count; ++index)
                moved |= state.masks[index] ^ maskScratch_[index];
            for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
                ++cascade) {
                const uint32_t bit = 1u << cascade;
                if ((moved & bit) != 0u && !sameCascadeSequence(contents_,
                        state.masks, contents_, maskScratch_, bit))
                    changedCascades |= bit;
            }
        } else if (lag == 1u && previousAvailable_ &&
            state.masks.size() == scratchContents_.size()) {
            // One change since this light last looked; the scratch buffers
            // still hold the sequence it saw.
            for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
                ++cascade) {
                const uint32_t bit = 1u << cascade;
                if (!sameCascadeSequence(scratchContents_, state.masks,
                        contents_, maskScratch_, bit))
                    changedCascades |= bit;
            }
        } else {
            // This light missed a change (it was not evaluated in a frame
            // where the casters changed): every cascade is treated as changed.
            changedCascades = AllCascades;
        }
        for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
            ++cascade) {
            if ((changedCascades & (1u << cascade)) != 0u)
                state.revisions[cascade] = nextRevision_++;
            state.matrices[cascade] = plan.cascades[cascade].worldToShadowClip;
        }
        state.masks.swap(maskScratch_);
        state.sequenceRevision = sequenceRevision_;
        state.valid = true;
        return state.revisions;
    }

    bool VulkanShadowCasterRevisions::sameCascadeSequence(
        std::span<const VulkanCasterContent> previous,
        std::span<const uint8_t> previousMasks,
        std::span<const VulkanCasterContent> current,
        std::span<const uint8_t> currentMasks, uint32_t bit) noexcept {
        size_t left = 0;
        size_t right = 0;
        for (;;) {
            while (left < previous.size() && (previousMasks[left] & bit) == 0u)
                ++left;
            while (right < current.size() && (currentMasks[right] & bit) == 0u)
                ++right;
            if (left == previous.size() || right == current.size())
                return left == previous.size() && right == current.size();
            if (!sameCasterContent(previous[left], current[right])) return false;
            ++left;
            ++right;
        }
    }

    // ---------------------------------------------------------------------
    // Depth-history content
    // ---------------------------------------------------------------------

    bool VulkanDepthContentRevision::update(Queue& queue,
        std::span<const DrawPacket> packets, bool contentTrigger,
        const VulkanCasterMaterialSource& materials) {
        // A GPU-scene parity packet's content is a function of its primitive
        // and the published tables, so an unchanged publication only needs its
        // primitive index; direct packets are compared field by field.
        if (!contentTrigger && queue.keys.size() == packets.size()) {
            bool same = true;
            for (size_t index = 0; index < packets.size() && same; ++index) {
                const DrawPacket& packet = packets[index];
                const uint32_t key = hasGpuScenePrimitive(packet)
                    ? packet.firstInstanceTransform : InvalidGpuSceneIndex;
                same = key == queue.keys[index] &&
                    (key != InvalidGpuSceneIndex ||
                        sameDirect(packet, queue.contents[index]));
            }
            if (same) return false;
        }
        queue.scratchKeys.resize(packets.size());
        queue.scratchContents.resize(packets.size());
        const bool sameSize = packets.size() == queue.contents.size();
        bool changed = !sameSize;
        for (size_t index = 0; index < packets.size(); ++index) {
            const DrawPacket& packet = packets[index];
            queue.scratchKeys[index] = hasGpuScenePrimitive(packet)
                ? packet.firstInstanceTransform : InvalidGpuSceneIndex;
            makeCasterContent(queue.scratchContents[index], packet, materials);
            if (!changed)
                changed = !sameCasterContent(queue.scratchContents[index],
                    queue.contents[index]);
        }
        queue.keys.swap(queue.scratchKeys);
        queue.contents.swap(queue.scratchContents);
        return changed;
    }

    bool VulkanDepthContentRevision::update(Queue& queue,
        const VulkanIndirectScene& scene, const OpaqueSubmission& opaque,
        bool contentTrigger, const VulkanCasterMaterialSource& materials) {
        const std::span<const uint32_t> order = opaque.order;
        // A GPU-scene entry's content is a function of its primitive and the
        // published tables, so an unchanged publication only needs its
        // primitive index; direct packets are compared field by field.
        if (!contentTrigger && queue.keys.size() == order.size()) {
            bool same = true;
            for (size_t index = 0; index < order.size() && same; ++index) {
                const uint32_t entry = order[index];
                const bool direct = OpaqueSubmission::isDirect(entry);
                const uint32_t key = direct ? InvalidGpuSceneIndex : entry;
                same = key == queue.keys[index] && (!direct || sameDirect(
                    opaque.directPackets[OpaqueSubmission::indexOf(entry)],
                    queue.contents[index]));
            }
            if (same) return false;
        }
        queue.scratchKeys.resize(order.size());
        queue.scratchContents.resize(order.size());
        const bool sameSize = order.size() == queue.contents.size();
        bool changed = !sameSize;
        VulkanResolvedCaster caster{};
        for (size_t index = 0; index < order.size(); ++index) {
            const uint32_t entry = order[index];
            VulkanCasterContent& content = queue.scratchContents[index];
            if (OpaqueSubmission::isDirect(entry)) {
                queue.scratchKeys[index] = InvalidGpuSceneIndex;
                makeCasterContent(content,
                    opaque.directPackets[OpaqueSubmission::indexOf(entry)],
                    materials);
            }
            else {
                queue.scratchKeys[index] = entry;
                if (resolveIndirectCaster(scene, entry,
                        GpuSceneConsumerMainOpaque, caster))
                    makeCasterContent(content, caster, materials);
                else
                    content = {};
            }
            if (!changed)
                changed = !sameCasterContent(content, queue.contents[index]);
        }
        queue.keys.swap(queue.scratchKeys);
        queue.contents.swap(queue.scratchContents);
        return changed;
    }

    uint64_t VulkanDepthContentRevision::evaluate(
        const VulkanIndirectScene& scene, const OpaqueSubmission& opaque,
        std::span<const DrawPacket> forwardQueue,
        const VulkanCasterMaterialSource& materials) {
        const Trigger trigger{
            .valid = true,
            .sceneEpoch = sceneEpoch_,
            .publicationRevision = publicationRevision_,
            .materialRevision = materials.revision,
        };
        const bool contentTrigger = trigger != trigger_;
        const bool opaqueChanged = update(opaque_, scene, opaque,
            contentTrigger, materials);
        const bool forwardChanged = update(forward_, forwardQueue,
            contentTrigger, materials);
        trigger_ = trigger;
        if (opaqueChanged || forwardChanged) ++revision_;
        return revision_;
    }

} // namespace Iridium
