#include "qualification/vulkan/VulkanCasterRevisionOracle.h"

#include "qualification/LegacyGpuSceneMembershipHash.h"
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/vulkan/VulkanResourceRegistry.h"
#include "renderer/vulkan/VulkanShadowCasters.h"

#include <algorithm>
#include <ostream>

namespace Iridium {

    namespace {
        constexpr uint64_t FnvOffset = 1469598103934665603ull;
        constexpr uint32_t MaximumPrintedDivergences = 32;

        void appendFnv1a(uint64_t& hash, const void* data, size_t size) noexcept {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        }

        void appendCaster(uint64_t& hash, const VulkanResourceRegistry& resources,
            const VulkanResolvedCaster& caster) noexcept {
            appendFnv1a(hash, &caster.worldTransform, sizeof(caster.worldTransform));
            appendFnv1a(hash, &caster.geometry.id, sizeof(caster.geometry.id));
            appendFnv1a(hash, &caster.material.id, sizeof(caster.material.id));
            appendFnv1a(hash, &caster.pipeline.id, sizeof(caster.pipeline.id));
            appendFnv1a(hash, &caster.indexCount, sizeof(caster.indexCount));
            appendFnv1a(hash, &caster.firstIndex, sizeof(caster.firstIndex));
            if (const VulkanMaterialPayload* material =
                    resources.materials().get(caster.material)) {
                appendFnv1a(hash, &material->packedRevision,
                    sizeof(material->packedRevision));
                appendFnv1a(hash, &material->packed.alphaMode,
                    sizeof(material->packed.alphaMode));
                appendFnv1a(hash, &material->packed.doubleSided,
                    sizeof(material->packed.doubleSided));
            }
        }

        const char* streamName(VulkanCasterRevisionStream stream) noexcept {
            switch (stream) {
            case VulkanCasterRevisionStream::Shadow: return "shadow";
            case VulkanCasterRevisionStream::DirectionalShadow: return "directional";
            case VulkanCasterRevisionStream::DepthHistory: return "depth_history";
            case VulkanCasterRevisionStream::Membership: return "membership";
            }
            return "unknown";
        }

        const char* relationName(VulkanCasterRevisionOracle::Relation relation) noexcept {
            switch (relation) {
            case VulkanCasterRevisionOracle::Relation::Unchanged: return "unchanged";
            case VulkanCasterRevisionOracle::Relation::BothChanged: return "both_changed";
            case VulkanCasterRevisionOracle::Relation::RevisionOnly: return "revision_only";
            case VulkanCasterRevisionOracle::Relation::HashOnly: return "hash_only";
            }
            return "unknown";
        }
    }

    uint64_t legacyShadowCasterHash(const VulkanIndirectScene& scene,
        const VulkanResourceRegistry& resources,
        const ShadowCasterSubmission& casters) noexcept {
        uint64_t hash = FnvOffset;
        visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
            [&](const VulkanResolvedCaster& caster) {
                appendCaster(hash, resources, caster);
            });
        return hash;
    }

    std::array<uint64_t, kDirectionalShadowCascadeCount>
        legacyDirectionalShadowCasterHashes(const VulkanIndirectScene& scene,
            const VulkanResourceRegistry& resources,
            const ShadowCasterSubmission& casters,
            const DirectionalShadowCascadePlan& plan) noexcept {
        std::array<uint64_t, kDirectionalShadowCascadeCount> hashes{};
        hashes.fill(FnvOffset);
        visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
            [&](const VulkanResolvedCaster& caster) {
                const uint32_t cascadeMask = directionalShadowCasterCascadeMask(
                    plan, caster.boundsSphereCenterWorld,
                    caster.boundsSphereRadiusWorld, 0xfu);
                for (uint32_t cascade = 0;
                    cascade < kDirectionalShadowCascadeCount; ++cascade) {
                    if ((cascadeMask & (1u << cascade)) == 0u) continue;
                    appendCaster(hashes[cascade], resources, caster);
                }
            });
        return hashes;
    }

    uint64_t legacyDepthContentHash(const VulkanIndirectScene& scene,
        const VulkanResourceRegistry& resources,
        std::span<const DrawPacket> opaqueQueue,
        std::span<const DrawPacket> forwardQueue) noexcept {
        uint64_t hash = legacyShadowCasterHash(scene, resources, {
            .directPackets = opaqueQueue,
        });
        const uint64_t forwardHash = legacyShadowCasterHash(scene, resources, {
            .directPackets = forwardQueue,
        });
        appendFnv1a(hash, &forwardHash, sizeof(forwardHash));
        return hash == 0u ? 1u : hash;
    }

    VulkanCasterRevisionOracle::State& VulkanCasterRevisionOracle::state(
        VulkanCasterRevisionStream stream, uint32_t ordinal, uint32_t cascade) {
        switch (stream) {
        case VulkanCasterRevisionStream::Shadow: return shadow_;
        case VulkanCasterRevisionStream::DepthHistory: return depth_;
        case VulkanCasterRevisionStream::Membership:
            return membership_[(std::min)(ordinal, 2u)];
        case VulkanCasterRevisionStream::DirectionalShadow: break;
        }
        const size_t index = static_cast<size_t>(ordinal) *
            kDirectionalShadowCascadeCount + cascade;
        if (directional_.size() <= index) directional_.resize(index + 1u);
        return directional_[index];
    }

    VulkanCasterRevisionOracle::Relation VulkanCasterRevisionOracle::compare(
        VulkanCasterRevisionStream stream, uint32_t ordinal, uint32_t cascade,
        uint64_t frameSerial, uint64_t hash, uint64_t revision) {
        State& previous = state(stream, ordinal, cascade);
        const bool first = !previous.valid;
        const bool hashChanged = previous.hash != hash;
        const bool revisionChanged = previous.revision != revision;
        previous = { true, hash, revision };
        if (first) return Relation::Unchanged;

        Totals& totals = totals_[static_cast<size_t>(stream)];
        ++totals.samples;
        Relation relation = Relation::Unchanged;
        if (hashChanged && revisionChanged) relation = Relation::BothChanged;
        else if (revisionChanged) relation = Relation::RevisionOnly;
        else if (hashChanged) relation = Relation::HashOnly;
        switch (relation) {
        case Relation::Unchanged: ++totals.unchanged; break;
        case Relation::BothChanged: ++totals.bothChanged; break;
        case Relation::RevisionOnly: ++totals.revisionOnly; break;
        case Relation::HashOnly: ++totals.hashOnly; break;
        }
        if ((relation == Relation::RevisionOnly || relation == Relation::HashOnly) &&
            output_ != nullptr && printedDivergences_ < MaximumPrintedDivergences) {
            ++printedDivergences_;
            *output_ << "IRIDIUM_CASTER_REVISION_DIVERGENCE {\"stream\":\""
                << streamName(stream) << "\",\"ordinal\":" << ordinal
                << ",\"cascade\":" << cascade
                << ",\"frame_serial\":" << frameSerial
                << ",\"relation\":\"" << relationName(relation) << "\"}\n";
        }
        return relation;
    }

    void VulkanCasterRevisionOracle::observeCasterRevision(
        const VulkanCasterRevisionSample& sample) {
        if (sample.stream == VulkanCasterRevisionStream::Membership) {
            // Ordinal 0: the shadow list; 1: the probe list; 2: main opaque.
            if (sample.tables == nullptr || sample.revisions.size() != 3u) return;
            const GpuScenePackedTables& tables = *sample.tables;
            (void)compare(sample.stream, 0, 0, sample.frameSerial,
                legacyGpuSceneMembershipHash(tables, GpuSceneConsumerShadow,
                    tables.shadowConsumerPrimitiveIndices), sample.revisions[0]);
            (void)compare(sample.stream, 1, 0, sample.frameSerial,
                legacyGpuSceneMembershipHash(tables, GpuSceneConsumerProbe,
                    tables.probeConsumerPrimitiveIndices), sample.revisions[1]);
            (void)compare(sample.stream, 2, 0, sample.frameSerial,
                legacyGpuSceneMembershipHash(tables, GpuSceneConsumerMainOpaque,
                    tables.mainOpaqueConsumerPrimitiveIndices), sample.revisions[2]);
            return;
        }
        if (sample.scene == nullptr || sample.resources == nullptr) return;
        switch (sample.stream) {
        case VulkanCasterRevisionStream::Shadow:
            if (sample.casters == nullptr || sample.revisions.size() != 1u) return;
            (void)compare(sample.stream, 0, 0, sample.frameSerial,
                legacyShadowCasterHash(*sample.scene, *sample.resources,
                    *sample.casters), sample.revisions[0]);
            return;
        case VulkanCasterRevisionStream::DirectionalShadow: {
            if (sample.casters == nullptr || sample.plan == nullptr ||
                sample.revisions.size() != kDirectionalShadowCascadeCount)
                return;
            const auto hashes = legacyDirectionalShadowCasterHashes(
                *sample.scene, *sample.resources, *sample.casters, *sample.plan);
            for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
                ++cascade)
                (void)compare(sample.stream, sample.ordinal, cascade,
                    sample.frameSerial, hashes[cascade], sample.revisions[cascade]);
            return;
        }
        case VulkanCasterRevisionStream::DepthHistory:
            if (sample.revisions.size() != 1u) return;
            (void)compare(sample.stream, 0, 0, sample.frameSerial,
                legacyDepthContentHash(*sample.scene, *sample.resources,
                    sample.opaqueQueue, sample.forwardQueue), sample.revisions[0]);
            return;
        case VulkanCasterRevisionStream::Membership:
            return;
        }
    }

    bool VulkanCasterRevisionOracle::passed() const noexcept {
        for (const Totals& totals : totals_)
            if (totals.hashOnly != 0u) return false;
        return true;
    }

    void VulkanCasterRevisionOracle::finish() {
        if (finished_) return;
        finished_ = true;
        if (output_ == nullptr) return;
        *output_ << "IRIDIUM_CASTER_REVISION_ORACLE {";
        for (uint32_t stream = 0; stream < StreamCount; ++stream) {
            const Totals& totals = totals_[stream];
            *output_ << "\"" << streamName(static_cast<VulkanCasterRevisionStream>(stream))
                << "\":{\"samples\":" << totals.samples
                << ",\"unchanged\":" << totals.unchanged
                << ",\"both_changed\":" << totals.bothChanged
                << ",\"revision_only\":" << totals.revisionOnly
                << ",\"hash_only\":" << totals.hashOnly << "},";
        }
        *output_ << "\"passed\":" << (passed() ? "true" : "false") << "}\n";
        output_->flush();
    }

} // namespace Iridium
