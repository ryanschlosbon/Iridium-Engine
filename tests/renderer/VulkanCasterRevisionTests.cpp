// Device-free tests for the change-driven caster revisions (M7R R5c.1/R5c.2).
//
// Each revision source is driven on its own: the shadow list's content
// watermark (GpuScene.cpp), GPU-scene record changes through the publisher,
// the shadow list's membership, material state, direct packets, directional
// cascade membership and the depth-history queues. Every step compares the
// revision's change relation with an independent re-implementation of the
// retired per-frame FNV-1a hash over the same inputs; a final randomized test
// runs many mixed steps.
#include "renderer/lighting/DirectionalShadow.h"
#include "renderer/vulkan/VulkanCasterRevisions.h"
#include "renderer/vulkan/VulkanShadowCasters.h"
#include "renderer/scene/GpuScenePublisher.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    SceneEntityUuid uuid(uint32_t value) {
        char text[40];
        std::snprintf(text, sizeof(text), "019fb73d-5a60-7000-8000-%012x", value);
        return *SceneEntityUuid::parse(text);
    }
    AssetGuid guid(uint32_t value) {
        char text[40];
        std::snprintf(text, sizeof(text), "019fb73d-5a61-7000-8000-%012x", value);
        return *AssetGuid::parse(text);
    }

    // ---------------------------------------------------------------------
    // Fake backend material state.
    // ---------------------------------------------------------------------
    struct FakeMaterial {
        uint64_t packedRevision = 1;
        uint32_t alphaMode = 0;
        uint32_t doubleSided = 0;
    };
    struct FakeMaterials {
        std::map<uint32_t, FakeMaterial> live;
        uint64_t revision = 1;

        void set(MaterialHandle handle, FakeMaterial material) {
            live[handle.id] = material;
            ++revision;
        }
        void erase(MaterialHandle handle) { live.erase(handle.id); ++revision; }
        [[nodiscard]] VulkanCasterMaterialSource source() const {
            return { this, [](const void* owner, MaterialHandle handle,
                uint64_t& packedRevision, uint32_t& alphaMode, uint32_t& doubleSided) {
                const auto& self = *static_cast<const FakeMaterials*>(owner);
                const auto found = self.live.find(handle.id);
                if (found == self.live.end()) return false;
                packedRevision = found->second.packedRevision;
                alphaMode = found->second.alphaMode;
                doubleSided = found->second.doubleSided;
                return true;
            }, revision };
        }
    };

    // ---------------------------------------------------------------------
    // The retired hash, re-implemented independently over the fake materials.
    // ---------------------------------------------------------------------
    void fnv(uint64_t& hash, const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    }
    void appendReference(uint64_t& hash, const FakeMaterials& materials,
        const VulkanResolvedCaster& caster) {
        fnv(hash, &caster.worldTransform, sizeof(caster.worldTransform));
        fnv(hash, &caster.geometry.id, 4);
        fnv(hash, &caster.material.id, 4);
        fnv(hash, &caster.pipeline.id, 4);
        fnv(hash, &caster.indexCount, 4);
        fnv(hash, &caster.firstIndex, 4);
        const auto found = materials.live.find(caster.material.id);
        if (found != materials.live.end()) {
            fnv(hash, &found->second.packedRevision, 8);
            fnv(hash, &found->second.alphaMode, 4);
            fnv(hash, &found->second.doubleSided, 4);
        }
    }
    uint64_t referenceHash(const VulkanIndirectScene& scene,
        const ShadowCasterSubmission& casters, const FakeMaterials& materials) {
        uint64_t hash = 1469598103934665603ull;
        visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
            [&](const VulkanResolvedCaster& caster) {
                appendReference(hash, materials, caster);
            });
        return hash;
    }
    std::array<uint64_t, kDirectionalShadowCascadeCount> referenceCascadeHashes(
        const VulkanIndirectScene& scene, const ShadowCasterSubmission& casters,
        const FakeMaterials& materials, const DirectionalShadowCascadePlan& plan) {
        std::array<uint64_t, kDirectionalShadowCascadeCount> hashes{};
        hashes.fill(1469598103934665603ull);
        visitIndirectCasters(scene, casters, GpuSceneConsumerShadow,
            [&](const VulkanResolvedCaster& caster) {
                const uint32_t mask = directionalShadowCasterCascadeMask(plan,
                    caster.boundsSphereCenterWorld, caster.boundsSphereRadiusWorld,
                    0xfu);
                for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
                    ++cascade)
                    if ((mask & (1u << cascade)) != 0u)
                        appendReference(hashes[cascade], materials, caster);
            });
        return hashes;
    }

    // ---------------------------------------------------------------------
    // A small published scene: instances of one shared mesh at positions.
    // ---------------------------------------------------------------------
    struct Caster {
        glm::vec3 position{ 0.0f };
        uint32_t material = 1;      // MaterialHandle index
        bool shadow = true;         // instance casts shadows
        bool selected = false;
        bool present = true;
    };

    struct World {
        std::vector<Caster> casters;
        std::vector<DrawPacket> directs;
        FakeMaterials materials;
        GpuScenePublisher publisher{ GpuSceneCapacity{ 64, 128, 16, 128 } };
        std::vector<GpuSceneObservedInstance> observations;
        const GpuScenePackedTables* packed = nullptr;
        uint64_t observationRevision = 0;
        uint64_t serial = 0;

        void publish() {
            ++observationRevision;
            observations.clear();
            for (size_t index = 0; index < casters.size(); ++index) {
                const Caster& caster = casters[index];
                if (!caster.present) continue;
                GpuSceneObservedInstance instance;
                instance.identity = { uuid(0x100u + static_cast<uint32_t>(index)),
                    guid(1), 1, "cook" };
                instance.observationRevision = observationRevision;
                instance.flags = GpuSceneInstanceEnabled |
                    (caster.selected ? GpuSceneInstanceSelected : 0u);
                instance.consumerMask = GpuSceneConsumerMainOpaque |
                    GpuSceneConsumerProbe | GpuSceneConsumerSelection |
                    (caster.shadow ? GpuSceneConsumerShadow : 0u);
                instance.worldTransform = glm::translate(glm::mat4(1.0f),
                    caster.position);
                instance.worldBoundsSphere = glm::vec4(caster.position, 1.0f);
                instance.worldBoundsMin = glm::vec4(caster.position - 1.0f, 0.0f);
                instance.worldBoundsMax = glm::vec4(caster.position + 1.0f, 0.0f);
                instance.primitives.push_back({
                    .identity = { instance.identity.owner, guid(2), guid(3),
                        guid(10u + caster.material) },
                    .geometryIdentity = { guid(1), guid(2), guid(3) },
                    .legacyGeometry = GeometryHandle::fromParts(4, 1),
                    .material = MaterialHandle::fromParts(caster.material, 1),
                    .pipeline = PipelineHandle::fromParts(6, 1),
                    .firstIndex = 0, .indexCount = 36,
                    .consumerMask = GpuSceneConsumerMainOpaque |
                        GpuSceneConsumerShadow | GpuSceneConsumerProbe,
                    .localBoundsSphere = { 0.0f, 0.0f, 0.0f, 1.0f },
                    .localBoundsMin = { -1.0f, -1.0f, -1.0f, 0.0f },
                    .localBoundsMax = { 1.0f, 1.0f, 1.0f, 0.0f },
                    .materialRevision = 1,
                });
                observations.push_back(std::move(instance));
            }
            packed = &publisher.synchronize(1, observations, serial + 2u, serial);
            ++serial;
        }
        [[nodiscard]] VulkanIndirectScene scene() const {
            return {
                .transforms = packed->transforms,
                .instances = packed->instances,
                .primitives = packed->primitives,
                .geometries = packed->geometries,
                .identities = packed->primitiveIdentities,
                .published = {
                    static_cast<uint32_t>(packed->transforms.size()),
                    static_cast<uint32_t>(packed->instances.size()),
                    static_cast<uint32_t>(packed->primitives.size()),
                    static_cast<uint32_t>(packed->geometries.size()),
                },
            };
        }
        [[nodiscard]] ShadowCasterSubmission submission() const {
            return {
                .gpuScenePrimitiveIndices = packed->shadowConsumerPrimitiveIndices,
                .directPackets = directs,
                .membershipRevision = packed->shadowConsumerMembershipRevision,
            };
        }
    };

    World makeWorld(size_t count) {
        World world;
        for (size_t index = 0; index < count; ++index)
            world.casters.push_back({ .position = { 2.0f + 6.0f * index, 0.0f, -10.0f } });
        for (uint32_t material = 1; material <= 4; ++material)
            world.materials.set(MaterialHandle::fromParts(material, 1), {});
        world.publish();
        return world;
    }

    // Steps a world and checks the shadow revision's change relation against
    // the reference hash.
    struct ShadowProbe {
        VulkanShadowCasterRevisions revisions;
        uint64_t hash = 0;
        uint64_t revision = 0;
        bool primed = false;

        // Returns 1 when both changed, 0 when neither, -1 on a mismatch.
        int step(World& world) {
            revisions.publishScene(*world.packed);
            const VulkanIndirectScene scene = world.scene();
            const ShadowCasterSubmission casters = world.submission();
            const uint64_t nextRevision = revisions.casterRevision(scene, casters,
                world.materials.source());
            const uint64_t nextHash = referenceHash(scene, casters, world.materials);
            const bool hashChanged = primed && nextHash != hash;
            const bool revisionChanged = primed && nextRevision != revision;
            hash = nextHash;
            revision = nextRevision;
            primed = true;
            if (hashChanged != revisionChanged) return -1;
            return hashChanged ? 1 : 0;
        }
    };

    // ---------------------------------------------------------------------
    // Revision sources.
    // ---------------------------------------------------------------------

    // GpuScene.cpp: the shadow list's content watermark rises with a member's
    // transform, primitive or geometry record and ignores everything else.
    bool watermarkTracksMemberRecords() {
        World world = makeWorld(3);
        const uint64_t initial = world.packed->shadowConsumerContentWatermark;
        CHECK(initial != 0u);
        CHECK(initial == gpuSceneConsumerContentWatermark(*world.packed,
            world.packed->shadowConsumerPrimitiveIndices));

        world.publish();  // republished, unchanged
        CHECK(world.packed->shadowConsumerContentWatermark == initial);

        world.casters[0].selected = true;  // instance-record change only
        world.publish();
        CHECK(world.packed->shadowConsumerContentWatermark == initial);

        world.casters[1].position.y += 0.5f;  // a member's transform
        world.publish();
        const uint64_t moved = world.packed->shadowConsumerContentWatermark;
        CHECK(moved > initial);

        world.casters[2].material = 3;  // a member's primitive record
        world.publish();
        CHECK(world.packed->shadowConsumerContentWatermark > moved);

        // A non-member's transform does not move the shadow watermark.
        const uint64_t before = world.packed->shadowConsumerContentWatermark;
        world.casters[0].shadow = false;
        world.publish();
        world.casters[0].position.x += 1.0f;
        const uint64_t afterExclusion = world.packed->shadowConsumerContentWatermark;
        world.publish();
        CHECK(world.packed->shadowConsumerContentWatermark == afterExclusion);
        (void)before;
        return true;
    }

    // Publisher record revisions (transforms, primitives) through the
    // watermark trigger.
    bool shadowRevisionFollowsRecordChanges() {
        World world = makeWorld(4);
        ShadowProbe probe;
        CHECK(probe.step(world) == 0);
        CHECK(probe.step(world) == 0);               // static frame
        world.publish();
        CHECK(probe.step(world) == 0);               // unchanged publication
        world.casters[2].position.z -= 1.0f;
        world.publish();
        CHECK(probe.step(world) == 1);               // transform
        CHECK(probe.step(world) == 0);
        world.casters[1].material = 2;
        world.publish();
        CHECK(probe.step(world) == 1);               // primitive binding
        world.casters[3].selected = true;
        world.publish();
        CHECK(probe.step(world) == 0);               // selection: no content
        CHECK(probe.revisions.stats().rebuilds >= 4u);
        return true;
    }

    // Shadow list membership: instances entering and leaving the list.
    bool shadowRevisionFollowsMembership() {
        World world = makeWorld(4);
        ShadowProbe probe;
        CHECK(probe.step(world) == 0);
        world.casters[1].shadow = false;
        world.publish();
        CHECK(probe.step(world) == 1);
        world.casters[1].shadow = true;
        world.publish();
        CHECK(probe.step(world) == 1);
        world.casters[0].present = false;  // removal shifts dense slots
        world.publish();
        CHECK(probe.step(world) == 1);
        world.casters.push_back({ .position = { 40.0f, 0.0f, -10.0f } });
        world.publish();
        CHECK(probe.step(world) == 1);
        CHECK(probe.step(world) == 0);
        return true;
    }

    // Backend material state: only materials a caster uses count.
    bool shadowRevisionFollowsMaterials() {
        World world = makeWorld(3);  // all casters use material 1
        ShadowProbe probe;
        CHECK(probe.step(world) == 0);
        world.materials.set(MaterialHandle::fromParts(4, 1), { .packedRevision = 7 });
        CHECK(probe.step(world) == 0);               // unused material
        world.materials.set(MaterialHandle::fromParts(1, 1),
            { .packedRevision = 2, .alphaMode = 1 });
        CHECK(probe.step(world) == 1);               // used material
        world.materials.erase(MaterialHandle::fromParts(1, 1));
        CHECK(probe.step(world) == 1);               // payload freed
        world.materials.set(MaterialHandle::fromParts(9, 1), {});
        CHECK(probe.step(world) == 0);
        return true;
    }

    // Direct (non-GPU-scene) packets: compared field by field each call.
    bool shadowRevisionFollowsDirectPackets() {
        World world = makeWorld(2);
        DrawPacket packet{};
        packet.worldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f));
        packet.geometry = GeometryHandle::fromParts(8, 1);
        packet.material = MaterialHandle::fromParts(2, 1);
        packet.pipeline = PipelineHandle::fromParts(6, 1);
        packet.indexCount = 6;
        packet.boundsSphereCenterWorld = glm::vec3(1.0f);
        packet.boundsSphereRadiusWorld = 1.0f;
        world.directs = { packet };
        ShadowProbe probe;
        CHECK(probe.step(world) == 0);
        world.directs[0].distanceToCamera = 42.0f;   // not caster content
        CHECK(probe.step(world) == 0);
        world.directs[0].worldTransform[3].x += 0.25f;
        CHECK(probe.step(world) == 1);
        world.directs[0].firstIndex = 3;
        CHECK(probe.step(world) == 1);
        world.directs.push_back(packet);
        CHECK(probe.step(world) == 1);
        world.directs.clear();
        CHECK(probe.step(world) == 1);
        CHECK(probe.step(world) == 0);
        return true;
    }

    // Directional cascades: four side-by-side orthographic cascades along x.
    DirectionalShadowCascadePlan stripPlan(float shift) {
        DirectionalShadowCascadePlan plan{};
        for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
            ++cascade) {
            const float left = 20.0f * static_cast<float>(cascade) + shift;
            plan.cascades[cascade].worldToShadowClip = glm::orthoRH_ZO(left,
                left + 20.0f, -10.0f, 10.0f, 0.1f, 100.0f);
            plan.splitFar[cascade] = 20.0f * static_cast<float>(cascade + 1u);
        }
        return plan;
    }

    struct DirectionalProbe {
        VulkanShadowCasterRevisions revisions;
        std::array<uint64_t, kDirectionalShadowCascadeCount> hashes{};
        std::array<uint64_t, kDirectionalShadowCascadeCount> values{};
        bool primed = false;

        // The cascades whose hash changed, or -1 on a relation mismatch.
        int step(World& world, const DirectionalShadowCascadePlan& plan) {
            revisions.publishScene(*world.packed);
            revisions.beginFrame();
            const VulkanIndirectScene scene = world.scene();
            const ShadowCasterSubmission casters = world.submission();
            const auto next = revisions.directionalRevisions(scene, casters,
                world.materials.source(), plan);
            const auto reference = referenceCascadeHashes(scene, casters,
                world.materials, plan);
            int changed = 0;
            for (uint32_t cascade = 0; cascade < kDirectionalShadowCascadeCount;
                ++cascade) {
                const bool hashChanged = primed && reference[cascade] != hashes[cascade];
                const bool valueChanged = primed && next[cascade] != values[cascade];
                if (hashChanged != valueChanged) return -1;
                if (hashChanged) changed |= 1 << cascade;
            }
            hashes = reference;
            values = next;
            primed = true;
            return changed;
        }
    };

    bool directionalRevisionsFollowCascadeMembership() {
        // Casters at x = 2, 8, 14 (cascade 0), 20 (0 and 1), 26 (1).
        World world = makeWorld(5);
        DirectionalProbe probe;
        CHECK(probe.step(world, stripPlan(0.0f)) == 0);
        CHECK(probe.step(world, stripPlan(0.0f)) == 0);
        // Moving one caster inside cascade 1 changes cascade 1 only.
        world.casters[4].position.y += 1.0f;
        world.publish();
        CHECK(probe.step(world, stripPlan(0.0f)) == 0b0010);
        // Shifting every cascade by a little moves no caster across a border.
        CHECK(probe.step(world, stripPlan(0.25f)) == 0);
        // A larger shift moves casters between cascades.
        const int shifted = probe.step(world, stripPlan(5.5f));
        CHECK(shifted > 0);
        // Back again: the hashes return to earlier values, the revisions
        // advance; both change.
        CHECK(probe.step(world, stripPlan(0.0f)) > 0);
        // Material change of a caster in cascade 0 only.
        world.casters[0].material = 2;
        world.publish();
        CHECK(probe.step(world, stripPlan(0.0f)) == 0b0001);
        world.materials.set(MaterialHandle::fromParts(2, 1), { .packedRevision = 5 });
        CHECK(probe.step(world, stripPlan(0.0f)) == 0b0001);
        // A caster-count change.
        world.casters[1].shadow = false;
        world.publish();
        CHECK(probe.step(world, stripPlan(0.0f)) == 0b0001);
        return true;
    }

    // Depth history: opaque and forward queues of parity and direct packets.
    DrawPacket parityPacket(const World& world, uint32_t primitiveIndex) {
        const GpuScenePackedTables& tables = *world.packed;
        const GpuScenePrimitiveRecord& primitive = tables.primitives[primitiveIndex];
        const GpuSceneInstanceRecord& instance = tables.instances[primitive.binding.x];
        const GpuSceneGeometryRecord& geometry = tables.geometries[primitive.binding.y];
        DrawPacket packet{};
        packet.worldTransform = unpackGpuSceneAffine(
            tables.transforms[instance.references.x]);
        packet.geometry = GeometryHandle{ geometry.storage.x };
        packet.material = MaterialHandle{ primitive.binding.z };
        packet.pipeline = PipelineHandle{ primitive.binding.w };
        packet.indexCount = geometry.draw.y;
        packet.firstIndex = geometry.draw.x;
        packet.executionFlags = DrawPacketGpuScenePrimitive;
        packet.firstInstanceTransform = primitiveIndex;
        return packet;
    }

    uint64_t referenceDepthHash(std::span<const DrawPacket> opaque,
        std::span<const DrawPacket> forward, const FakeMaterials& materials) {
        const VulkanIndirectScene empty{};
        uint64_t hash = referenceHash(empty, { .directPackets = opaque }, materials);
        const uint64_t forwardHash = referenceHash(empty,
            { .directPackets = forward }, materials);
        fnv(hash, &forwardHash, sizeof(forwardHash));
        return hash;
    }

    // M7R R5c.4b/d: the revision reads the opaque submission (GPU-scene
    // primitives resolved from the records, direct packets); the reference is
    // the retired hash over the parity queue those entries stand for.
    bool depthRevisionFollowsQueues() {
        World world = makeWorld(4);
        VulkanDepthContentRevision depth;
        uint64_t hash = 0, revision = 0;
        bool primed = false;
        std::vector<DrawPacket> opaque, forward;
        std::vector<DrawPacket> directs;
        std::vector<uint32_t> order;
        const auto build = [&](std::vector<uint32_t> forwardVisible) {
            directs.clear();
            forward.clear();
            for (uint32_t index : forwardVisible)
                forward.push_back(parityPacket(world, index));
        };
        const auto step = [&]() {
            // GPU-scene primitives 0 and 1, then the direct packets.
            order = { 0u, 1u };
            opaque = { parityPacket(world, 0), parityPacket(world, 1) };
            for (uint32_t index = 0; index < directs.size(); ++index) {
                order.push_back(OpaqueSubmissionDirectBit | index);
                opaque.push_back(directs[index]);
            }
            depth.publishScene(*world.packed);
            const uint64_t nextRevision = depth.evaluate(world.scene(),
                OpaqueSubmission{ .order = order, .directPackets = directs },
                forward, world.materials.source());
            const uint64_t nextHash = referenceDepthHash(opaque, forward,
                world.materials);
            const bool hashChanged = primed && nextHash != hash;
            const bool revisionChanged = primed && nextRevision != revision;
            hash = nextHash;
            revision = nextRevision;
            primed = true;
            if (hashChanged != revisionChanged) return -1;
            return hashChanged ? 1 : 0;
        };
        build({ 2, 3 });
        CHECK(step() == 0);
        CHECK(step() == 0);
        build({ 2 });                                 // forward visibility
        CHECK(step() == 1);
        CHECK(step() == 0);
        world.publish();                              // unchanged publication
        build({ 2 });
        CHECK(step() == 0);
        world.casters[0].position.x += 0.5f;          // opaque occluder moved
        world.publish();
        build({ 2 });
        CHECK(step() == 1);
        DrawPacket direct{};                           // a direct opaque packet
        direct.geometry = GeometryHandle::fromParts(9, 1);
        direct.material = MaterialHandle::fromParts(3, 1);
        direct.pipeline = PipelineHandle::fromParts(6, 1);
        direct.indexCount = 3;
        directs.push_back(direct);
        CHECK(step() == 1);
        directs.back().distanceToCamera = 3.0f;        // not content
        CHECK(step() == 0);
        directs.back().worldTransform[3].y = 2.0f;
        CHECK(step() == 1);
        world.materials.set(MaterialHandle::fromParts(3, 1), { .packedRevision = 4 });
        CHECK(step() == 1);
        world.materials.set(MaterialHandle::fromParts(4, 1), { .packedRevision = 4 });
        CHECK(step() == 0);
        return true;
    }

    // Many mixed steps: the local and directional relations always match.
    bool randomizedEquivalence() {
        std::mt19937 random(20261003u);
        World world = makeWorld(12);
        ShadowProbe shadow;
        DirectionalProbe directional;
        float shift = 0.0f;
        uint32_t changes = 0;
        for (uint32_t frame = 0; frame < 600u; ++frame) {
            const uint32_t action = random() % 10u;
            const size_t target = random() % world.casters.size();
            bool republish = false;
            switch (action) {
            case 0: world.casters[target].position.x += 0.5f; republish = true; break;
            case 1: world.casters[target].position.y -= 0.25f; republish = true; break;
            case 2: world.casters[target].material = 1u + random() % 4u; republish = true; break;
            case 3: world.casters[target].shadow = !world.casters[target].shadow; republish = true; break;
            case 4: world.casters[target].selected = !world.casters[target].selected; republish = true; break;
            case 5: world.materials.set(MaterialHandle::fromParts(1u + random() % 4u, 1),
                        { .packedRevision = 1u + random() % 3u }); break;
            case 6: shift = static_cast<float>(random() % 9u); break;
            case 7: world.casters[target].present = !world.casters[target].present; republish = true; break;
            default: break;  // static frame
            }
            if (republish) world.publish();
            const int local = shadow.step(world);
            CHECK(local >= 0);
            const int cascades = directional.step(world, stripPlan(shift));
            CHECK(cascades >= 0);
            changes += local > 0 ? 1u : 0u;
        }
        CHECK(changes > 50u);
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Shadow-list content watermark", watermarkTracksMemberRecords },
        { "Shadow revision: GPU-scene record changes", shadowRevisionFollowsRecordChanges },
        { "Shadow revision: list membership", shadowRevisionFollowsMembership },
        { "Shadow revision: material state", shadowRevisionFollowsMaterials },
        { "Shadow revision: direct packets", shadowRevisionFollowsDirectPackets },
        { "Directional revisions: cascade membership", directionalRevisionsFollowCascadeMembership },
        { "Depth-history revision: queues", depthRevisionFollowsQueues },
        { "Randomized change-relation equivalence", randomizedEquivalence },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
