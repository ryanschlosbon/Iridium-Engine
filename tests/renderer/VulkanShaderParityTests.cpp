// Headless GPU/CPU parity: production GLSL evaluated on the device against the
// C++ reference implementations. Replaces source-text checks whose intent was
// "the shader implements the reference": SceneColorTests (scene_color.glsl),
// LightingReferenceTests (direct_lighting.glsl), StandardMaterialShadingTests
// (material_bsdf.glsl) and Stage3ArchitectureTests group 1 (the four
// *_compact.comp device LOD selectors against the CPU oracles).

#include "HeadlessComputeKernel.h"
#include "HeadlessVulkanDevice.h"
#include "TestHarness.h"

#include "material/StandardMaterialShading.h"
#include "renderer/color/SceneColor.h"
#include "renderer/lighting/LightExtractor.h"
#include "renderer/lighting/LightingReference.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneIndirect.h"
#include "renderer/rhi/GpuSceneLod.h"
#include "renderer/vulkan/VulkanDirectionalShadowMap.h"
#include "renderer/vulkan/VulkanPointShadowPools.h"
#include "renderer/vulkan/VulkanReflectionProbeCapturePass.h"
#include "renderer/vulkan/VulkanSpotShadowAtlas.h"
#include "scene/components/LightComponent.h"
#include "scene/components/RelationshipComponent.h"
#include "scene/components/TransformComponent.h"
#include "scene/systems/TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace {

    using namespace Iridium;
    using Iridium::Test::HeadlessComputeKernel;
    using Iridium::Test::HeadlessVulkanBuffer;
    using Iridium::Test::HeadlessVulkanDevice;

    std::unique_ptr<HeadlessVulkanDevice> gpu;

    constexpr VkBufferUsageFlags StorageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags UniformUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    // RAII host buffer filled from a span.
    struct Buffer {
        Buffer(VkDeviceSize size, VkBufferUsageFlags usage)
            : value(gpu->createHostBuffer((std::max)(size, VkDeviceSize{ 16 }), usage)) {}
        template<typename T>
        Buffer(std::span<const T> data, VkBufferUsageFlags usage)
            : Buffer(data.size_bytes(), usage) {
            std::memcpy(value.mapped, data.data(), data.size_bytes());
        }
        ~Buffer() { gpu->destroy(value); }
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        template<typename T> [[nodiscard]] const T* as() const {
            return static_cast<const T*>(value.mapped);
        }
        HeadlessVulkanBuffer value;
    };

    template<typename T>
    std::span<const std::byte> bytesOf(const T& value) {
        return std::as_bytes(std::span(&value, 1));
    }

    bool near(double actual, double expected, double relative, double absolute) {
        if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
        return std::abs(actual - expected) <=
            (std::max)(absolute, relative * std::abs(expected));
    }

    // --- scene_color.glsl vs SceneColor.h ------------------------------------
    bool testSceneColorParity() {
        std::vector<glm::vec4> inputs;
        for (const float r : { 0.0f, 0.18f, 1.0f, 4.0f, 16.0f })
            for (const float g : { 0.0f, 0.5f, 2.0f })
                for (const float b : { 0.02f, 1.0f, 8.0f, -0.1f })
                    inputs.emplace_back(r, g, b, 0.0f);
        const auto count = static_cast<uint32_t>(inputs.size());
        Buffer input(std::span<const glm::vec4>(inputs), StorageUsage);
        Buffer output(sizeof(glm::vec4) * 2u * count, StorageUsage);
        HeadlessComputeKernel kernel(*gpu, Iridium::Test::parityShader(
            "scene_color_parity_comp.spv"));
        kernel.dispatch({ { { 0, 0 }, input.value.buffer },
            { { 0, 1 }, output.value.buffer } }, bytesOf(count), (count + 63u) / 64u);
        const glm::vec4* results = output.as<glm::vec4>();
        for (uint32_t index = 0; index < count; ++index) {
            const Color::Rgb source{ inputs[index].x, inputs[index].y, inputs[index].z };
            const Color::Rgb ap1 = Color::linearSrgbToAcesCg(source);
            const Color::Rgb srgb = Color::acesCgToLinearSrgb(source);
            const glm::vec4 gpuAp1 = results[2u * index];
            const glm::vec4 gpuSrgb = results[2u * index + 1u];
            const double scale = (std::max)({ 1.0, std::abs(source.r),
                std::abs(source.g), std::abs(source.b) });
            for (int channel = 0; channel < 3; ++channel) {
                const double expectedAp1 = channel == 0 ? ap1.r : channel == 1 ? ap1.g : ap1.b;
                const double expectedSrgb = channel == 0 ? srgb.r : channel == 1 ? srgb.g : srgb.b;
                IRIDIUM_CHECK_MSG(near(gpuAp1[channel], expectedAp1, 0.0, 2.0e-6 * scale),
                    "AP1 sample " << index << " channel " << channel);
                IRIDIUM_CHECK_MSG(near(gpuSrgb[channel], expectedSrgb, 0.0, 4.0e-6 * scale),
                    "sRGB sample " << index << " channel " << channel);
            }
        }
        return gpu->validationErrors() == 0u;
    }

    // --- direct_lighting.glsl vs LightingReference.h --------------------------
    SceneEntityUuid lightUuid(uint32_t suffix) {
        std::string value = "019fb7d3-0520-7000-8000-000000000000";
        constexpr char digits[] = "0123456789abcdef";
        value[value.size() - 1] = digits[suffix & 0xfu];
        return *SceneEntityUuid::parse(value);
    }

    bool testDirectLightingParity() {
        SceneWorld world;
        struct Authored { LightType type; glm::vec3 position; glm::vec3 rotation; };
        const std::array authored{
            Authored{ LightType::Directional, { 0, 10, 0 }, { -60, 20, 0 } },
            Authored{ LightType::Point, { 1, 2, 3 }, { 0, 0, 0 } },
            Authored{ LightType::Spot, { -2, 4, 1 }, { -70, 10, 0 } },
        };
        std::vector<LightComponent> components;
        for (uint32_t index = 0; index < authored.size(); ++index) {
            const Entity entity = world.createEntity(lightUuid(index + 1));
            auto& transform = world.registry().addComponent<TransformComponent>(entity);
            transform.position = authored[index].position;
            transform.rotation = authored[index].rotation;
            auto& relationship =
                world.registry().addComponent<RelationshipComponent>(entity);
            relationship.siblingOrder = static_cast<int32_t>(index);
            auto& light = world.registry().addComponent<LightComponent>(entity);
            light.type = authored[index].type;
            light.colorLinearRec709 = { 1.0f, 0.6f, 0.3f };
            light.illuminanceLux = 80'000.0f;
            light.luminousIntensityCandela = 1'500.0f;
            light.rangeMeters = 12.0f;
            light.sourceRadiusMeters = 0.25f;
            light.innerConeDegrees = 15.0f;
            light.outerConeDegrees = 35.0f;
            components.push_back(light);
        }
        TransformSystem transforms;
        (void)transforms.update(world.registry());
        LightExtractor extractor;
        const LightingFramePacket packet = extractor.extract(world);
        IRIDIUM_CHECK(packet.stats.activeLightCount == authored.size());
        std::vector<PackedGpuLight> records(packet.records.begin(), packet.records.end());
        std::vector<uint32_t> componentOfSlot(records.size(), UINT32_MAX);
        std::vector<uint32_t> slots;
        for (uint32_t index = 0; index < authored.size(); ++index) {
            const auto slot = extractor.slotFor(lightUuid(index + 1));
            IRIDIUM_CHECK(slot.has_value() && *slot < records.size());
            componentOfSlot[*slot] = index;
            slots.push_back(*slot);
        }

        // Surface samples: inside, near the source radius, at the range edge
        // and outside it; across, at and beyond the spot cone.
        std::vector<glm::vec4> samples;
        std::vector<uint32_t> sampleLight;
        for (const uint32_t slot : slots) {
            const glm::vec3 lightPosition(records[slot].positionRange);
            const glm::vec3 emission = glm::normalize(glm::vec3(records[slot].directionOuterCos));
            for (const float distance : { 0.1f, 0.24f, 1.0f, 3.0f, 7.5f, 11.9f, 12.5f })
                for (const float angle : { 0.0f, 10.0f, 20.0f, 30.0f, 34.0f, 40.0f, 80.0f }) {
                    glm::vec3 axis = glm::normalize(glm::cross(emission, glm::vec3(0, 0, 1)));
                    if (!std::isfinite(axis.x)) axis = glm::vec3(1, 0, 0);
                    const glm::mat4 tilt = glm::rotate(glm::mat4(1.0f),
                        glm::radians(angle), axis);
                    const glm::vec3 direction = glm::vec3(tilt * glm::vec4(emission, 0.0f));
                    const glm::vec3 position = lightPosition + direction * distance;
                    samples.emplace_back(position, std::bit_cast<float>(slot));
                    samples.emplace_back(-direction, 0.0f);
                    sampleLight.push_back(slot);
                }
        }
        const auto count = static_cast<uint32_t>(sampleLight.size());
        Buffer lights(std::span<const PackedGpuLight>(records), StorageUsage);
        Buffer input(std::span<const glm::vec4>(samples), StorageUsage);
        Buffer output(sizeof(glm::vec4) * 2u * count, StorageUsage);
        HeadlessComputeKernel kernel(*gpu, Iridium::Test::parityShader(
            "direct_lighting_parity_comp.spv"));
        kernel.dispatch({ { { 0, 0 }, lights.value.buffer },
            { { 0, 1 }, input.value.buffer }, { { 0, 2 }, output.value.buffer } },
            bytesOf(count), (count + 63u) / 64u);

        const glm::vec4* results = output.as<glm::vec4>();
        namespace Reference = Iridium::LightingReference;
        uint32_t litSamples = 0, spotFalloffSamples = 0;
        for (uint32_t index = 0; index < count; ++index) {
            const PackedGpuLight& record = records[sampleLight[index]];
            const LightComponent& light = components[componentOfSlot[sampleLight[index]]];
            const glm::dvec3 chroma(record.colorIntensity);
            const glm::dvec3 position(samples[2u * index]);
            glm::dvec3 expected(0.0);
            if (light.type == LightType::Directional) {
                IRIDIUM_CHECK(record.colorIntensity.w == light.illuminanceLux);
                expected = chroma * Reference::directionalRadiance(1.0, light.illuminanceLux);
                const glm::dvec3 toLight = -glm::normalize(glm::dvec3(record.directionOuterCos));
                IRIDIUM_CHECK(glm::length(glm::dvec3(results[2u * index]) - toLight) < 1.0e-6);
            } else {
                const glm::dvec3 toLight = glm::dvec3(record.positionRange) - position;
                const double distance = glm::length(toLight);
                double cone = 1.0;
                if (light.type == LightType::Spot) {
                    const double cosine = glm::dot(-toLight / distance,
                        glm::normalize(glm::dvec3(record.directionOuterCos)));
                    cone = Reference::spotConeAttenuation(cosine,
                        std::cos(glm::radians(double(light.innerConeDegrees))),
                        std::cos(glm::radians(double(light.outerConeDegrees))));
                    spotFalloffSamples += cone > 0.0 && cone < 1.0;
                }
                IRIDIUM_CHECK(record.colorIntensity.w == light.luminousIntensityCandela);
                IRIDIUM_CHECK(record.positionRange.w == light.rangeMeters);
                expected = chroma * Reference::localRadiance(1.0,
                    light.luminousIntensityCandela, distance, light.rangeMeters,
                    light.sourceRadiusMeters, cone);
            }
            const glm::vec4 radiance = results[2u * index + 1u];
            const double scale = (std::max)({ expected.x, expected.y, expected.z, 1.0e-6 });
            for (int channel = 0; channel < 3; ++channel)
                IRIDIUM_CHECK_MSG(near(radiance[channel], expected[channel], 2.0e-4,
                    1.0e-5 * scale), "light sample " << index << " channel " << channel
                    << " gpu " << radiance[channel] << " reference " << expected[channel]);
            litSamples += expected.x > 0.0;
        }
        IRIDIUM_CHECK(litSamples > count / 3u);
        IRIDIUM_CHECK(spotFalloffSamples >= 4u);
        return gpu->validationErrors() == 0u;
    }

    // --- material_bsdf.glsl vs StandardMaterialShading.h -----------------------
    bool testBsdfParity() {
        std::vector<glm::vec4> samples;
        struct Inputs { glm::vec3 base, f0, f90; float metallic, roughness; glm::vec3 n, v, l; };
        std::vector<Inputs> cases;
        const glm::vec3 normal(0.0f, 0.0f, 1.0f);
        for (const float roughness : { 0.0f, 0.04f, 0.3f, 0.7f, 1.0f })
            for (const float metallic : { 0.0f, 0.5f, 1.0f })
                for (const float viewAngle : { 0.0f, 45.0f, 80.0f })
                    for (const float lightAngle : { 0.0f, 30.0f, 60.0f, 85.0f }) {
                        const float va = glm::radians(viewAngle);
                        const float la = glm::radians(lightAngle);
                        const glm::vec3 view(std::sin(va), 0.0f, std::cos(va));
                        const glm::vec3 light(-std::sin(la) * 0.6f, std::sin(la) * 0.8f,
                            std::cos(la));
                        cases.push_back({ { 0.8f, 0.4f, 0.2f }, { 0.04f, 0.05f, 0.06f },
                            { 1.0f, 0.9f, 0.8f }, metallic, roughness, normal, view,
                            glm::normalize(light) });
                    }
        for (const Inputs& value : cases) {
            samples.emplace_back(value.base, value.metallic);
            samples.emplace_back(value.f0, value.roughness);
            samples.emplace_back(value.f90, 0.0f);
            samples.emplace_back(value.n, 0.0f);
            samples.emplace_back(value.v, 0.0f);
            samples.emplace_back(value.l, 0.0f);
        }
        const auto count = static_cast<uint32_t>(cases.size());
        Buffer input(std::span<const glm::vec4>(samples), StorageUsage);
        Buffer output(sizeof(glm::vec4) * 2u * count, StorageUsage);
        HeadlessComputeKernel kernel(*gpu, Iridium::Test::parityShader("bsdf_parity_comp.spv"));
        kernel.dispatch({ { { 0, 0 }, input.value.buffer },
            { { 0, 1 }, output.value.buffer } }, bytesOf(count), (count + 63u) / 64u);
        const glm::vec4* results = output.as<glm::vec4>();
        for (uint32_t index = 0; index < count; ++index) {
            const Inputs& value = cases[index];
            const glm::vec3 expected = materialEvaluateStandardBrdf(value.base, value.f0,
                value.f90, value.metallic, value.roughness, value.n, value.v, value.l);
            for (int channel = 0; channel < 3; ++channel) {
                // Forward (standard) and deferred (canonical) entry points both
                // evaluate the CPU reference BSDF.
                IRIDIUM_CHECK_MSG(near(results[2u * index][channel], expected[channel],
                    2.0e-4, 1.0e-6), "standard " << index << " ch " << channel
                    << " gpu " << results[2u * index][channel] << " cpu " << expected[channel]);
                IRIDIUM_CHECK_MSG(near(results[2u * index + 1u][channel], expected[channel],
                    2.0e-4, 1.0e-6), "canonical " << index << " ch " << channel);
            }
        }
        return gpu->validationErrors() == 0u;
    }

    // --- *_compact.comp vs the CPU LOD selectors ------------------------------
    constexpr uint32_t ShadowConsumer = GpuSceneConsumerShadow;
    constexpr uint32_t ProbeConsumer = GpuSceneConsumerProbe;

    struct CompactScene {
        std::vector<GpuSceneAffineTransform> transforms;
        std::vector<GpuSceneInstanceRecord> instances;
        std::vector<GpuScenePrimitiveRecord> primitives;
        std::vector<GpuSceneGeometryRecord> geometries;
        std::vector<GpuSceneIndirectCandidate> candidates;
    };

    // Base geometry plus three coarser LODs (dense indices 0..3) and a second,
    // malformed chain (4..5) whose child breaks the triangle-count contract.
    void addGeometryChains(CompactScene& scene) {
        const std::array<float, 4> errors{ 0.0f, 0.01f, 0.04f, 0.16f };
        const std::array<uint32_t, 4> counts{ 3000u, 1500u, 600u, 300u };
        uint32_t firstIndex = 0;
        for (uint32_t lod = 0; lod < 4; ++lod) {
            GpuSceneGeometryRecord geometry{};
            geometry.localBoundsSphere = { 0.0f, 0.0f, 0.0f, std::sqrt(3.0f) };
            geometry.localBoundsMin = { -1.0f, -1.0f, -1.0f, errors[lod] };
            geometry.localBoundsMax = { 1.0f, 1.0f, 1.0f, static_cast<float>(lod) };
            geometry.draw = { firstIndex, counts[lod], 0u, 1u };
            geometry.storage = { 0u, 0u, 7u, 0u };
            geometry.state = { 1u, lod, lod < 3 ? lod + 1u : InvalidGpuSceneIndex, 0u };
            scene.geometries.push_back(geometry);
            firstIndex += counts[lod];
        }
        GpuSceneGeometryRecord brokenBase = scene.geometries[0];
        brokenBase.draw.x = 90'000u;
        brokenBase.state.z = 5u;
        GpuSceneGeometryRecord brokenChild = scene.geometries[1];
        brokenChild.draw = { 93'000u, 1501u, 0u, 1u }; // not a triangle multiple
        brokenChild.state.z = InvalidGpuSceneIndex;
        scene.geometries.push_back(brokenBase);
        scene.geometries.push_back(brokenChild);
    }

    void addInstance(CompactScene& scene, glm::vec3 center, float scale,
        uint32_t consumers, uint32_t primitiveConsumers, bool enabled,
        uint32_t maximumLod, uint32_t baseGeometry) {
        const auto index = static_cast<uint32_t>(scene.instances.size());
        const glm::mat4 world = glm::scale(glm::translate(glm::mat4(1.0f), center),
            glm::vec3(scale));
        scene.transforms.push_back(packGpuSceneAffine(world));
        GpuSceneInstanceRecord instance{};
        instance.worldBoundsSphere = { center.x, center.y, center.z, scale * std::sqrt(3.0f) };
        instance.worldBoundsMin = { center.x - scale, center.y - scale, center.z - scale, 0 };
        instance.worldBoundsMax = { center.x + scale, center.y + scale, center.z + scale, 0 };
        instance.references = { index, InvalidGpuSceneIndex, 0u, 0u };
        instance.state = { 1u, 0u, enabled ? uint32_t{ GpuSceneInstanceEnabled } : 0u,
            consumers };
        scene.instances.push_back(instance);
        GpuScenePrimitiveRecord primitive{};
        primitive.binding = { index, baseGeometry, 0u, 0u };
        primitive.state = { 1u, 0u, 0u, primitiveConsumers };
        scene.primitives.push_back(primitive);
        GpuSceneIndirectCandidate candidate{};
        candidate.primitiveIndex = index;
        candidate.binIndex = index;
        candidate.commandBase = index;
        candidate.commandCapacity = 1u;
        candidate.maximumLod = maximumLod;
        scene.candidates.push_back(candidate);
    }

    // Instances spread through a view volume at many scales and LOD caps, plus
    // consumer-mask, disabled-instance and malformed-chain cases.
    CompactScene buildScene(const std::function<glm::vec3(float, float, float)>& place) {
        CompactScene scene;
        addGeometryChains(scene);
        const std::array<float, 8> scales{ 0.2f, 0.35f, 0.6f, 1.0f, 1.7f, 3.0f, 5.0f, 8.0f };
        for (uint32_t index = 0; index < 64u; ++index) {
            const float u = static_cast<float>(index % 8u) / 7.0f;
            const float v = static_cast<float>((index / 8u) % 8u) / 7.0f;
            const float depth = static_cast<float>((index * 5u) % 64u) / 63.0f;
            const uint32_t maximumLod = index % 5u == 0u ? 0u : (index % 7u == 0u ? 2u : 15u);
            addInstance(scene, place(u, v, depth), scales[index % scales.size()],
                ShadowConsumer | ProbeConsumer, ShadowConsumer | ProbeConsumer, true,
                maximumLod, 0u);
        }
        const glm::vec3 middle = place(0.5f, 0.5f, 0.5f);
        addInstance(scene, middle, 0.3f, ShadowConsumer | ProbeConsumer, ShadowConsumer,
            true, 15u, 0u); // shadow-only primitive
        addInstance(scene, middle, 0.3f, ShadowConsumer | ProbeConsumer, ProbeConsumer,
            true, 15u, 0u); // probe-only primitive
        addInstance(scene, middle, 0.3f, ProbeConsumer, ShadowConsumer | ProbeConsumer,
            true, 15u, 0u); // probe-only instance
        addInstance(scene, middle, 0.3f, ShadowConsumer | ProbeConsumer,
            ShadowConsumer | ProbeConsumer, false, 15u, 0u); // disabled
        addInstance(scene, middle, 0.25f, ShadowConsumer | ProbeConsumer,
            ShadowConsumer | ProbeConsumer, true, 15u, 4u); // malformed chain
        return scene;
    }

    struct CompactExpectation {
        std::function<uint32_t(const CompactScene&, uint32_t primitive, float target)> select;
        uint32_t consumer = ShadowConsumer;
        uint32_t excludedInstance = InvalidGpuSceneIndex;
        float target = 1.0f;
    };

    // Fills a push block by reflected member name, so the test follows the
    // shader's own layout and fails if a member it relies on disappears.
    class PushWriter {
    public:
        explicit PushWriter(const Iridium::Test::SpirvModule& module)
            : members_(module.pushConstantMembers()),
              bytes_(module.pushConstantExtent()) {}
        void set(std::string_view name, uint32_t value) { write(name, &value); }
        void set(std::string_view name, float value) { write(name, &value); }
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
        [[nodiscard]] bool complete() const noexcept { return written_ == members_.size(); }
    private:
        void write(std::string_view name, const void* value) {
            for (const auto& member : members_)
                if (member.name == name) {
                    std::memcpy(bytes_.data() + member.offset, value, 4);
                    ++written_;
                    return;
                }
            throw std::runtime_error("push member missing: " + std::string(name));
        }
        std::vector<Iridium::Test::SpirvMember> members_;
        std::vector<std::byte> bytes_;
        size_t written_ = 0;
    };

    // Runs the production compaction shader and compares every emitted command
    // with the CPU selector. Decisions within 0.1% of the error threshold are
    // excluded as floating-point ties.
    bool runCompactParity(const char* spv, const CompactScene& scene,
        std::span<const std::byte> uniformData,
        const std::function<void(PushWriter&)>& fillPush,
        const CompactExpectation& expectation) {
        HeadlessComputeKernel kernel(*gpu, Iridium::Test::shaderBinary(spv));
        const auto candidateCount = static_cast<uint32_t>(scene.candidates.size());
        Buffer uniform(uniformData.size(), UniformUsage);
        std::memcpy(uniform.value.mapped, uniformData.data(), uniformData.size());
        Buffer transforms(std::span(scene.transforms), StorageUsage);
        Buffer instances(std::span(scene.instances), StorageUsage);
        Buffer primitives(std::span(scene.primitives), StorageUsage);
        Buffer geometries(std::span(scene.geometries), StorageUsage);
        Buffer candidates(std::span(scene.candidates), StorageUsage);
        Buffer commands(sizeof(VkDrawIndexedIndirectCommand) * candidateCount, StorageUsage);
        Buffer counts(sizeof(uint32_t) * candidateCount, StorageUsage);

        PushWriter push(kernel.module());
        push.set("candidateCount", candidateCount);
        push.set("transformCount", static_cast<uint32_t>(scene.transforms.size()));
        push.set("instanceCount", static_cast<uint32_t>(scene.instances.size()));
        push.set("primitiveCount", static_cast<uint32_t>(scene.primitives.size()));
        push.set("geometryCount", static_cast<uint32_t>(scene.geometries.size()));
        push.set("countBase", 0u);
        push.set("commandBase", 0u);
        fillPush(push);
        IRIDIUM_CHECK_MSG(push.complete(), spv << " has unfilled push members");

        kernel.dispatch({ { { 0, 0 }, uniform.value.buffer },
            { { 1, 0 }, transforms.value.buffer }, { { 1, 1 }, instances.value.buffer },
            { { 1, 2 }, primitives.value.buffer }, { { 1, 3 }, geometries.value.buffer },
            { { 2, 0 }, candidates.value.buffer }, { { 2, 1 }, commands.value.buffer },
            { { 2, 2 }, counts.value.buffer } }, push.bytes(), (candidateCount + 63u) / 64u);

        const auto* emitted = commands.as<VkDrawIndexedIndirectCommand>();
        const auto* emittedCounts = counts.as<uint32_t>();
        std::array<uint32_t, 4> lodHistogram{};
        uint32_t compared = 0, ties = 0, filtered = 0;
        for (uint32_t index = 0; index < candidateCount; ++index) {
            const GpuScenePrimitiveRecord& primitive = scene.primitives[index];
            const GpuSceneInstanceRecord& instance = scene.instances[primitive.binding.x];
            const bool accepted = (instance.state.z & GpuSceneInstanceEnabled) != 0u &&
                (instance.state.w & expectation.consumer) != 0u &&
                (primitive.state.w & expectation.consumer) != 0u &&
                primitive.binding.x != expectation.excludedInstance;
            IRIDIUM_CHECK_MSG(emittedCounts[index] == (accepted ? 1u : 0u),
                spv << " candidate " << index << " count " << emittedCounts[index]);
            if (!accepted) { ++filtered; continue; }
            const uint32_t selected = expectation.select(scene, index, expectation.target);
            if (expectation.select(scene, index, expectation.target * 0.999f) != selected ||
                expectation.select(scene, index, expectation.target * 1.001f) != selected) {
                ++ties;
                continue;
            }
            const GpuSceneGeometryRecord& geometry = scene.geometries[selected];
            const VkDrawIndexedIndirectCommand& command = emitted[index];
            IRIDIUM_CHECK_MSG(command.indexCount == geometry.draw.y &&
                command.firstIndex == geometry.draw.x, spv << " candidate " << index
                << " device firstIndex " << command.firstIndex << ", CPU geometry "
                << selected);
            IRIDIUM_CHECK(command.instanceCount == 1u);
            IRIDIUM_CHECK(command.vertexOffset == static_cast<int32_t>(geometry.draw.z));
            IRIDIUM_CHECK(command.firstInstance == primitive.binding.x);
            if (selected < lodHistogram.size()) ++lodHistogram[selected];
            ++compared;
        }
        std::cout << "  " << spv << ": " << compared << " compared, " << ties
            << " threshold ties, " << filtered << " filtered; LOD histogram "
            << lodHistogram[0] << '/' << lodHistogram[1] << '/' << lodHistogram[2]
            << '/' << lodHistogram[3] << '\n';
        IRIDIUM_CHECK(compared >= 40u);
        // The sweep must exercise every level of the chain.
        for (const uint32_t levelCount : lodHistogram) IRIDIUM_CHECK(levelCount > 0u);
        IRIDIUM_CHECK(filtered >= 3u);
        return gpu->validationErrors() == 0u;
    }

    bool testDirectionalCompactParity() {
        constexpr uint32_t CascadeLayer = 5u;
        const CompactScene scene = buildScene([](float u, float v, float depth) {
            return glm::vec3(-30.0f + 60.0f * u, -30.0f + 60.0f * v, -50.0f + 100.0f * depth);
        });
        VulkanDirectionalShadowData data{};
        data.worldToShadowClip[CascadeLayer] = glm::orthoRH_ZO(-60.0f, 60.0f, -60.0f,
            60.0f, 0.1f, 400.0f) * glm::lookAt(glm::vec3(0, 0, 150), glm::vec3(0),
                glm::vec3(0, 1, 0));
        data.texelWorldSize[CascadeLayer / 4u][CascadeLayer % 4u] = 0.05f;
        CompactExpectation expectation;
        expectation.target = 1.0f;
        expectation.select = [&](const CompactScene& value, uint32_t primitive, float target) {
            const auto& record = value.primitives[primitive];
            return selectGpuSceneDensityLodGeometry(value.geometries, record.binding.y,
                value.instances[record.binding.x], 0.05f, target,
                value.candidates[primitive].maximumLod);
        };
        return runCompactParity("directional_shadow_compact_comp.spv", scene,
            bytesOf(data), [&](PushWriter& push) {
                push.set("cascadeLayerIndex", CascadeLayer);
                push.set("lodErrorTexels", expectation.target);
            }, expectation);
    }

    bool testSpotCompactParity() {
        constexpr uint32_t Slot = 3u;
        constexpr uint32_t TileResolution = 1024u;
        const CompactScene scene = buildScene([](float u, float v, float depth) {
            const float z = -(6.0f + 140.0f * depth);
            return glm::vec3((u - 0.5f) * 0.9f * -z, (v - 0.5f) * 0.9f * -z, z);
        });
        auto data = std::make_unique<VulkanSpotShadowData>();
        const glm::mat4 worldToClip = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f,
            0.1f, 200.0f) * glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
        data->entries[Slot].worldToShadowClip = worldToClip;
        data->entries[Slot].metadata = { 0u, 1u, TileResolution, 0u };
        CompactExpectation expectation;
        expectation.target = 1.0f;
        expectation.select = [&](const CompactScene& value, uint32_t primitive, float target) {
            const auto& record = value.primitives[primitive];
            const auto& instance = value.instances[record.binding.x];
            const glm::mat4 clipFromLocal = worldToClip *
                unpackGpuSceneAffine(value.transforms[instance.references.x]);
            return selectGpuSceneLodGeometry(value.geometries, record.binding.y,
                clipFromLocal, glm::vec2(static_cast<float>(TileResolution)), target,
                value.candidates[primitive].maximumLod);
        };
        return runCompactParity("spot_shadow_compact_comp.spv", scene, bytesOf(*data),
            [&](PushWriter& push) {
                push.set("shadowDataSlot", Slot);
                push.set("lodErrorTexels", expectation.target);
            }, expectation);
    }

    bool testPointCompactParity() {
        constexpr uint32_t Entry = 2u, Face = 1u, FaceResolution = 512u;
        const glm::vec3 light(10.0f, 0.0f, 0.0f);
        const CompactScene scene = buildScene([&](float u, float v, float depth) {
            const float forward = 6.0f + 80.0f * depth;
            return light + glm::vec3(-forward, (u - 0.5f) * 0.9f * forward,
                (v - 0.5f) * 0.9f * forward);
        });
        auto data = std::make_unique<VulkanPointShadowData>();
        data->entries[Entry].lightPositionFar = glm::vec4(light, 100.0f);
        data->entries[Entry].worldToShadowClip[Face] = glm::perspectiveRH_ZO(
            glm::radians(90.0f), 1.0f, 0.05f, 100.0f) *
            glm::lookAt(light, light + glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0));
        CompactExpectation expectation;
        expectation.target = 1.0f;
        expectation.select = [&](const CompactScene& value, uint32_t primitive, float target) {
            const auto& record = value.primitives[primitive];
            return selectGpuSceneRadialLodGeometry(value.geometries, record.binding.y,
                value.instances[record.binding.x], light,
                static_cast<float>(FaceResolution), target,
                value.candidates[primitive].maximumLod);
        };
        return runCompactParity("point_shadow_compact_comp.spv", scene, bytesOf(*data),
            [&](PushWriter& push) {
                push.set("shadowFaceSlot", Entry * 6u + Face);
                push.set("lodErrorTexels", expectation.target);
                push.set("faceResolution", FaceResolution);
            }, expectation);
    }

    bool testReflectionProbeCompactParity() {
        constexpr uint32_t Resolution = 256u;
        constexpr uint32_t Excluded = 9u;
        const glm::vec3 capture(0.0f, 2.0f, 0.0f);
        const CompactScene scene = buildScene([&](float u, float v, float depth) {
            const float forward = 6.0f + 70.0f * depth;
            return capture + glm::vec3((u - 0.5f) * 0.9f * forward,
                (v - 0.5f) * 0.9f * forward, -forward);
        });
        VulkanReflectionProbeCaptureFaceData face{};
        face.worldToClip = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 200.0f) *
            glm::lookAt(capture, capture + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
        face.clipToWorld = glm::inverse(face.worldToClip);
        face.capturePositionNear = glm::vec4(capture, 0.1f);
        face.metadata = { 0u, 0u, Resolution, 0u };
        CompactExpectation expectation;
        expectation.consumer = ProbeConsumer;
        expectation.excludedInstance = Excluded;
        expectation.target = 1.0f;
        expectation.select = [&](const CompactScene& value, uint32_t primitive, float target) {
            const auto& record = value.primitives[primitive];
            return selectGpuSceneRadialLodGeometry(value.geometries, record.binding.y,
                value.instances[record.binding.x], capture,
                static_cast<float>(Resolution), target,
                value.candidates[primitive].maximumLod);
        };
        return runCompactParity("reflection_probe_capture_compact_comp.spv", scene,
            bytesOf(face), [&](PushWriter& push) {
                push.set("excludedInstanceIndex", Excluded);
                push.set("lodErrorPixels", expectation.target);
                push.set("reserved", 0u);
            }, expectation);
    }

} // namespace

int main() {
    try {
        gpu = std::make_unique<HeadlessVulkanDevice>(
            HeadlessVulkanDevice::Options{ "Iridium shader parity" });
    } catch (const std::exception& exception) {
        std::cerr << "headless Vulkan device unavailable: " << exception.what() << '\n';
        return 1;
    }
    constexpr Iridium::Test::TestCase tests[] = {
        { "scene_color.glsl matches SceneColor.h", testSceneColorParity },
        { "direct_lighting.glsl matches LightingReference.h", testDirectLightingParity },
        { "material_bsdf.glsl matches StandardMaterialShading.h", testBsdfParity },
        { "directional_shadow_compact matches the density LOD oracle",
            testDirectionalCompactParity },
        { "spot_shadow_compact matches the projected LOD oracle", testSpotCompactParity },
        { "point_shadow_compact matches the radial LOD oracle", testPointCompactParity },
        { "reflection_probe_capture_compact matches the radial LOD oracle",
            testReflectionProbeCompactParity },
    };
    const int result = Iridium::Test::runTests(tests);
    gpu.reset();
    return result;
}
