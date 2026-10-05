// C++/GLSL ABI contracts checked against the compiled SPIR-V the engine loads.
// Replaces the source-text record/binding assertions of LightExtractorTests,
// ReflectionProbeTests, LightingReferenceTests, StandardMaterialShadingTests and
// Stage3ArchitectureTests (groups 1, 2 and 6). See
// docs/milestones/M7R-R2-assertion-disposition.md.

#include "SpirvInspector.h"
#include "TestHarness.h"

#include "material/MaterialRuntime.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/GpuScene.h"
#include "renderer/rhi/GpuSceneIndirect.h"
#include "renderer/rhi/LightingTypes.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ReflectionProbeTypes.h"
#include "renderer/transparency/LayeredGlass.h"
#include "renderer/vulkan/VkLightingPipeline.h"
#include "renderer/vulkan/VulkanDirectionalShadowMap.h"
#include "renderer/vulkan/VulkanPointShadowPools.h"
#include "renderer/vulkan/VulkanReflectionProbeCapturePass.h"
#include "renderer/vulkan/VulkanSpotShadowAtlas.h"
#include "renderer/vulkan/VulkanVertexUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using namespace Iridium;
    using Iridium::Test::SpirvDescriptorKind;
    using Iridium::Test::SpirvModule;

    struct ExpectedMember {
        const char* name;
        size_t offset;
    };

#define IRIDIUM_MEMBER(Type, field) ExpectedMember{ #field, offsetof(Type, field) }

    SpirvModule load(const char* spvName) {
        return SpirvModule::load(Iridium::Test::shaderBinary(spvName));
    }

    // Every listed member exists in the laid-out SPIR-V struct at the C++ offset,
    // and the SPIR-V extent does not exceed the C++ size (equality when exact).
    bool membersMatch(const SpirvModule& module, const char* structName,
        std::initializer_list<ExpectedMember> expected, size_t cppSize,
        bool exactExtent = true) {
        const auto members = module.structMembers(structName);
        IRIDIUM_CHECK_MSG(members.has_value(), module.label() << ": " << structName);
        for (const ExpectedMember& member : expected) {
            const auto offset = module.memberOffset(structName, member.name);
            IRIDIUM_CHECK_MSG(offset.has_value(),
                module.label() << ": " << structName << '.' << member.name);
            IRIDIUM_CHECK_MSG(*offset == member.offset, module.label() << ": "
                << structName << '.' << member.name << " at " << *offset
                << ", C++ " << member.offset);
        }
        const auto extent = module.structExtent(structName);
        IRIDIUM_CHECK_MSG(extent.has_value(), module.label() << ": " << structName);
        if (exactExtent)
            IRIDIUM_CHECK_MSG(*extent == cppSize, module.label() << ": " << structName
                << " extent " << *extent << ", C++ " << cppSize);
        else
            IRIDIUM_CHECK_MSG(*extent <= cppSize, module.label() << ": " << structName
                << " extent " << *extent << ", C++ " << cppSize);
        return true;
    }

    // Members in declaration order sit at the given offsets (for blocks whose
    // GLSL member names differ from the C++ field names).
    bool offsetsInOrder(const std::vector<Iridium::Test::SpirvMember>& members,
        std::initializer_list<size_t> offsets, const std::string& label) {
        IRIDIUM_CHECK_MSG(members.size() == offsets.size(), label << " member count "
            << members.size());
        size_t index = 0;
        for (const size_t offset : offsets) {
            IRIDIUM_CHECK_MSG(members[index].offset == offset, label << " member "
                << members[index].name << " at " << members[index].offset
                << ", C++ " << offset);
            ++index;
        }
        return true;
    }

    constexpr std::array ForwardMaterialShaders{
        "complex_material_indexed_frag.spv",
        "complex_opaque_material_indexed_frag.spv",
        "layered_ordinary2_material_indexed_frag.spv",
        "layered_deep_material_indexed_frag.spv",
        "layered_deep_residual_material_indexed_frag.spv",
        "weighted_oit_material_indexed_frag.spv",
    };
    // M9 G5a: every shader that declares the set-0 view block through
    // include/view_uniforms.glsl.
    constexpr std::array ViewUniformShaders{
        "canonical_material_vert.spv",
        "gpu_scene_material_vert.spv",
        "weighted_oit_instanced_vert.spv",
        "gpu_scene_frustum_compact_comp.spv",
        "gpu_scene_frustum_occlusion_compact_comp.spv",
        "depth_pyramid_gpu_scene_query_comp.spv",
        "transparency_pyramid_comp.spv",
        "complex_material_indexed_frag.spv",
        "complex_opaque_material_indexed_frag.spv",
        "layered_ordinary2_material_indexed_frag.spv",
        "layered_deep_material_indexed_frag.spv",
        "layered_deep_residual_material_indexed_frag.spv",
        "weighted_oit_material_indexed_frag.spv",
    };
    constexpr std::array DeferredLightingShaders{
        "canonical_reference_lighting_frag.spv",
        "canonical_packed_lighting_frag.spv",
    };
    constexpr std::array CompactShaders{
        "directional_shadow_compact_comp.spv",
        "spot_shadow_compact_comp.spv",
        "point_shadow_compact_comp.spv",
        "reflection_probe_capture_compact_comp.spv",
    };

    // LightExtractorTests: PackedGpuLight mirror, in the cluster builder and in
    // both deferred and forward consumers (one record representation).
    bool testPackedLightRecordAbi() {
        for (const char* spv : { "cluster_count_comp.spv",
                "canonical_reference_lighting_frag.spv",
                "complex_material_indexed_frag.spv" }) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK(membersMatch(module, "PackedGpuLight", {
                IRIDIUM_MEMBER(PackedGpuLight, positionRange),
                IRIDIUM_MEMBER(PackedGpuLight, directionOuterCos),
                IRIDIUM_MEMBER(PackedGpuLight, colorIntensity),
                IRIDIUM_MEMBER(PackedGpuLight, shapeMetadata) },
                sizeof(PackedGpuLight)));
        }
        const SpirvModule builder = load("cluster_count_comp.spv");
        IRIDIUM_CHECK(builder.memberArrayStride("LightRecords", "lights") ==
            sizeof(PackedGpuLight));
        IRIDIUM_CHECK(builder.structExtent("ClusterLightHeader") ==
            sizeof(ClusterLightHeader));
        for (const char* spv : { "canonical_reference_lighting_frag.spv",
                "complex_material_indexed_frag.spv" }) {
            const SpirvModule consumer = load(spv);
            IRIDIUM_CHECK(consumer.memberArrayStride("IridiumLightRecordBuffer",
                "iridiumLights") == sizeof(PackedGpuLight));
            IRIDIUM_CHECK(consumer.memberArrayStride("IridiumClusterHeaderBuffer",
                "iridiumClusterHeaders") == sizeof(ClusterLightHeader));
        }
        return true;
    }

    // ReflectionProbeTests: probe record and cluster-parameter mirrors.
    bool testReflectionProbeRecordAbi() {
        const SpirvModule cluster = load("reflection_probe_cluster_comp.spv");
        IRIDIUM_CHECK(membersMatch(cluster, "PackedGpuReflectionProbe", {
            IRIDIUM_MEMBER(PackedGpuReflectionProbe, worldToProbe),
            IRIDIUM_MEMBER(PackedGpuReflectionProbe, influence),
            IRIDIUM_MEMBER(PackedGpuReflectionProbe, positionIntensity),
            IRIDIUM_MEMBER(PackedGpuReflectionProbe, metadata) },
            sizeof(PackedGpuReflectionProbe)));
        IRIDIUM_CHECK(cluster.memberArrayStride("ProbeRecords", "probeRecords") ==
            sizeof(PackedGpuReflectionProbe));
        const auto parameters = cluster.structMembers("ProbeClusterParameters");
        IRIDIUM_CHECK(parameters.has_value());
        using Parameters = PackedGpuReflectionProbeClusterParameters;
        IRIDIUM_CHECK(offsetsInOrder(*parameters, {
            offsetof(Parameters, view), offsetof(Parameters, projection),
            offsetof(Parameters, inverseView), offsetof(Parameters, grid),
            offsetof(Parameters, depth), offsetof(Parameters, limits),
            offsetof(Parameters, tiles) }, "ProbeClusterParameters"));
        IRIDIUM_CHECK(cluster.structExtent("ProbeClusterParameters") ==
            sizeof(Parameters));
        IRIDIUM_CHECK((cluster.localSize() == std::array<uint32_t, 3>{ 64, 1, 1 }));
        const auto headers = cluster.descriptorNamed("ProbeClusterHeaders");
        IRIDIUM_CHECK(headers.has_value());
        IRIDIUM_CHECK(headers->kind == SpirvDescriptorKind::StorageBuffer);
        IRIDIUM_CHECK(headers->set == 0 && headers->binding == 3);
        const auto uniform = cluster.descriptorNamed("ProbeClusterParameters");
        IRIDIUM_CHECK(uniform.has_value());
        IRIDIUM_CHECK(uniform->kind == SpirvDescriptorKind::UniformBuffer);

        // Deferred and forward consumers read the same probe record.
        for (const char* spv : { "canonical_reference_lighting_frag.spv",
                "complex_material_indexed_frag.spv" }) {
            const SpirvModule consumer = load(spv);
            IRIDIUM_CHECK(membersMatch(consumer, "PackedGpuReflectionProbe", {
                IRIDIUM_MEMBER(PackedGpuReflectionProbe, worldToProbe),
                IRIDIUM_MEMBER(PackedGpuReflectionProbe, metadata) },
                sizeof(PackedGpuReflectionProbe)));
        }
        return true;
    }

    // Stage3 group 1: the GPU-scene tables and indirect records every device
    // compaction shader reads/writes match the C++ records.
    bool testGpuSceneCompactionAbi() {
        for (const char* spv : CompactShaders) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK_MSG((module.localSize() ==
                std::array<uint32_t, 3>{ 64, 1, 1 }), spv);
            IRIDIUM_CHECK(membersMatch(module, "Transform", {
                IRIDIUM_MEMBER(GpuSceneAffineTransform, row0),
                IRIDIUM_MEMBER(GpuSceneAffineTransform, row1),
                IRIDIUM_MEMBER(GpuSceneAffineTransform, row2) },
                sizeof(GpuSceneAffineTransform)));
            IRIDIUM_CHECK(membersMatch(module, "Instance", {
                IRIDIUM_MEMBER(GpuSceneInstanceRecord, worldBoundsSphere),
                IRIDIUM_MEMBER(GpuSceneInstanceRecord, worldBoundsMin),
                IRIDIUM_MEMBER(GpuSceneInstanceRecord, worldBoundsMax),
                IRIDIUM_MEMBER(GpuSceneInstanceRecord, references),
                IRIDIUM_MEMBER(GpuSceneInstanceRecord, state) },
                sizeof(GpuSceneInstanceRecord)));
            IRIDIUM_CHECK(membersMatch(module, "Primitive", {
                IRIDIUM_MEMBER(GpuScenePrimitiveRecord, binding),
                IRIDIUM_MEMBER(GpuScenePrimitiveRecord, state),
                IRIDIUM_MEMBER(GpuScenePrimitiveRecord, revisions) },
                sizeof(GpuScenePrimitiveRecord)));
            IRIDIUM_CHECK(membersMatch(module, "Geometry", {
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, localBoundsSphere),
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, localBoundsMin),
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, localBoundsMax),
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, draw),
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, storage),
                IRIDIUM_MEMBER(GpuSceneGeometryRecord, state) },
                sizeof(GpuSceneGeometryRecord)));
            IRIDIUM_CHECK(membersMatch(module, "Candidate", {
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, primitiveIndex),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, binIndex),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, commandBase),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, commandCapacity),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, maximumLod),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, historySlot),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, historyTokenLow),
                IRIDIUM_MEMBER(GpuSceneIndirectCandidate, historyTokenHigh) },
                sizeof(GpuSceneIndirectCandidate)));
            IRIDIUM_CHECK(membersMatch(module, "IndexedCommand", {
                IRIDIUM_MEMBER(VkDrawIndexedIndirectCommand, indexCount),
                IRIDIUM_MEMBER(VkDrawIndexedIndirectCommand, instanceCount),
                IRIDIUM_MEMBER(VkDrawIndexedIndirectCommand, firstIndex),
                IRIDIUM_MEMBER(VkDrawIndexedIndirectCommand, vertexOffset),
                IRIDIUM_MEMBER(VkDrawIndexedIndirectCommand, firstInstance) },
                sizeof(VkDrawIndexedIndirectCommand)));
            IRIDIUM_CHECK(module.memberArrayStride("Commands", "values") ==
                sizeof(VkDrawIndexedIndirectCommand));
            IRIDIUM_CHECK(module.memberArrayStride("Geometries", "values") ==
                sizeof(GpuSceneGeometryRecord));
            // The device selector is fed by a host-chosen error threshold.
            const auto members = module.pushConstantMembers();
            const bool hasThreshold = std::ranges::any_of(members,
                [](const auto& member) {
                    return member.name == "lodErrorTexels" ||
                        member.name == "lodErrorPixels";
                });
            IRIDIUM_CHECK_MSG(hasThreshold, spv);
            // Vulkan guarantees 128 bytes of push constants on every device.
            IRIDIUM_CHECK_MSG(module.pushConstantExtent() <= 128u, spv);
        }
        return true;
    }

    // The shadow and probe tables the compaction shaders and the lighting
    // consumers read are the C++ upload records, field for field.
    bool testShadowAndProbeTableAbi() {
        const SpirvModule directional = load("directional_shadow_compact_comp.spv");
        IRIDIUM_CHECK(membersMatch(directional, "DirectionalShadowData", {
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, worldToShadowClip),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, splitFar),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, metadata),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, texelWorldSize),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, depthSpanMeters),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, filterParameters),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, filterMetadata),
            IRIDIUM_MEMBER(VulkanDirectionalShadowData, biasParameters) },
            sizeof(VulkanDirectionalShadowData)));

        const SpirvModule spot = load("spot_shadow_compact_comp.spv");
        IRIDIUM_CHECK(membersMatch(spot, "SpotShadowEntry", {
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, worldToShadowClip),
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, atlasScaleBias),
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, metadata),
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, biasParameters),
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, projectionParameters),
            IRIDIUM_MEMBER(VulkanSpotShadowEntry, filterMetadata) },
            sizeof(VulkanSpotShadowEntry)));
        IRIDIUM_CHECK(membersMatch(spot, "SpotShadowData", {
            IRIDIUM_MEMBER(VulkanSpotShadowData, entries),
            IRIDIUM_MEMBER(VulkanSpotShadowData, metadata) },
            sizeof(VulkanSpotShadowData)));
        IRIDIUM_CHECK(spot.memberArrayStride("SpotShadowData", "entries") ==
            sizeof(VulkanSpotShadowEntry));

        const SpirvModule point = load("point_shadow_compact_comp.spv");
        IRIDIUM_CHECK(membersMatch(point, "PointShadowEntry", {
            IRIDIUM_MEMBER(VulkanPointShadowEntry, worldToShadowClip),
            IRIDIUM_MEMBER(VulkanPointShadowEntry, lightPositionFar),
            IRIDIUM_MEMBER(VulkanPointShadowEntry, metadata),
            IRIDIUM_MEMBER(VulkanPointShadowEntry, depthBias),
            IRIDIUM_MEMBER(VulkanPointShadowEntry, filterParameters),
            IRIDIUM_MEMBER(VulkanPointShadowEntry, filterMetadata) },
            sizeof(VulkanPointShadowEntry)));
        IRIDIUM_CHECK(membersMatch(point, "PointShadowData", {
            IRIDIUM_MEMBER(VulkanPointShadowData, entries),
            IRIDIUM_MEMBER(VulkanPointShadowData, metadata) },
            sizeof(VulkanPointShadowData)));

        const SpirvModule probe = load("reflection_probe_capture_compact_comp.spv");
        IRIDIUM_CHECK(membersMatch(probe, "CaptureFaceData", {
            IRIDIUM_MEMBER(VulkanReflectionProbeCaptureFaceData, worldToClip),
            IRIDIUM_MEMBER(VulkanReflectionProbeCaptureFaceData, clipToWorld),
            IRIDIUM_MEMBER(VulkanReflectionProbeCaptureFaceData, capturePositionNear),
            IRIDIUM_MEMBER(VulkanReflectionProbeCaptureFaceData, metadata) },
            sizeof(VulkanReflectionProbeCaptureFaceData)));

        // Lighting consumers bind the same spot/point tables (bindings 23/27).
        for (const char* spv : { "canonical_reference_lighting_frag.spv",
                "complex_material_indexed_frag.spv" }) {
            const SpirvModule consumer = load(spv);
            IRIDIUM_CHECK(consumer.structExtent("IridiumSpotShadowData") ==
                sizeof(VulkanSpotShadowData));
            IRIDIUM_CHECK(consumer.memberArrayStride("IridiumSpotShadowData",
                "iridiumSpotShadowEntries") == sizeof(VulkanSpotShadowEntry));
            IRIDIUM_CHECK(consumer.structExtent("IridiumPointShadowData") ==
                sizeof(VulkanPointShadowData));
            IRIDIUM_CHECK(consumer.structExtent("IridiumDirectionalShadowData") ==
                sizeof(VulkanDirectionalShadowData));
        }
        return true;
    }

    // Stage3 group 2 / StandardMaterialShadingTests: mesh push block, packed
    // material record and the view UBO.
    bool testMeshPushMaterialAndViewAbi() {
        for (const char* spv : { "canonical_material_vert.spv",
                "gpu_scene_material_vert.spv", "complex_material_indexed_frag.spv",
                "complex_opaque_material_indexed_frag.spv",
                "layered_ordinary2_material_indexed_frag.spv",
                "layered_deep_material_indexed_frag.spv",
                "layered_deep_residual_material_indexed_frag.spv",
                "weighted_oit_material_indexed_frag.spv",
                "layered_scene_resolve_frag.spv" }) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK_MSG(module.pushConstantBlockName() ==
                std::optional<std::string>("CanonicalPushConstants"), spv);
            IRIDIUM_CHECK_MSG(module.pushConstantExtent() ==
                sizeof(CanonicalMeshPushConstants), spv);
            IRIDIUM_CHECK(offsetsInOrder(module.pushConstantMembers(), {
                offsetof(CanonicalMeshPushConstants, renderMatrix),
                offsetof(CanonicalMeshPushConstants, materialIndex),
                offsetof(CanonicalMeshPushConstants, padding),
                offsetof(CanonicalMeshPushConstants, padding) + 4u,
                offsetof(CanonicalMeshPushConstants, padding) + 8u },
                std::string(spv) + " CanonicalPushConstants"));
        }

        const SpirvModule capture = load("layered_interface_capture_frag.spv");
        IRIDIUM_CHECK(capture.pushConstantExtent() ==
            sizeof(LayeredInterfaceCapturePushConstants));
        IRIDIUM_CHECK(offsetsInOrder(capture.pushConstantMembers(), {
            offsetof(LayeredInterfaceCapturePushConstants, renderMatrix),
            offsetof(LayeredInterfaceCapturePushConstants, materialIndex),
            offsetof(LayeredInterfaceCapturePushConstants, workTableIndex),
            offsetof(LayeredInterfaceCapturePushConstants, flags),
            offsetof(LayeredInterfaceCapturePushConstants, packedViewportOffset) },
            "LayeredInterfaceCapturePushConstants"));

        for (const char* spv : DeferredLightingShaders) {
            const SpirvModule lighting = load(spv);
            IRIDIUM_CHECK(lighting.pushConstantExtent() == sizeof(LightingPushConstants));
            IRIDIUM_CHECK(offsetsInOrder(lighting.pushConstantMembers(), {
                offsetof(LightingPushConstants, viewPos),
                offsetof(LightingPushConstants, invView),
                offsetof(LightingPushConstants, invProj),
                offsetof(LightingPushConstants, debugView) },
                std::string(spv) + " LightingPushConstants"));
        }

        // M9 G5a: every shader using the shared set-0 view block
        // (include/view_uniforms.glsl) matches the C++ layout.
        for (const char* spv : ViewUniformShaders) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK(membersMatch(module, "UniformBufferObject", {
                IRIDIUM_MEMBER(UniformBufferObject, model),
                IRIDIUM_MEMBER(UniformBufferObject, view),
                IRIDIUM_MEMBER(UniformBufferObject, proj),
                IRIDIUM_MEMBER(UniformBufferObject, inverseView),
                IRIDIUM_MEMBER(UniformBufferObject, inverseProjection),
                IRIDIUM_MEMBER(UniformBufferObject, cameraPosition),
                IRIDIUM_MEMBER(UniformBufferObject, depthRange),
                IRIDIUM_MEMBER(UniformBufferObject, renderInfo),
                IRIDIUM_MEMBER(UniformBufferObject, worldUnits) },
                sizeof(UniformBufferObject)));
        }

        for (const char* spv : ForwardMaterialShaders) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK(membersMatch(module, "PackedMaterial", {
                IRIDIUM_MEMBER(PackedGpuMaterial, schemaVersion),
                IRIDIUM_MEMBER(PackedGpuMaterial, closureClass),
                IRIDIUM_MEMBER(PackedGpuMaterial, textureMask),
                IRIDIUM_MEMBER(PackedGpuMaterial, alphaMode),
                IRIDIUM_MEMBER(PackedGpuMaterial, complexLobeCount),
                IRIDIUM_MEMBER(PackedGpuMaterial, baseColorFactor),
                IRIDIUM_MEMBER(PackedGpuMaterial, metallicRoughnessIorSpecular),
                IRIDIUM_MEMBER(PackedGpuMaterial, emissiveFactorStrength),
                IRIDIUM_MEMBER(PackedGpuMaterial, surfaceParameters),
                IRIDIUM_MEMBER(PackedGpuMaterial, complexLobes),
                IRIDIUM_MEMBER(PackedGpuMaterial, textureUses),
                IRIDIUM_MEMBER(PackedGpuMaterial, textureIndices),
                IRIDIUM_MEMBER(PackedGpuMaterial, transparencyPolicy),
                IRIDIUM_MEMBER(PackedGpuMaterial, transparencyPriority),
                IRIDIUM_MEMBER(PackedGpuMaterial, thinSheetThicknessMeters) },
                sizeof(PackedGpuMaterial), false));
            IRIDIUM_CHECK_MSG(module.memberArrayStride("PackedMaterial",
                "complexLobes") == sizeof(PackedGpuComplexLobe), spv);
        }
        return true;
    }

    // Vertex fetch: the production attribute table feeds the canonical mesh
    // shader, and opaque shadow shaders consume position only (Stage3 group 6).
    bool testVertexInputContracts() {
        constexpr auto componentCount = [](VkFormat format) -> uint32_t {
            switch (format) {
            case VK_FORMAT_R32_SFLOAT: return 1;
            case VK_FORMAT_R32G32_SFLOAT: return 2;
            case VK_FORMAT_R32G32B32_SFLOAT: return 3;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return 4;
            default: return 0;
            }
        };
        const auto attributes = VulkanVertexUtils::getAttributeDescriptions();
        const SpirvModule canonical = load("canonical_material_vert.spv");
        const auto inputs = canonical.inputs();
        IRIDIUM_CHECK(inputs.size() == attributes.size());
        for (size_t index = 0; index < inputs.size(); ++index) {
            const auto& attribute = attributes[index];
            IRIDIUM_CHECK(inputs[index].location == attribute.location);
            IRIDIUM_CHECK(inputs[index].scalar == Iridium::Test::SpirvScalarKind::Float);
            IRIDIUM_CHECK_MSG(inputs[index].componentCount ==
                componentCount(attribute.format), inputs[index].name);
        }
        IRIDIUM_CHECK(VulkanVertexUtils::getBindingDescription().stride == sizeof(Vertex));

        for (const char* spv : {
                "directional_shadow_opaque_vert.spv",
                "directional_shadow_gpu_scene_opaque_vert.spv",
                "spot_shadow_opaque_vert.spv",
                "spot_shadow_gpu_scene_opaque_vert.spv",
                "point_shadow_opaque_vert.spv",
                "point_shadow_gpu_scene_opaque_vert.spv" }) {
            const SpirvModule module = load(spv);
            IRIDIUM_CHECK_MSG(module.executionModel() ==
                Iridium::Test::SpirvExecutionModel::Vertex, spv);
            const auto opaqueInputs = module.inputs();
            IRIDIUM_CHECK_MSG(opaqueInputs.size() == 1u, spv << " inputs "
                << opaqueInputs.size());
            IRIDIUM_CHECK(opaqueInputs[0].location == attributes[0].location);
            IRIDIUM_CHECK(opaqueInputs[0].componentCount ==
                componentCount(attributes[0].format));
            IRIDIUM_CHECK(opaqueInputs[0].scalar == Iridium::Test::SpirvScalarKind::Float);
        }
        // The alpha-masked variants still fetch texture coordinates, so the
        // opaque specialisation is a real reduction.
        for (const char* spv : { "directional_shadow_vert.spv",
                "spot_shadow_vert.spv", "point_shadow_vert.spv" })
            IRIDIUM_CHECK_MSG(load(spv).inputs().size() > 1u, spv);
        return true;
    }

    // One lighting descriptor representation: the deferred lighting pass (set 0)
    // and every forward-family consumer (set 3) declare identical resources at
    // each shared binding, including clustered lights, shadows and IBL.
    bool testLightingDescriptorRepresentation() {
        struct Declared {
            std::string name;
            SpirvDescriptorKind kind;
            uint32_t count;
            std::optional<uint32_t> imageDim;
            bool imageArrayed;
            std::string owner;
        };
        std::map<uint32_t, Declared> bindings;
        const auto merge = [&](const char* spv, uint32_t set) {
            const SpirvModule module = load(spv);
            uint32_t seen = 0;
            for (const auto& binding : module.descriptorBindings()) {
                if (binding.set != set) continue;
                ++seen;
                const auto [entry, inserted] = bindings.try_emplace(binding.binding,
                    Declared{ binding.name, binding.kind, binding.count,
                        binding.imageDim, binding.imageArrayed, spv });
                // Bindings 0-8 carry the GBuffer surface cache, which consumers
                // name by their own decoding; from 9 on every resource is shared
                // by identity (clustered lights, shadows, IBL, probes).
                const bool sameName = binding.binding < 9u ||
                    entry->second.name == binding.name;
                IRIDIUM_CHECK_MSG(inserted || (sameName &&
                    entry->second.kind == binding.kind &&
                    entry->second.count == binding.count &&
                    entry->second.imageDim == binding.imageDim &&
                    entry->second.imageArrayed == binding.imageArrayed),
                    spv << " binding " << binding.binding << " '" << binding.name
                        << "' disagrees with " << entry->second.owner << " '"
                        << entry->second.name << "'");
            }
            IRIDIUM_CHECK_MSG(seen != 0, spv << " declares no lighting set");
            return true;
        };
        for (const char* spv : DeferredLightingShaders) IRIDIUM_CHECK(merge(spv, 0));
        for (const char* spv : ForwardMaterialShaders) IRIDIUM_CHECK(merge(spv, 3));

        // Shared by deferred and forward: clustered light access (9-15),
        // environment/IBL (16-19, 28-31), directional (20-21), spot (22-23) and
        // point (24-27) shadows.
        for (const char* spv : { "canonical_reference_lighting_frag.spv",
                "complex_material_indexed_frag.spv" }) {
            const SpirvModule module = load(spv);
            const uint32_t set = std::string_view(spv).starts_with("canonical") ? 0u : 3u;
            for (uint32_t binding = 9; binding <= 31; ++binding)
                IRIDIUM_CHECK_MSG(module.descriptor(set, binding).has_value(),
                    spv << " lacks lighting binding " << binding);
        }
        const auto expectKind = [&](uint32_t binding, SpirvDescriptorKind kind) {
            const auto entry = bindings.find(binding);
            return entry != bindings.end() && entry->second.kind == kind;
        };
        for (uint32_t binding = 9; binding <= 14; ++binding)
            IRIDIUM_CHECK(expectKind(binding, SpirvDescriptorKind::StorageBuffer));
        for (uint32_t binding : { 15u, 21u, 23u, 27u })
            IRIDIUM_CHECK(expectKind(binding, SpirvDescriptorKind::UniformBuffer));
        for (uint32_t binding : { 16u, 17u, 18u, 19u, 20u, 22u, 24u, 25u, 26u, 31u })
            IRIDIUM_CHECK(expectKind(binding, SpirvDescriptorKind::CombinedImageSampler));
        for (uint32_t binding = 28; binding <= 30; ++binding)
            IRIDIUM_CHECK(expectKind(binding, SpirvDescriptorKind::StorageBuffer));
        IRIDIUM_CHECK(bindings.at(31).count == kMaximumGpuReflectionProbeEnvironments);
        // Environment/probe radiance are cube maps (no equirectangular HDRI
        // sampling in shading); point shadows are cube arrays, the directional
        // cascades a 2D array, the spot atlas and BRDF LUT plain 2D.
        constexpr uint32_t Dim2D = 1u, DimCube = 3u;
        const auto expectImage = [&](uint32_t binding, uint32_t dim, bool arrayed) {
            const auto entry = bindings.find(binding);
            return entry != bindings.end() && entry->second.imageDim == dim &&
                entry->second.imageArrayed == arrayed;
        };
        for (uint32_t binding : { 16u, 17u, 19u, 31u })
            IRIDIUM_CHECK_MSG(expectImage(binding, DimCube, false), "binding " << binding);
        IRIDIUM_CHECK(expectImage(18u, Dim2D, false));
        IRIDIUM_CHECK(expectImage(20u, Dim2D, true));
        IRIDIUM_CHECK(expectImage(22u, Dim2D, false));
        for (uint32_t binding : { 24u, 25u, 26u })
            IRIDIUM_CHECK_MSG(expectImage(binding, DimCube, true), "binding " << binding);
        return true;
    }

} // namespace

int main() {
    constexpr Iridium::Test::TestCase tests[] = {
        { "packed light record ABI", testPackedLightRecordAbi },
        { "reflection-probe record ABI", testReflectionProbeRecordAbi },
        { "GPU-scene compaction ABI", testGpuSceneCompactionAbi },
        { "shadow and probe table ABI", testShadowAndProbeTableAbi },
        { "mesh push, packed material and view ABI", testMeshPushMaterialAndViewAbi },
        { "vertex input contracts", testVertexInputContracts },
        { "lighting descriptor representation", testLightingDescriptorRepresentation },
    };
    return Iridium::Test::runTests(tests);
}
