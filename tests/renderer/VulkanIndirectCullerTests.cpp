// Device-free identity tests for the GPU-driven indirect cullers (M7R R3a.7).
//
// The cullers record through VulkanCullerCommands and allocate through
// VulkanCullerResources; here both are fakes that log every call. Each test
// builds a small GPU scene (two vertex/index arenas, an index-format change,
// alpha-masked and double-sided materials, a resident LOD chain that breaks on
// a buffer change) and asserts the exact ordered command log and the exact
// host-written candidate/count bytes per view kind, against expectations
// written out by hand from the binning rules.
#include "renderer/vulkan/VulkanIndirectViewCuller.h"
#include "renderer/vulkan/VulkanOpaqueIndirectCuller.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
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

    template<typename Handle>
    Handle fake(uint64_t value) {
        return reinterpret_cast<Handle>(static_cast<uintptr_t>(value));
    }
    uint64_t id(const void* handle) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    }

    // ---------------------------------------------------------------------
    // Logging seams.
    // ---------------------------------------------------------------------
    struct Recorder {
        std::vector<std::string> log;
        std::vector<std::unique_ptr<std::vector<std::byte>>> memory;
        uint64_t nextHandle = 0x1000;
        // R4c.2: frame slots reported in flight, and buffers sent to the
        // deletion queue.
        std::array<bool, kIndirectCullerFramesInFlight> inFlight{};
        std::vector<VkBuffer> retired;
        uint32_t descriptorWrites = 0;

        void add(std::string line) { log.push_back(std::move(line)); }
        static Recorder& of(void* user) { return *static_cast<Recorder*>(user); }
    };

    std::string words(const void* data, uint32_t size) {
        std::ostringstream out;
        const auto* values = static_cast<const uint32_t*>(data);
        for (uint32_t index = 0; index < size / 4u; ++index)
            out << (index ? "," : "") << values[index];
        return out.str();
    }

    VulkanCullerCommands loggingCommands(Recorder& recorder) {
        return {
            .user = &recorder,
            .bindPipeline = [](void* user, VkCommandBuffer, VkPipelineBindPoint point,
                VkPipeline pipeline) {
                Recorder::of(user).add("pipe " + std::to_string(point) + " " +
                    std::to_string(id(pipeline)));
            },
            .bindDescriptorSets = [](void* user, VkCommandBuffer,
                VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                uint32_t count, const VkDescriptorSet* sets, uint32_t dynamicCount,
                const uint32_t* dynamic) {
                std::ostringstream out;
                out << "sets " << point << " L" << id(layout) << " first=" << first
                    << " [";
                for (uint32_t index = 0; index < count; ++index)
                    out << (index ? "," : "") << id(sets[index]);
                out << "] dyn=[";
                for (uint32_t index = 0; index < dynamicCount; ++index)
                    out << (index ? "," : "") << dynamic[index];
                out << "]";
                Recorder::of(user).add(out.str());
            },
            .pushConstants = [](void* user, VkCommandBuffer, VkPipelineLayout layout,
                VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                const void* values) {
                std::ostringstream out;
                out << "push L" << id(layout) << " stages=" << stages << " off="
                    << offset << " ";
                if (size == sizeof(CanonicalMeshPushConstants)) {
                    CanonicalMeshPushConstants push{};
                    std::memcpy(&push, values, sizeof(push));
                    out << "mesh slot=" << push.padding[0] << " material="
                        << push.materialIndex;
                }
                else
                    out << "[" << words(values, size) << "]";
                Recorder::of(user).add(out.str());
            },
            .dispatch = [](void* user, VkCommandBuffer, uint32_t x, uint32_t y,
                uint32_t z) {
                Recorder::of(user).add("dispatch " + std::to_string(x) + " " +
                    std::to_string(y) + " " + std::to_string(z));
            },
            .memoryBarrier = [](void* user, VkCommandBuffer, VkPipelineStageFlags src,
                VkPipelineStageFlags dst, VkAccessFlags srcAccess,
                VkAccessFlags dstAccess) {
                Recorder::of(user).add("barrier " + std::to_string(src) + "->" +
                    std::to_string(dst) + " " + std::to_string(srcAccess) + "->" +
                    std::to_string(dstAccess));
            },
            .bindVertexBuffer = [](void* user, VkCommandBuffer, VkBuffer buffer,
                VkDeviceSize offset) {
                Recorder::of(user).add("vb " + std::to_string(id(buffer)) + "@" +
                    std::to_string(offset));
            },
            .bindIndexBuffer = [](void* user, VkCommandBuffer, VkBuffer buffer,
                VkDeviceSize offset, VkIndexType type) {
                Recorder::of(user).add("ib " + std::to_string(id(buffer)) + "@" +
                    std::to_string(offset) + " t" + std::to_string(type));
            },
            .drawIndexedIndirectCount = [](void* user, VkCommandBuffer,
                VkBuffer commands, VkDeviceSize commandOffset, VkBuffer counts,
                VkDeviceSize countOffset, uint32_t maxDraws, uint32_t stride) {
                Recorder::of(user).add("draw " + std::to_string(id(commands)) + "@" +
                    std::to_string(commandOffset) + " " + std::to_string(id(counts)) +
                    "@" + std::to_string(countOffset) + " max=" +
                    std::to_string(maxDraws) + " stride=" + std::to_string(stride));
            },
            .beginGpuRange = [](void* user, const char* name) {
                Recorder::of(user).add(std::string("range ") + name);
                return VulkanGpuRangeToken{ 0u, 0u, true };
            },
            .endGpuRange = [](void* user, VulkanGpuRangeToken& token) {
                Recorder::of(user).add("end");
                token.active = false;
            },
        };
    }

    VulkanCullerResources fakeResources(Recorder& recorder) {
        return {
            .user = &recorder,
            .createBuffer = [](void* user, VkDeviceSize bytes, VkBufferUsageFlags) {
                Recorder& r = Recorder::of(user);
                r.memory.push_back(std::make_unique<std::vector<std::byte>>(
                    static_cast<size_t>(bytes), std::byte{ 0 }));
                VulkanBufferResource buffer{};
                buffer.buffer = fake<VkBuffer>(r.nextHandle++);
                buffer.memory = fake<VkDeviceMemory>(r.nextHandle++);
                buffer.size = bytes;
                buffer.mapped = r.memory.back()->data();
                return buffer;
            },
            .destroyBuffer = [](void*, VulkanBufferResource& buffer) { buffer = {}; },
            .allocateSet = [](void* user, VkDescriptorSetLayout) {
                return fake<VkDescriptorSet>(Recorder::of(user).nextHandle++);
            },
            .freeSet = [](void*, VkDescriptorSet) {},
            .writeStorageBuffers = [](void* user, VkDescriptorSet, uint32_t,
                std::span<const VkDescriptorBufferInfo>) {
                ++Recorder::of(user).descriptorWrites;
            },
            .writeCombinedImageSampler = [](void* user, VkDescriptorSet, uint32_t,
                VkSampler, VkImageView, VkImageLayout) {
                ++Recorder::of(user).descriptorWrites;
            },
            .slotInFlight = [](void* user, uint32_t slot) {
                return Recorder::of(user).inFlight[slot];
            },
            .retireBuffer = [](void* user, const VulkanBufferResource& buffer) {
                Recorder::of(user).retired.push_back(buffer.buffer);
            },
        };
    }

    // ---------------------------------------------------------------------
    // Fake GPU scene.
    //
    // Geometries (index = handle id - 1):
    //   g0 arena A u32  first 0   count 30 vo 0   -> LOD child g4
    //   g1 arena A u32  first 30  count 60 vo 100
    //   g2 arena B u32  first 0   count 90 vo 0
    //   g3 arena B, own u16 index buffer, first 0 count 12 vo 0
    //   g4 arena A u32  first 90  count 15 vo 200 -> g5 (buffer change: ends LOD)
    //   g5 arena B u32  first 300 count 6  vo 300
    // Materials: 1 opaque single-sided, 2 alpha-masked, 3 double-sided.
    // Primitives p0..p9 (geometry, material):
    //   p0 g0 m1, p1 g1 m1, p2 g2 m1, p3 g0 m2, p4 g2 m3,
    //   p5 g3 m1, p6 g1 m2, p7 g2 m1, p8 g0 m1, p9 g3 m3
    // ---------------------------------------------------------------------
    constexpr uint64_t VertexA = 0xA0, IndexA = 0xA1;
    constexpr uint64_t VertexB = 0xB0, IndexB = 0xB1, IndexB16 = 0xB2;
    constexpr uint32_t PrimitiveCount = 10;

    struct GeometrySpec {
        uint64_t vertexBuffer, indexBuffer;
        IndexFormat format;
        uint32_t firstIndex, indexCount, vertexOffset, next;
    };
    constexpr std::array<GeometrySpec, 6> Geometries{ {
        { VertexA, IndexA, IndexFormat::UInt32, 0, 30, 0, 4 },
        { VertexA, IndexA, IndexFormat::UInt32, 30, 60, 100, InvalidGpuSceneIndex },
        { VertexB, IndexB, IndexFormat::UInt32, 0, 90, 0, InvalidGpuSceneIndex },
        { VertexB, IndexB16, IndexFormat::UInt16, 0, 12, 0, InvalidGpuSceneIndex },
        { VertexA, IndexA, IndexFormat::UInt32, 90, 15, 200, 5 },
        { VertexB, IndexB, IndexFormat::UInt32, 300, 6, 300, InvalidGpuSceneIndex },
    } };
    constexpr std::array<std::array<uint32_t, 2>, PrimitiveCount> Primitives{ {
        { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 2 }, { 2, 3 },
        { 3, 1 }, { 1, 2 }, { 2, 1 }, { 0, 1 }, { 3, 3 },
    } };

    struct FakeScene {
        std::vector<GpuSceneAffineTransform> transforms;
        std::vector<GpuSceneInstanceRecord> instances;
        std::vector<GpuScenePrimitiveRecord> primitives;
        std::vector<GpuSceneGeometryRecord> geometries;
        std::vector<GpuScenePrimitiveIdentity> identities;
        std::vector<uint32_t> indices;

        FakeScene() {
            constexpr uint32_t consumers = GpuSceneConsumerMainOpaque |
                GpuSceneConsumerShadow | GpuSceneConsumerProbe;
            for (uint32_t index = 0; index < PrimitiveCount; ++index) {
                GpuSceneAffineTransform transform{};
                transform.row0 = { 1.0f, 0.0f, 0.0f, static_cast<float>(index) };
                transform.row1 = { 0.0f, 1.0f, 0.0f, 0.0f };
                transform.row2 = { 0.0f, 0.0f, 1.0f, 0.0f };
                transforms.push_back(transform);
                GpuSceneInstanceRecord instance{};
                instance.worldBoundsSphere = { static_cast<float>(index), 0.0f,
                    0.0f, 1.0f };
                instance.references = { index, InvalidGpuSceneIndex, 0u, 0u };
                instance.state = { 0u, 0u,
                    packGpuSceneInstanceFlags(GpuSceneInstanceEnabled, 2u),
                    consumers };
                instances.push_back(instance);
                GpuScenePrimitiveRecord primitive{};
                primitive.binding = { index, Primitives[index][0],
                    Primitives[index][1], 1u };
                primitive.state = { 0u, 0u, InvalidGpuSceneIndex, consumers };
                primitives.push_back(primitive);
                identities.push_back({});
                indices.push_back(index);
            }
            for (uint32_t index = 0; index < Geometries.size(); ++index) {
                const GeometrySpec& spec = Geometries[index];
                GpuSceneGeometryRecord geometry{};
                geometry.draw = { spec.firstIndex, spec.indexCount,
                    spec.vertexOffset, static_cast<uint32_t>(spec.format) };
                geometry.storage = { index + 1u, InvalidGpuSceneIndex, 0u,
                    GpuSceneGeometryLegacyRhiHandle };
                geometry.state = { 0u, InvalidGpuSceneIndex, spec.next, 0u };
                geometry.localBoundsMax.w = static_cast<float>(index);
                geometries.push_back(geometry);
            }
        }

        [[nodiscard]] VulkanIndirectScene view() const {
            return {
                .transforms = transforms,
                .instances = instances,
                .primitives = primitives,
                .geometries = geometries,
                .identities = identities,
                .published = {
                    .transforms = PrimitiveCount,
                    .instances = PrimitiveCount,
                    .primitives = PrimitiveCount,
                    .geometries = static_cast<uint32_t>(Geometries.size()),
                },
            };
        }
    };

    VulkanIndirectAssetResolver fakeAssets() {
        return {
            .owner = nullptr,
            .geometry = [](const void*, GeometryHandle handle,
                VulkanIndirectGeometry& geometry) {
                if (handle.id == 0u || handle.id > Geometries.size()) return false;
                const GeometrySpec& spec = Geometries[handle.id - 1u];
                geometry = { fake<VkBuffer>(spec.vertexBuffer),
                    fake<VkBuffer>(spec.indexBuffer), spec.format,
                    static_cast<VkDeviceSize>(spec.vertexOffset) * sizeof(Vertex) };
                return true;
            },
            .material = [](const void*, MaterialHandle handle,
                VulkanIndirectMaterial& material) {
                if (handle.id == 0u || handle.id > 3u) return false;
                material = { handle.id == 2u ? 1u : 0u, handle.id == 3u ? 1u : 0u };
                return true;
            },
            .gbufferIndirectPipeline = [](const void*, PipelineHandle handle) {
                return handle.id == 1u;
            },
        };
    }

    struct FakeServicesContext {
        Recorder recorder;
        FakeScene scene;
    };

    VulkanCullerServices fakeServices(FakeServicesContext& context,
        IVulkanIndirectOracle* oracle = nullptr) {
        return {
            .commands = loggingCommands(context.recorder),
            .resources = fakeResources(context.recorder),
            .capabilities = { true, true, true, 1u << 20 },
            .oracle = oracle,
            .sceneOwner = &context.scene,
            .scene = [](const void* owner, uint32_t) {
                return static_cast<const FakeScene*>(owner)->view();
            },
            .maximumPrimitiveCapacity = 65536u,
        };
    }

    // ---------------------------------------------------------------------
    // Expected shadow/probe binning of p0..p9 (first-seen (vertex, index,
    // index type, alpha, double-sided) bins; stable within each bin).
    //   bin0 A/u32 single  p0 p1 p8   base 0
    //   bin1 B/u32 single  p2 p7      base 3
    //   bin2 A/u32 alpha   p3 p6      base 5
    //   bin3 B/u32 double  p4         base 7
    //   bin4 B/u16 single  p5         base 8
    //   bin5 B/u16 double  p9         base 9
    // ---------------------------------------------------------------------
    constexpr uint32_t BinCount = 6;
    constexpr std::array<uint32_t, PrimitiveCount> CandidateOrder{
        0, 1, 8, 2, 7, 3, 6, 4, 5, 9 };
    constexpr std::array<uint32_t, PrimitiveCount> CandidateBin{
        0, 0, 0, 1, 1, 2, 2, 3, 4, 5 };
    constexpr std::array<uint32_t, BinCount> BinBase{ 0, 3, 5, 7, 8, 9 };
    constexpr std::array<uint32_t, BinCount> BinSize{ 3, 2, 2, 1, 1, 1 };

    std::vector<GpuSceneIndirectCandidate> expectedCandidates(bool lod) {
        std::vector<GpuSceneIndirectCandidate> result;
        for (uint32_t slot = 0; slot < PrimitiveCount; ++slot) {
            const uint32_t primitive = CandidateOrder[slot];
            const uint32_t bin = CandidateBin[slot];
            // g0 has one resident buffer-compatible child (g4); g4->g5
            // changes vertex buffer and ends the prefix.
            const uint32_t maximumLod =
                lod && Primitives[primitive][0] == 0u ? 1u : 0u;
            result.push_back({ .primitiveIndex = primitive, .binIndex = bin,
                .commandBase = BinBase[bin], .commandCapacity = BinSize[bin],
                .maximumLod = maximumLod });
        }
        return result;
    }

    bool sameBytes(const void* actual, const void* expected, size_t bytes) {
        return std::memcmp(actual, expected, bytes) == 0;
    }

    std::string pushLine(uint64_t layout, std::initializer_list<uint32_t> values) {
        std::ostringstream out;
        out << "push L" << layout << " stages=" << VK_SHADER_STAGE_COMPUTE_BIT
            << " off=0 [";
        bool first = true;
        for (const uint32_t value : values) {
            out << (first ? "" : ",") << value;
            first = false;
        }
        out << "]";
        return out.str();
    }

    const std::string HostBarrier = "barrier " +
        std::to_string(VK_PIPELINE_STAGE_HOST_BIT) + "->" +
        std::to_string(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) + " " +
        std::to_string(VK_ACCESS_HOST_WRITE_BIT) + "->" +
        std::to_string(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const std::string DrawBarrier = "barrier " +
        std::to_string(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) + "->" +
        std::to_string(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) + " " +
        std::to_string(VK_ACCESS_SHADER_WRITE_BIT) + "->" +
        std::to_string(VK_ACCESS_INDIRECT_COMMAND_READ_BIT);

    struct ViewRig {
        FakeServicesContext context;
        VulkanIndirectViewCuller culler;
        VulkanCompactPipeline pipeline{ fake<VkPipelineLayout>(0x51),
            fake<VkPipeline>(0x52) };

        explicit ViewRig(IndirectViewKind kind, IVulkanIndirectOracle* oracle = nullptr) {
            culler.init(fakeServices(context, oracle), kind, pipeline,
                fake<VkDescriptorSetLayout>(0x50), oracle);
            culler.resize(64u, false);
            context.recorder.log.clear();
        }
        ~ViewRig() { culler.destroy(VK_NULL_HANDLE); }

        [[nodiscard]] IndirectViewInputs inputs(float lod = 0.0f,
            uint64_t revision = 7u) const {
            return {
                .primitiveIndices = context.scene.indices,
                .membershipRevision = revision,
                .lodErrorThreshold = lod,
                .lodMaximumLevel = 15u,
                .scene = context.scene.view(),
                .assets = fakeAssets(),
            };
        }
        [[nodiscard]] const void* candidateBytes(uint32_t frame) const {
            return culler.buffers().candidates[frame].mapped;
        }
        [[nodiscard]] const uint32_t* counts(uint32_t frame) const {
            return static_cast<const uint32_t*>(culler.buffers().counts[frame].mapped);
        }
        [[nodiscard]] uint64_t set(uint32_t frame) const;
    };

    // The indirect sets are the first handles the recorder allocated.
    uint64_t ViewRig::set(uint32_t frame) const { return 0x1000u + frame; }

    // ---------------------------------------------------------------------
    // Tests.
    // ---------------------------------------------------------------------
    bool testDirectionalStream() {
        ViewRig rig(IndirectViewKind::DirectionalShadow);
        // Packet 0: light 1, cascades 0 and 2 (layers 4, 6). Packet 1: light 0,
        // cascade 1 (layer 1). Work follows packets; dispatches follow layers.
        std::array<DirectionalShadowFramePacket, 2> shadows{};
        shadows[0].shadowIndex = 1u;
        shadows[0].updateMask = 0b0101u;
        shadows[1].shadowIndex = 0u;
        shadows[1].updateMask = 0b0010u;
        // Bytes past the 18 count slots must stay untouched.
        std::memset(rig.culler.buffers().counts[0].mapped, 0xff,
            static_cast<size_t>(rig.culler.buffers().counts[0].size));
        CHECK(rig.culler.plan(rig.inputs(), directionalShadowWork(shadows), 0u));
        CHECK(rig.context.recorder.log.empty()); // planning records nothing
        CHECK(rig.culler.fallbackReason() == GpuSceneIndirectFallbackReason::None);
        CHECK(!rig.culler.membershipCacheHit());
        CHECK(rig.culler.bins().size() == BinCount);
        CHECK(rig.culler.workIndex(4u) == 0u && rig.culler.workIndex(6u) == 1u &&
            rig.culler.workIndex(1u) == 2u && rig.culler.workIndex(0u) ==
                InvalidGpuSceneIndex);
        const auto candidates = expectedCandidates(false);
        CHECK(sameBytes(rig.candidateBytes(0), candidates.data(),
            candidates.size() * sizeof(GpuSceneIndirectCandidate)));
        for (uint32_t index = 0; index < BinCount * 3u; ++index)
            CHECK(rig.counts(0)[index] == 0u);
        CHECK(rig.counts(0)[BinCount * 3u] == 0xffffffffu);

        const uint32_t dispatches = rig.culler.recordCompaction(nullptr, 0u, {
            .set0 = fake<VkDescriptorSet>(0x60), .gpuScene = fake<VkDescriptorSet>(0x61) });
        CHECK(dispatches == 3u);
        const std::vector<std::string> expected{
            HostBarrier,
            "range gpu.shadow.directional.compact",
            "pipe 1 " + std::to_string(0x52),
            "sets 1 L" + std::to_string(0x51) + " first=0 [96,97," +
                std::to_string(rig.set(0)) + "] dyn=[]",
            pushLine(0x51, { 10, 10, 10, 10, 6, 1, 2 * 6, 2 * 10, 0 }),
            "dispatch 1 1 1",
            pushLine(0x51, { 10, 10, 10, 10, 6, 4, 0, 0, 0 }),
            "dispatch 1 1 1",
            pushLine(0x51, { 10, 10, 10, 10, 6, 6, 6, 10, 0 }),
            "dispatch 1 1 1",
            "end",
            // R3b.7: the compute -> indirect barrier is the graph executor's.
        };
        CHECK(rig.context.recorder.log == expected);

        // Draws of work item 0 (layer 4): pipeline per (alpha, sidedness),
        // vertex/index buffers per bin, slot push, count/command regions.
        rig.context.recorder.log.clear();
        const uint32_t bins = rig.culler.recordDraws(nullptr, 0u, 0u, {
            .layout = fake<VkPipelineLayout>(0x70),
            .pipeline = [](const void*, bool alpha, bool doubleSided) {
                return fake<VkPipeline>(0x80u + (alpha ? 1u : 0u) +
                    (doubleSided ? 2u : 0u));
            },
            .pushSlotWord = true,
            .slotWord = 4u,
        });
        CHECK(bins == BinCount);
        const uint64_t commandBuffer = id(rig.culler.buffers().commands[0].buffer);
        const uint64_t countBuffer = id(rig.culler.buffers().counts[0].buffer);
        const auto draw = [&](uint32_t bin) {
            return "draw " + std::to_string(commandBuffer) + "@" +
                std::to_string(BinBase[bin] * 20u) + " " +
                std::to_string(countBuffer) + "@" + std::to_string(bin * 4u) +
                " max=" + std::to_string(BinSize[bin]) + " stride=20";
        };
        const std::string push = "push L" + std::to_string(0x70) + " stages=" +
            std::to_string(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) +
            " off=0 mesh slot=4 material=0";
        const std::string u32 = " t" + std::to_string(VK_INDEX_TYPE_UINT32);
        const std::string u16 = " t" + std::to_string(VK_INDEX_TYPE_UINT16);
        const std::vector<std::string> expectedDraws{
            "pipe 0 128", "vb 160@0", "ib 161@0" + u32, push, draw(0),
            "vb 176@0", "ib 177@0" + u32, push, draw(1),
            "pipe 0 129", "vb 160@0", "ib 161@0" + u32, push, draw(2),
            "pipe 0 130", "vb 176@0", "ib 177@0" + u32, push, draw(3),
            "pipe 0 128", "vb 176@0", "ib 178@0" + u16, push, draw(4),
            "pipe 0 130", "vb 176@0", "ib 178@0" + u16, push, draw(5),
        };
        CHECK(rig.context.recorder.log == expectedDraws);

        // The slot is in flight until collected.
        bool rejected = false;
        try { (void)rig.culler.plan(rig.inputs(), directionalShadowWork(shadows), 0u); }
        catch (const std::logic_error& error) {
            rejected = std::string(error.what()) ==
                "directional-shadow indirect validation slot is still in flight";
        }
        CHECK(rejected);
        rig.culler.collect(0u);
        CHECK(rig.culler.plan(rig.inputs(), directionalShadowWork(shadows), 0u));
        rig.culler.collect(0u);
        return true;
    }

    bool testMembershipCacheAndRejects() {
        ViewRig rig(IndirectViewKind::DirectionalShadow);
        std::array<DirectionalShadowFramePacket, 1> shadows{};
        shadows[0].updateMask = 0b0001u;
        CHECK(rig.culler.plan(rig.inputs(), directionalShadowWork(shadows), 0u));
        // Same revision, LOD key and level on the next slot: cached bins,
        // identical host bytes.
        CHECK(rig.culler.plan(rig.inputs(), directionalShadowWork(shadows), 1u));
        CHECK(rig.culler.membershipCacheHit());
        const auto candidates = expectedCandidates(false);
        CHECK(sameBytes(rig.candidateBytes(1), candidates.data(),
            candidates.size() * sizeof(GpuSceneIndirectCandidate)));
        rig.culler.collect(0u);
        rig.culler.collect(1u);
        // A new LOD key rebuilds with the resident prefix.
        CHECK(rig.culler.plan(rig.inputs(2.0f), directionalShadowWork(shadows), 0u));
        CHECK(!rig.culler.membershipCacheHit());
        const auto lodCandidates = expectedCandidates(true);
        CHECK(sameBytes(rig.candidateBytes(0), lodCandidates.data(),
            lodCandidates.size() * sizeof(GpuSceneIndirectCandidate)));
        rig.context.recorder.log.clear();
        (void)rig.culler.recordCompaction(nullptr, 0u, {});
        // Host barrier, range, pipeline, sets, push, dispatch, end (R3b.7:
        // no in-pass compute -> indirect barrier).
        CHECK(rig.context.recorder.log.size() == 7u);
        CHECK(rig.context.recorder.log[4] == pushLine(0x51,
            { 10, 10, 10, 10, 6, 0, 0, 0, std::bit_cast<uint32_t>(2.0f) }));
        rig.culler.collect(0u);
        rig.context.recorder.log.clear();

        // Reject ladder: nothing is recorded and the reason is reported.
        auto tiny = rig.inputs();
        tiny.primitiveIndices = std::span(rig.context.scene.indices).first(7u);
        CHECK(!rig.culler.plan(tiny, directionalShadowWork(shadows), 0u));
        CHECK(rig.culler.fallbackReason() == GpuSceneIndirectFallbackReason::TinyWorkload);
        auto direct = rig.inputs();
        direct.forceDirectShadowReference = true;
        CHECK(!rig.culler.plan(direct, directionalShadowWork(shadows), 0u));
        CHECK(rig.culler.fallbackReason() == GpuSceneIndirectFallbackReason::DirectReference);
        std::array<DirectionalShadowFramePacket, 2> duplicate{};
        duplicate[0].updateMask = duplicate[1].updateMask = 0b0001u;
        CHECK(!rig.culler.plan(rig.inputs(), directionalShadowWork(duplicate), 0u));
        CHECK(rig.culler.fallbackReason() == GpuSceneIndirectFallbackReason::InvalidPacket);
        std::array<DirectionalShadowFramePacket, 1> idle{};
        CHECK(!rig.culler.plan(rig.inputs(), directionalShadowWork(idle), 0u));
        CHECK(rig.culler.fallbackReason() == GpuSceneIndirectFallbackReason::CapacityExceeded);
        CHECK(rig.context.recorder.log.empty());

        // R4c.2 growth without a drain: an idle slot is collected, swapped
        // and rebound at once; an in-flight slot keeps its buffers until its
        // retirement. A second growth before then replaces the parked set.
        const uint32_t writes = rig.context.recorder.descriptorWrites;
        const VkBuffer idleCommands = rig.culler.buffers().commands[0].buffer;
        const VkBuffer busyCommands = rig.culler.buffers().commands[1].buffer;
        const VkDeviceSize commandBytes = rig.culler.buffers().commands[0].size;
        rig.context.recorder.inFlight = { false, true };
        rig.culler.resize(128u, false);
        CHECK(rig.context.recorder.descriptorWrites == writes + 1u);
        CHECK(rig.culler.primitiveCapacity() == 128u);
        CHECK(rig.culler.buffers().commands[0].buffer != idleCommands);
        CHECK(rig.culler.buffers().commands[0].size == commandBytes * 2u);
        CHECK(rig.culler.buffers().commands[1].buffer == busyCommands);
        CHECK(!rig.culler.slotSwapPending(0u) && rig.culler.slotSwapPending(1u));
        rig.culler.resize(256u, false);
        CHECK(rig.culler.buffers().commands[1].buffer == busyCommands);
        CHECK(rig.context.recorder.descriptorWrites == writes + 2u);
        CHECK(!rig.culler.swapRetiredSlot(0u));
        CHECK(rig.culler.swapRetiredSlot(1u));
        CHECK(rig.culler.buffers().commands[1].buffer != busyCommands);
        CHECK(rig.culler.buffers().commands[1].size == commandBytes * 4u);
        CHECK(rig.context.recorder.descriptorWrites == writes + 3u);
        CHECK(!rig.culler.slotSwapPending(1u));
        CHECK(!rig.culler.swapRetiredSlot(1u));
        CHECK(rig.context.recorder.retired.empty());
        return true;
    }

    bool testSpotAndPointStreams() {
        {
            ViewRig rig(IndirectViewKind::SpotShadow);
            std::array<SpotShadowFramePacket, 3> shadows{};
            shadows[0].shadowDataSlot = 3u; shadows[0].update = true;
            shadows[1].shadowDataSlot = 7u; shadows[1].update = false;
            shadows[2].shadowDataSlot = 1u; shadows[2].update = true;
            CHECK(rig.culler.plan(rig.inputs(), spotShadowWork(shadows), 1u));
            for (uint32_t index = 0; index < BinCount * 2u; ++index)
                CHECK(rig.counts(1)[index] == 0u);
            CHECK(rig.culler.recordCompaction(nullptr, 1u, {
                .set0 = fake<VkDescriptorSet>(0x60),
                .gpuScene = fake<VkDescriptorSet>(0x61) }) == 2u);
            const std::vector<std::string> expected{
                HostBarrier,
                "range gpu.shadow.spot.compact",
                "pipe 1 " + std::to_string(0x52),
                "sets 1 L" + std::to_string(0x51) + " first=0 [96,97," +
                    std::to_string(rig.set(1)) + "] dyn=[]",
                pushLine(0x51, { 10, 10, 10, 10, 6, 3, 0, 0, 0 }),
                "dispatch 1 1 1",
                pushLine(0x51, { 10, 10, 10, 10, 6, 1, 6, 10, 0 }),
                "dispatch 1 1 1",
                "end",
            };
            CHECK(rig.context.recorder.log == expected);
            rig.culler.collect(1u);
            shadows[2].shadowDataSlot = 3u;
            CHECK(!rig.culler.plan(rig.inputs(), spotShadowWork(shadows), 1u));
            CHECK(rig.culler.fallbackReason() ==
                GpuSceneIndirectFallbackReason::InvalidPacket);
        }
        {
            ViewRig rig(IndirectViewKind::PointShadow);
            std::array<PointShadowFramePacket, 1> shadows{};
            shadows[0].shadowDataSlot = 2u;
            shadows[0].update = true;
            shadows[0].resolution = 512u;
            CHECK(rig.culler.plan(rig.inputs(), pointShadowWork(shadows), 0u));
            CHECK(rig.culler.recordCompaction(nullptr, 0u, {}) == 6u);
            const auto& log = rig.context.recorder.log;
            CHECK(log.size() == 4u + 12u + 1u);
            CHECK(log[1] == "range gpu.shadow.point.compact");
            for (uint32_t face = 0; face < 6u; ++face) {
                CHECK(log[4u + face * 2u] == pushLine(0x51, { 10, 10, 10, 10, 6,
                    12u + face, face * 6u, face * 10u, 0, 512 }));
                CHECK(log[5u + face * 2u] == "dispatch 1 1 1");
            }
            CHECK(rig.culler.workIndex(17u) == 5u);
            rig.culler.collect(0u);
        }
        return true;
    }

    bool testProbePerWorkItem() {
        ViewRig rig(IndirectViewKind::ReflectionProbe);
        std::array<ReflectionProbeCaptureScheduleEntry, 2> captures{};
        captures[0].scheduledFaceMask = 0b000101u;
        captures[1].scheduledFaceMask = 0b100000u;
        // The probe ignores the shadow-only direct reference.
        auto inputs = rig.inputs(8.0f, 0u);
        inputs.forceDirectShadowReference = true;
        CHECK(rig.culler.plan(inputs, reflectionProbeWork(captures), 0u));
        const auto candidates = expectedCandidates(true);
        CHECK(sameBytes(rig.candidateBytes(0), candidates.data(),
            candidates.size() * sizeof(GpuSceneIndirectCandidate)));
        // Prepare records only the host barrier; faces dispatch one by one.
        CHECK(rig.culler.recordCompaction(nullptr, 0u, {}) == 0u);
        CHECK(rig.context.recorder.log == std::vector<std::string>{ HostBarrier });
        rig.context.recorder.log.clear();
        CHECK(rig.culler.recordWorkItem(nullptr, 0u, {
            .set0 = fake<VkDescriptorSet>(0x90), .set0DynamicOffset = 256u,
            .gpuScene = fake<VkDescriptorSet>(0x61) }, { 7u, 2u, 0u }) == 1u);
        const std::vector<std::string> expected{
            "pipe 1 " + std::to_string(0x52),
            "sets 1 L" + std::to_string(0x51) + " first=0 [144] dyn=[256]",
            "sets 1 L" + std::to_string(0x51) + " first=1 [97," +
                std::to_string(rig.set(0)) + "] dyn=[]",
            pushLine(0x51, { 10, 10, 10, 10, 6, 7, 2 * 6, 2 * 10,
                std::bit_cast<uint32_t>(8.0f), 0 }),
            "dispatch 1 1 1",
            DrawBarrier,
        };
        CHECK(rig.context.recorder.log == expected);
        rig.culler.collect(0u);
        // 30 scheduled faces exceed the pass's 24 face records.
        std::array<ReflectionProbeCaptureScheduleEntry, 5> many{};
        for (auto& capture : many) capture.scheduledFaceMask = 0b111111u;
        CHECK(!rig.culler.plan(inputs, reflectionProbeWork(many), 0u));
        CHECK(rig.culler.fallbackReason() ==
            GpuSceneIndirectFallbackReason::CapacityExceeded);
        return true;
    }

    // Records shadow-command expectations; every other oracle entry point is
    // unused by the view culler.
    class ExpectationOracle final : public IVulkanIndirectOracle {
    public:
        struct Expectation {
            VulkanIndirectOracleView view;
            uint32_t slot;
            size_t countIndex;
            GpuSceneIndexedIndirectCommand command;
        };
        std::vector<Expectation> expectations;
        size_t begun = 0;

        bool enabled(VulkanIndirectOracleView) const noexcept override { return true; }
        void beginShadowWork(VulkanIndirectOracleView, uint32_t, size_t regions) override {
            begun = regions;
        }
        void expectShadowCommand(VulkanIndirectOracleView view, uint32_t slot,
            size_t countIndex, const GpuSceneIndexedIndirectCommand& command) override {
            expectations.push_back({ view, slot, countIndex, command });
        }
        VulkanIndirectOracleResult verifyShadowWork(VulkanIndirectOracleView,
            uint32_t, const VulkanIndirectReadback&) override {
            return { .validated = true };
        }
        void beginOpaqueWork(uint32_t, uint32_t, uint32_t, bool, bool) override {}
        void expectOpaqueVisibleCandidate(uint32_t, uint32_t) override {}
        void expectOpaqueOcclusionQuery(uint32_t, uint32_t) override {}
        void expectOpaqueProjectionRejected(uint32_t) override {}
        void expectOpaqueLod(uint32_t, uint32_t, const VulkanOpaqueLodExpectation&) override {}
        void verifyOpaqueLodCommands(uint32_t, const uint32_t*,
            std::span<const uint32_t>, const GpuSceneIndexedIndirectCommand*) override {}
        VulkanOcclusionQueryVerdict verifyOcclusionQueries(uint32_t,
            const DepthPyramidDeviceResult*) override { return {}; }
        bool opaqueCandidateCpuVisible(uint32_t, uint32_t) const noexcept override {
            return true;
        }
        bool unsafeGpuSceneOcclusion(uint32_t, uint32_t) const noexcept override {
            return false;
        }
        bool rejectOpaqueLodPrimitive(uint32_t, uint32_t) override { return true; }
        VulkanOpaqueLodVerdict finishOpaqueLod(uint32_t) override { return {}; }
        bool beginVirtualShadowVerify(const VulkanVirtualShadowOracleInput&) override {
            return false;
        }
        void verifyVirtualShadowRequest(uint32_t, const VirtualShadowPageRequest&) override {}
        uint64_t comparedVirtualShadowRequests() const noexcept override { return 0; }
    };

    bool testExpectationEmission() {
        ExpectationOracle oracle;
        ViewRig rig(IndirectViewKind::SpotShadow, &oracle);
        std::array<SpotShadowFramePacket, 2> shadows{};
        shadows[0].shadowDataSlot = 0u; shadows[0].update = true;
        shadows[1].shadowDataSlot = 5u; shadows[1].update = true;
        CHECK(rig.culler.plan(rig.inputs(2.0f), spotShadowWork(shadows), 0u));
        CHECK(oracle.begun == BinCount * 2u);
        std::vector<VulkanResolvedCaster> casters(PrimitiveCount);
        for (uint32_t index = 0; index < PrimitiveCount; ++index) {
            CHECK(resolveIndirectCaster(rig.context.scene.view(), index,
                GpuSceneConsumerShadow, casters[index]));
            // As the backend's caster visitor does.
            casters[index].gpuScenePrimitiveIndex = index;
        }
        casters.push_back({}); // a direct caster is never expected
        std::vector<uint8_t> visible(casters.size(), 1u);
        visible[2] = 0u; // p2 culled on the CPU
        // The selector picks the coarsest resident LOD it is allowed.
        const IndirectLodMetric coarsest{ nullptr, [](const void*,
            const VulkanIndirectScene& scene, const GpuScenePrimitiveRecord& primitive,
            uint32_t maximumLod) {
            uint32_t geometry = primitive.binding.y;
            for (uint32_t lod = 0; lod < maximumLod; ++lod)
                geometry = scene.geometries[geometry].state.z;
            return geometry;
        } };
        rig.culler.emitExpectations(oracle, 0u, 1u, casters, visible, 1u, coarsest);
        CHECK(oracle.expectations.size() == PrimitiveCount - 1u);
        for (const auto& expectation : oracle.expectations) {
            const uint32_t primitive = expectation.command.firstInstance;
            const uint32_t base = Primitives[primitive][0];
            const uint32_t selected = base == 0u ? 4u : base; // g0 -> resident g4
            uint32_t bin = 0;
            for (uint32_t slot = 0; slot < PrimitiveCount; ++slot)
                if (CandidateOrder[slot] == primitive) bin = CandidateBin[slot];
            CHECK(primitive != 2u);
            CHECK(expectation.view == VulkanIndirectOracleView::SpotShadow);
            CHECK(expectation.countIndex == BinCount + bin); // work item 1
            CHECK(expectation.command.indexCount == Geometries[selected].indexCount);
            CHECK(expectation.command.firstIndex == Geometries[selected].firstIndex);
            CHECK(expectation.command.vertexOffset ==
                static_cast<int32_t>(Geometries[selected].vertexOffset));
        }
        rig.culler.collect(0u);
        return true;
    }

    bool testOpaqueStream() {
        FakeServicesContext context;
        VulkanOpaqueIndirectCuller culler;
        culler.init(fakeServices(context), {}, {
            .setLayout = fake<VkDescriptorSetLayout>(0x40),
            .layout = fake<VkPipelineLayout>(0x41),
            .cull = fake<VkPipeline>(0x42) });
        culler.resize(64u, false);
        culler.allocateSet(0u);
        culler.allocateSet(1u);
        culler.bindBuffers();
        context.recorder.log.clear();

        // Packets p0..p7 in queue order; bins are runs of equal pipeline,
        // material and vertex/index buffers: [p0 p1] [p2] [p3] [p4] [p5] [p6] [p7].
        std::vector<DrawPacket> queue(8u);
        for (uint32_t index = 0; index < queue.size(); ++index) {
            const GeometrySpec& geometry = Geometries[Primitives[index][0]];
            DrawPacket& packet = queue[index];
            packet.geometry = GeometryHandle{ Primitives[index][0] + 1u };
            packet.material = MaterialHandle{ Primitives[index][1] };
            packet.pipeline = PipelineHandle{ 1u };
            packet.indexCount = geometry.indexCount;
            packet.firstIndex = geometry.firstIndex;
            packet.firstInstanceTransform = index;
            packet.executionFlags = DrawPacketGpuScenePrimitive |
                DrawPacketCpuVisibilityOracle;
        }
        const ViewTransportRecord view{};
        CHECK(culler.plan({ .queue = queue, .scene = context.scene.view(),
            .sceneBuffersMapped = true, .assets = fakeAssets(), .view = &view }, 0u));
        constexpr std::array<uint32_t, 8> bin{ 0, 0, 1, 2, 3, 4, 5, 6 };
        constexpr std::array<uint32_t, 8> base{ 0, 0, 2, 3, 4, 5, 6, 7 };
        constexpr std::array<uint32_t, 8> size{ 2, 2, 1, 1, 1, 1, 1, 1 };
        CHECK(culler.bins().size() == 7u);
        std::vector<GpuSceneIndirectCandidate> expected;
        for (uint32_t index = 0; index < queue.size(); ++index)
            expected.push_back({ .primitiveIndex = index, .binIndex = bin[index],
                .commandBase = base[index], .commandCapacity = size[index] });
        CHECK(sameBytes(culler.buffers().candidates[0].mapped, expected.data(),
            expected.size() * sizeof(GpuSceneIndirectCandidate)));
        CHECK(context.recorder.log.empty());

        // R4c.2 growth while slot 1 is in flight: slot 0 swaps at once, the
        // shared LOD history is replaced for both slots and the old one goes
        // to the deletion queue; slot 1 swaps at its retirement.
        {
            const VkBuffer busyCommands = culler.buffers().commands[1].buffer;
            const uint32_t writes = context.recorder.descriptorWrites;
            context.recorder.inFlight = { false, true };
            culler.resize(128u, false);
            CHECK(context.recorder.retired.size() == 1u);
            CHECK(context.recorder.descriptorWrites == writes + 1u);
            CHECK(culler.commandCapacity() == 128u);
            CHECK(culler.buffers().commands[1].buffer == busyCommands);
            CHECK(culler.slotSwapPending(1u));
            CHECK(culler.swapRetiredSlot(1u));
            CHECK(culler.buffers().commands[1].buffer != busyCommands);
            CHECK(context.recorder.descriptorWrites == writes + 2u);
            context.recorder.inFlight = {};
            // Every slot idle: the replaced history is destroyed at once.
            culler.resize(256u, false);
            CHECK(context.recorder.retired.size() == 1u);
            CHECK(context.recorder.descriptorWrites == writes + 4u);
            CHECK(culler.plan({ .queue = queue, .scene = context.scene.view(),
                .sceneBuffersMapped = true, .assets = fakeAssets(), .view = &view }, 0u));
        }

        CHECK(culler.recordCompaction(nullptr, 0u, {
            .globalSet = fake<VkDescriptorSet>(0x30),
            .gpuSceneSet = fake<VkDescriptorSet>(0x31) }) == 1u);
        const auto& log = context.recorder.log;
        CHECK(log.size() == 8u);
        CHECK(log[0] == HostBarrier);
        CHECK(log[1] == "range gpu.gpu_scene.frustum_compact");
        CHECK(log[2] == "pipe 1 " + std::to_string(0x42));
        CHECK(log[3].starts_with("sets 1 L" + std::to_string(0x41) + " first=0 [48,49,"));
        CHECK(log[4] == pushLine(0x41, { 8, 10, 10, 10, 6, 0, 1, 0,
            std::bit_cast<uint32_t>(0.15f), 0, 0 }));
        CHECK(log[5] == "dispatch 1 1 1");
        CHECK(log[6] == "end");
        // R3b.7: only the compute -> host half stays in the pass; the graph
        // executor issues the compute -> indirect barrier at gbuffer.
        CHECK(log[7] == "barrier " +
            std::to_string(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) + "->" +
            std::to_string(VK_PIPELINE_STAGE_HOST_BIT) + " " +
            std::to_string(VK_ACCESS_SHADER_WRITE_BIT) + "->" +
            std::to_string(VK_ACCESS_HOST_READ_BIT));
        culler.collect(0u);

        // A pipeline without a GPU-scene variant falls the frame back.
        queue[3].pipeline = PipelineHandle{ 2u };
        CHECK(!culler.plan({ .queue = queue, .scene = context.scene.view(),
            .sceneBuffersMapped = true, .assets = fakeAssets(), .view = &view }, 0u));
        CHECK(!culler.plan({ .queue = queue, .scene = context.scene.view(),
            .sceneBuffersMapped = false, .assets = fakeAssets(), .view = &view }, 0u));
        CHECK(culler.indirectPlan().fallbackReason ==
            GpuSceneIndirectFallbackReason::InvalidPacket);
        culler.destroy(VK_NULL_HANDLE);
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Directional dispatch log, host bytes and draws", testDirectionalStream },
        { "Membership cache, LOD prefix, rejects and growth",
            testMembershipCacheAndRejects },
        { "Spot and point dispatch logs", testSpotAndPointStreams },
        { "Probe per-face work items", testProbePerWorkItem },
        { "Single expectation emission", testExpectationEmission },
        { "Opaque cull stream", testOpaqueStream },
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
