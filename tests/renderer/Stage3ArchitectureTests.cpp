#include "renderer/rhi/PipelineTypes.h"
#include "renderer/rhi/DrawPacket.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ResourcePool.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "renderer/rhi/Ordinary2CaptureValidation.h"
#include "renderer/transparency/LayeredAtlas.h"
#include "renderer/transparency/LayeredGlass.h"
#include "renderer/transparency/Ordinary2Atlas.h"
#include "renderer/transparency/WeightedOit.h"
#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanPipelineLibrary.h"
#include "renderer/vulkan/VulkanResourceState.h"
#include "renderer/vulkan/VulkanGBufferLayout.h"
#include "profiling/CpuAllocationProfile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    struct MoveOnlyPayload {
        std::unique_ptr<int> value;

        MoveOnlyPayload() = default;
        explicit MoveOnlyPayload(int initialValue)
            : value(std::make_unique<int>(initialValue)) {}

        MoveOnlyPayload(const MoveOnlyPayload&) = delete;
        MoveOnlyPayload& operator=(const MoveOnlyPayload&) = delete;
        MoveOnlyPayload(MoveOnlyPayload&&) noexcept = default;
        MoveOnlyPayload& operator=(MoveOnlyPayload&&) noexcept = default;
    };

    bool testRenderHandleAndResourcePool() {
        ResourcePool<int, GeometryHandle> pool(1);
        CHECK(pool.empty());
        CHECK(pool.activeCount() == 0);
        CHECK(pool.capacity() == 1);

        const GeometryHandle first = pool.allocate(17);
        CHECK(first.isValid());
        CHECK(first.getIndex() == 0);
        CHECK(first.getGeneration() != 0);
        CHECK(pool.activeCount() == 1);
        CHECK(*pool.get(first) == 17);

        pool.free(first);
        CHECK(pool.empty());
        CHECK(pool.activeCount() == 0);
        CHECK(pool.get(first) == nullptr);

        const GeometryHandle reused = pool.allocate(29);
        CHECK(reused.isValid());
        CHECK(reused.getIndex() == first.getIndex());
        CHECK(reused.getGeneration() != first.getGeneration());
        CHECK(reused.getGeneration() != 0);
        CHECK(*pool.get(reused) == 29);
        uint32_t indexedVisits = 0;
        bool indexedPayloadMatches = true;
        pool.forEachIndexed([&](GeometryHandle visited, int& value) {
            indexedPayloadMatches = indexedPayloadMatches && visited == reused &&
                value == 29;
            ++indexedVisits;
        });
        CHECK(indexedVisits == 1);
        CHECK(indexedPayloadMatches);

        ResourcePool<MoveOnlyPayload, MaterialHandle> movePool(0);
        const MaterialHandle moved = movePool.allocate(MoveOnlyPayload{42});
        CHECK(moved.isValid());
        CHECK(movePool.capacity() == 1);
        CHECK(movePool.activeCount() == 1);
        CHECK(movePool.get(moved)->value != nullptr);
        CHECK(*movePool.get(moved)->value == 42);

        ResourcePool<int, TextureHandle> wrappingPool(1);
        TextureHandle handle = wrappingPool.allocate(1);
        for (uint32_t iteration = 0; iteration < TextureHandle::MaxGeneration; ++iteration) {
            wrappingPool.free(handle);
            handle = wrappingPool.allocate(1);
            CHECK(handle.getGeneration() != 0);
        }
        CHECK(handle.getGeneration() == 1);

        return true;
    }

    bool testGeometryDesc() {
        static_assert(std::is_trivially_copyable_v<GeometryDesc>);
        static_assert(sizeof(IndexFormat) == sizeof(uint8_t));

        CHECK(indexElementSize(IndexFormat::UInt16) == 2);
        CHECK(indexElementSize(IndexFormat::UInt32) == 4);
        CHECK(GeometryDesc{}.vertexStride == 0);
        CHECK(GeometryDesc{}.indexFormat == IndexFormat::UInt32);
        return true;
    }

    bool testCanonicalMaterialGpuContract() {
        static_assert(sizeof(Vertex) == 72);
        static_assert(offsetof(Vertex, color) == 12);
        static_assert(offsetof(Vertex, uv0) == 40);
        static_assert(offsetof(Vertex, uv1) == 64);
        static_assert(sizeof(CanonicalMeshPushConstants) == 80);
        static_assert(sizeof(PackedGpuMaterial) == 832);
        CHECK(PackedGpuMaterial::SchemaVersion == 3);
        CHECK(ShaderProgram::CanonicalPbrGBuffer !=
            ShaderProgram::CanonicalComplexForward);
        return true;
    }

    bool testCanonicalMaterialScaleContract() {
        constexpr uint32_t ScaleCount = 65'536;
        CHECK(nextMaterialTableCapacity(
            64, 65, ScaleCount) == 128);
        CHECK(nextMaterialTableCapacity(
            4096, ScaleCount,
            ScaleCount) == ScaleCount);

        ResourcePool<uint32_t, MaterialHandle>
            materials(64);
        std::vector<MaterialHandle> handles;
        handles.reserve(ScaleCount);
        for (uint32_t index = 0;
            index < ScaleCount; ++index) {
            const MaterialHandle handle =
                materials.allocate(index);
            CHECK(handle.getIndex() == index);
            handles.push_back(handle);
        }
        CHECK(materials.activeCount() ==
            ScaleCount);
        CHECK(materials.capacity() >=
            ScaleCount);
        CHECK(*materials.get(handles.back()) ==
            ScaleCount - 1u);
        return true;
    }

    bool testPipelineStateDescHash() {
        const PipelineStateDesc baseline{};
        const PipelineStateDesc equal = baseline;
        const PipelineStateDescHash hash{};
        CHECK(baseline == equal);
        CHECK(hash(baseline) == hash(equal));

        const auto changesHashAndEquality = [&baseline, &hash](PipelineStateDesc changed) {
            return changed != baseline && hash(changed) != hash(baseline);
        };

        PipelineStateDesc changed = baseline;
        changed.shaderProgram = ShaderProgram::CanonicalComplexForward;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.renderPass = RenderPassClass::Forward;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.renderPass = RenderPassClass::Transparent;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.topology = static_cast<PrimitiveTopology>(1);
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.polygonMode = PolygonMode::Line;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.cullMode = CullMode::Front;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.frontFace = FrontFace::CounterClockwise;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.blendMode = BlendMode::AlphaBlend;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.blendMode = BlendMode::PremultipliedAlpha;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.depthCompare = DepthCompare::LessOrEqual;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.colorWriteMask = ColorWriteR;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.depthTest = false;
        CHECK(changesHashAndEquality(changed));
        changed = baseline;
        changed.depthWrite = false;
        CHECK(changesHashAndEquality(changed));

        return true;
    }

    bool testRenderQueueDepthCoverage() {
        CHECK(renderQueueWritesDepth(RenderQueue::Opaque));
        CHECK(renderQueueWritesDepth(RenderQueue::ForwardOpaque));
        CHECK(!renderQueueWritesDepth(RenderQueue::Transparent));
        return true;
    }

    bool testResourceStateMapping() {
        struct ExpectedState {
            ResourceState state;
            VkPipelineStageFlags stages;
            VkAccessFlags access;
            VkImageLayout layout;
        };

        constexpr ExpectedState expectedStates[] = {
            { ResourceState::Undefined, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED },
            { ResourceState::CopySource, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
            { ResourceState::CopyDestination, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL },
            { ResourceState::VertexBuffer, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED },
            { ResourceState::IndexBuffer, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED },
            { ResourceState::ConstantBuffer, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED },
            { ResourceState::ShaderResource, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { ResourceState::ColorAttachment, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { ResourceState::DepthWrite, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
            { ResourceState::DepthRead, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
            { ResourceState::Present, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        };

        for (const ExpectedState& expected : expectedStates) {
            const VulkanStateInfo actual = getVulkanStateInfo(
                expected.state, VK_IMAGE_ASPECT_COLOR_BIT);
            CHECK(actual.stages == expected.stages);
            CHECK(actual.access == expected.access);
            CHECK(actual.layout == expected.layout);
        }

        const VulkanStateInfo depthWrite = getVulkanStateInfo(ResourceState::DepthWrite, VK_IMAGE_ASPECT_DEPTH_BIT);
        const VulkanStateInfo depthRead = getVulkanStateInfo(ResourceState::DepthRead, VK_IMAGE_ASPECT_DEPTH_BIT);
        const VulkanStateInfo sampledDepth = getVulkanStateInfo(
            ResourceState::ShaderResource, VK_IMAGE_ASPECT_DEPTH_BIT);
        CHECK(depthWrite.layout != depthRead.layout);
        CHECK(sampledDepth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        return true;
    }

    bool testInvalidCommandList() {
        const VulkanCommandList commandList;
        CHECK(!commandList.isValid());
        CHECK(commandList.native() == VK_NULL_HANDLE);
        return true;
    }

    bool testGBufferCandidateContracts() {
        const auto reference = vulkanGBufferFormats(GBufferLayout::CanonicalReference);
        const auto quality = vulkanGBufferFormats(GBufferLayout::CanonicalQuality);
        const auto compact = vulkanGBufferFormats(GBufferLayout::CanonicalCompact);
        CHECK(reference.colorBytesPerPixel == 36 && reference.colorAttachmentCount == 5);
        CHECK(reference.materialFlags == VK_FORMAT_R32_UINT);
        CHECK(reference.metadataBits == 32 && reference.preservesScalarF90);
        CHECK(quality.colorBytesPerPixel == 26 && quality.normalF90 == VK_FORMAT_R16G16_SNORM);
        CHECK(quality.metadataBits == 16 && !quality.preservesScalarF90);
        CHECK(compact.colorBytesPerPixel == 18 && compact.diffuseAo == VK_FORMAT_R8G8B8A8_UNORM);
        CHECK(compact.metadataBits == 16 && !compact.preservesScalarF90);
        CHECK(parseGBufferLayout("r") == GBufferLayout::CanonicalReference);
        CHECK(parseGBufferLayout("quality") == GBufferLayout::CanonicalQuality);
        return true;
    }

    bool testPackedGBufferNumericBounds() {
        double maxAngularErrorDegrees = 0.0;
        for (int latitude = 0; latitude <= 256; ++latitude) {
            const double z = -1.0 + 2.0 * static_cast<double>(latitude) / 256.0;
            const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
            for (int longitude = 0; longitude < 512; ++longitude) {
                const double angle = 6.28318530717958647692 *
                    static_cast<double>(longitude) / 512.0;
                glm::dvec3 normal(radius * std::cos(angle), radius * std::sin(angle), z);
                glm::dvec3 oct = normal /
                    (std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z));
                if (oct.z < 0.0) {
                    const glm::dvec2 old(oct.x, oct.y);
                    oct.x = (1.0 - std::abs(old.y)) * (old.x < 0.0 ? -1.0 : 1.0);
                    oct.y = (1.0 - std::abs(old.x)) * (old.y < 0.0 ? -1.0 : 1.0);
                }
                glm::dvec2 encoded(
                    std::round(std::clamp(oct.x, -1.0, 1.0) * 32767.0) / 32767.0,
                    std::round(std::clamp(oct.y, -1.0, 1.0) * 32767.0) / 32767.0);
                glm::dvec3 decoded(encoded.x, encoded.y,
                    1.0 - std::abs(encoded.x) - std::abs(encoded.y));
                if (decoded.z < 0.0) {
                    const glm::dvec2 old(decoded.x, decoded.y);
                    decoded.x = (1.0 - std::abs(old.y)) * (old.x < 0.0 ? -1.0 : 1.0);
                    decoded.y = (1.0 - std::abs(old.x)) * (old.y < 0.0 ? -1.0 : 1.0);
                }
                decoded = glm::normalize(decoded);
                const double cosine = std::clamp(glm::dot(normal, decoded), -1.0, 1.0);
                maxAngularErrorDegrees = std::max(maxAngularErrorDegrees,
                    std::acos(cosine) * 57.29577951308232);
            }
        }
        CHECK(maxAngularErrorDegrees <= 1.0);

        double maxUnorm8Error = 0.0;
        for (int value = 0; value <= 65535; ++value) {
            const double source = static_cast<double>(value) / 65535.0;
            const double decoded = std::round(source * 255.0) / 255.0;
            maxUnorm8Error = std::max(maxUnorm8Error, std::abs(source - decoded));
        }
        CHECK(maxUnorm8Error <= (1.0 / 255.0));
        return true;
    }

    bool testTransparentWorkIntervalsAndDeterministicOrder() {
        DrawPacket interval{};
        interval.worldTransform = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 0.0f, -10.0f));
        CHECK(prepareTransparentWorkInterval(interval,
            glm::vec3(-1.0f), glm::vec3(1.0f), glm::mat4(1.0f),
            0.1f, 100.0f));
        CHECK((interval.transparentWorkFlags &
            TransparentWorkIntervalValid) != 0);
        CHECK((interval.transparentWorkFlags &
            TransparentWorkCameraIntersecting) == 0);
        CHECK(std::abs(interval.transparentNearDepth - 9.0f) < 0.0001f);
        CHECK(std::abs(interval.transparentFarDepth - 11.0f) < 0.0001f);
        CHECK(interval.boundsMinWorld == glm::vec3(-1.0f, -1.0f, -11.0f));
        CHECK(interval.boundsMaxWorld == glm::vec3(1.0f, 1.0f, -9.0f));

        DrawPacket crossing{};
        crossing.worldTransform = glm::mat4(1.0f);
        CHECK(prepareTransparentWorkInterval(crossing,
            { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, -0.01f },
            glm::mat4(1.0f), 0.1f, 100.0f));
        CHECK((crossing.transparentWorkFlags &
            TransparentWorkNearClipped) != 0);
        CHECK((crossing.transparentWorkFlags &
            TransparentWorkCameraIntersecting) != 0);
        CHECK(std::abs(crossing.transparentNearDepth - 0.1f) < 0.0001f);

        DrawPacket mirrored = interval;
        mirrored.worldTransform = glm::scale(interval.worldTransform,
            glm::vec3(-1.0f, 1.0f, 1.0f));
        CHECK(prepareTransparentWorkInterval(mirrored,
            glm::vec3(-1.0f), glm::vec3(1.0f), glm::mat4(1.0f),
            0.1f, 100.0f));
        CHECK((mirrored.transparentWorkFlags &
            TransparentWorkMirrored) != 0);

        DrawPacket cameraMotion = interval;
        CHECK(prepareTransparentWorkInterval(cameraMotion,
            glm::vec3(-1.0f), glm::vec3(1.0f),
            glm::translate(glm::mat4(1.0f),
                glm::vec3(0.0f, 0.0f, 5.0f)),
            0.1f, 100.0f));
        CHECK(std::abs(cameraMotion.transparentNearDepth - 4.0f) < 0.0001f);
        CHECK(std::abs(cameraMotion.transparentFarDepth - 6.0f) < 0.0001f);

        DrawPacket farClipped{};
        farClipped.worldTransform = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 0.0f, -99.0f));
        CHECK(prepareTransparentWorkInterval(farClipped,
            glm::vec3(-2.0f), glm::vec3(2.0f), glm::mat4(1.0f),
            0.1f, 100.0f));
        CHECK((farClipped.transparentWorkFlags &
            TransparentWorkFarClipped) != 0);
        CHECK(std::abs(farClipped.transparentFarDepth - 100.0f) < 0.0001f);

        DrawPacket behind{};
        behind.worldTransform = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 0.0f, 10.0f));
        CHECK(!prepareTransparentWorkInterval(behind,
            glm::vec3(-1.0f), glm::vec3(1.0f), glm::mat4(1.0f),
            0.1f, 100.0f));
        CHECK((behind.transparentWorkFlags & TransparentWorkCulled) != 0);
        behind.worldTransform = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 0.0f, -150.0f));
        CHECK(!prepareTransparentWorkInterval(behind,
            glm::vec3(-1.0f), glm::vec3(1.0f), glm::mat4(1.0f),
            0.1f, 100.0f));

        DrawPacket invalid{};
        invalid.worldTransform = glm::mat4(1.0f);
        CHECK(prepareTransparentWorkInterval(invalid,
            { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f },
            glm::vec3(1.0f), glm::mat4(1.0f), 0.1f, 100.0f));
        CHECK((invalid.transparentWorkFlags &
            TransparentWorkInvalidBoundsFallback) != 0);

        const auto guid = [](uint8_t suffix) {
            AssetGuid::Bytes bytes{};
            bytes[6] = 0x70;
            bytes[8] = 0x80;
            bytes[15] = suffix;
            return AssetGuid(bytes);
        };
        const auto owner = [](uint8_t suffix) {
            SceneEntityUuid::Bytes bytes{};
            bytes[6] = 0x70;
            bytes[8] = 0x80;
            bytes[15] = suffix;
            return SceneEntityUuid(bytes);
        };
        DrawPacket far = interval;
        far.owner = owner(1);
        far.sourcePrimitiveGuid = guid(2);
        far.primitiveGuid = guid(3);
        far.materialGuid = guid(4);
        far.transparentNearDepth = 20.0f;
        far.transparentFarDepth = 30.0f;
        far.transparency.priority = 0;

        DrawPacket near = far;
        near.primitiveGuid = guid(5);
        near.transparentNearDepth = 5.0f;
        near.transparentFarDepth = 10.0f;

        DrawPacket top = near;
        top.primitiveGuid = guid(6);
        top.transparency.priority = 7;

        const std::array source{ top, near, far };
        constexpr std::array permutations{
            std::array<size_t, 3>{ 0, 1, 2 },
            std::array<size_t, 3>{ 0, 2, 1 },
            std::array<size_t, 3>{ 1, 0, 2 },
            std::array<size_t, 3>{ 1, 2, 0 },
            std::array<size_t, 3>{ 2, 0, 1 },
            std::array<size_t, 3>{ 2, 1, 0 },
        };
        for (const auto& permutation : permutations) {
            std::array work{ source[permutation[0]], source[permutation[1]],
                source[permutation[2]] };
            std::sort(work.begin(), work.end(), transparentWorkLess);
            CHECK(work[0].primitiveGuid == far.primitiveGuid);
            CHECK(work[1].primitiveGuid == near.primitiveGuid);
            CHECK(work[2].primitiveGuid == top.primitiveGuid);

            for (DrawPacket& packet : work) {
                packet.transparencyExecutionMode =
                    TransparencyExecutionMode::Classified;
            }
            std::ranges::reverse(work);
            std::sort(work.begin(), work.end(),
                transparentCompatibilityLess);
            CHECK(work[0].primitiveGuid == far.primitiveGuid);
            CHECK(work[1].primitiveGuid == near.primitiveGuid);
            CHECK(work[2].primitiveGuid == top.primitiveGuid);
        }

        DrawPacket cameraIntersecting = far;
        cameraIntersecting.primitiveGuid = guid(7);
        cameraIntersecting.transparentNearDepth = 0.1f;
        cameraIntersecting.transparentFarDepth = 90.0f;
        cameraIntersecting.transparentWorkFlags |=
            TransparentWorkCameraIntersecting;
        DrawPacket invalidLate = invalid;
        invalidLate.owner = owner(8);
        invalidLate.sourcePrimitiveGuid = guid(8);
        invalidLate.primitiveGuid = guid(8);
        invalidLate.materialGuid = guid(8);
        std::array mixed{ invalidLate, cameraIntersecting, far };
        std::sort(mixed.begin(), mixed.end(), transparentWorkLess);
        CHECK(mixed[0].primitiveGuid == far.primitiveGuid);
        CHECK(mixed[1].primitiveGuid == cameraIntersecting.primitiveGuid);
        CHECK(mixed[2].primitiveGuid == invalidLate.primitiveGuid);

        DrawPacket identicalA = far;
        identicalA.primitiveGuid = guid(10);
        DrawPacket identicalB = far;
        identicalB.primitiveGuid = guid(9);
        std::array identical{ identicalA, identicalB };
        std::sort(identical.begin(), identical.end(), transparentWorkLess);
        CHECK(identical[0].primitiveGuid == identicalB.primitiveGuid);
        CHECK(identical[1].primitiveGuid == identicalA.primitiveGuid);

        near.transparentNearDepth = 25.0f;
        near.transparentFarDepth = 35.0f;
        CHECK(countAmbiguousTransparentIntervals(
            std::array{ far, near, top }) == 1);
        DrawPacket cycleA = far;
        cycleA.transparentNearDepth = 1.0f;
        cycleA.transparentFarDepth = 5.0f;
        DrawPacket cycleB = near;
        cycleB.transparentNearDepth = 2.0f;
        cycleB.transparentFarDepth = 6.0f;
        DrawPacket cycleC = top;
        cycleC.transparency.priority = 0;
        cycleC.transparentNearDepth = 3.0f;
        cycleC.transparentFarDepth = 7.0f;
        CHECK(countAmbiguousTransparentIntervals(
            std::array{ cycleA, cycleB, cycleC }) == 3);
        std::array<TransparentIntervalEndpoint, 3> endpointScratch{};
        std::array<float, 3> nearScratch{};
        std::array<uint32_t, 4> fenwickScratch{};
        CHECK(sweepAmbiguousTransparentIntervals(
            std::array{ cycleC, cycleA, cycleB }, endpointScratch,
            nearScratch, fenwickScratch) == 3);
        return true;
    }

    bool testSwapchainRebuildRetiresClusterDescriptors() {
        const std::filesystem::path backendPath =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "src/renderer/vulkan/VulkanVertexBackend.cpp";
        std::ifstream input(backendPath, std::ios::binary);
        CHECK(input.good());
        const std::string source{ std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        const size_t functionBegin = source.find(
            "void VulkanVertexBackend::recreateSwapchain(GLFWwindow* window)");
        const size_t functionEnd = source.find(
            "RenderExtent VulkanVertexBackend::getRenderExtent() const",
            functionBegin);
        CHECK(functionBegin != std::string::npos);
        CHECK(functionEnd != std::string::npos);
        const std::string function = source.substr(
            functionBegin, functionEnd - functionBegin);
        const size_t clear = function.find(
            "clusteredLighting_.clearDescriptors()");
        const size_t rebuild = function.find(
            "rebuildRenderGraphAfterDeviceIdle()");
        const size_t bind = function.find("bindClusterBuffers()", rebuild);
        CHECK(clear != std::string::npos);
        CHECK(rebuild != std::string::npos);
        CHECK(bind != std::string::npos);
        CHECK(clear < rebuild);
        CHECK(rebuild < bind);
        return true;
    }

    bool testOrdinary2InterfacePairingContract() {
        LayeredInterfaceCapturePushConstants capturePush{};
        capturePush.materialIndex = 7u;
        capturePush.workTableIndex = 42u;
        capturePush.flags = kLayeredCaptureMirrored | kLayeredCaptureExit;
        capturePush.packedViewportOffset = packLayeredViewportOffset(
            -123, 456);
        CHECK(capturePush.materialIndex == 7u);
        CHECK(capturePush.workTableIndex == 42u);
        CHECK((capturePush.flags & kLayeredCaptureMirrored) != 0u);
        CHECK((capturePush.flags & kLayeredCaptureExit) != 0u);
        CHECK(validLayeredViewportOffset(-32768, 32767));
        CHECK(!validLayeredViewportOffset(-32769, 0));
        CHECK(!validLayeredViewportOffset(32768, 0));
        CHECK(layeredViewportOffsetX(capturePush.packedViewportOffset) ==
            -123);
        CHECK(layeredViewportOffsetY(capturePush.packedViewportOffset) ==
            456u);

        constexpr PackedLayeredInterfaceIdentity empty{};
        CHECK(!empty.occupied());
        constexpr PackedLayeredInterfaceIdentity entryIdentity =
            packLayeredInterfaceIdentity(42u,
                LayeredFaceOrientation::Entry);
        constexpr PackedLayeredInterfaceIdentity exitIdentity =
            packLayeredInterfaceIdentity(42u,
                LayeredFaceOrientation::Exit);
        CHECK(entryIdentity.occupied());
        CHECK(exitIdentity.occupied());
        CHECK(entryIdentity.workTableIndex() == 42u);
        CHECK(exitIdentity.workTableIndex() == 42u);
        CHECK(entryIdentity.orientation() == LayeredFaceOrientation::Entry);
        CHECK(exitIdentity.orientation() == LayeredFaceOrientation::Exit);
        CHECK(entryIdentity != exitIdentity);
        CHECK(!packLayeredInterfaceIdentity(
            kMaximumLayeredWorkTableIndex + 1u,
            LayeredFaceOrientation::Entry).occupied());

        const LayeredCaptureCandidateResult acceptedEntry =
            evaluateLayeredCaptureCandidate(0.25f, 0.9f, 42u,
                true, false, false);
        CHECK(acceptedEntry.accepted());
        CHECK(acceptedEntry.identity == entryIdentity);
        const LayeredCaptureCandidateResult acceptedExit =
            evaluateLayeredCaptureCandidate(0.4f, 0.9f, 42u,
                false, false, true, 0.25f, entryIdentity);
        CHECK(acceptedExit.accepted());
        CHECK(acceptedExit.identity == exitIdentity);
        CHECK(evaluateLayeredCaptureCandidate(0.95f, 0.9f, 42u,
            true, false, false).status ==
            LayeredCaptureCandidateStatus::OpaqueOccluded);
        CHECK(evaluateLayeredCaptureCandidate(0.25f, 0.9f, 42u,
            false, false, false).status ==
            LayeredCaptureCandidateStatus::OrientationMismatch);
        CHECK(evaluateLayeredCaptureCandidate(0.4f, 0.9f, 42u,
            false, false, true, 0.25f, {}).status ==
            LayeredCaptureCandidateStatus::MissingEntry);
        CHECK(evaluateLayeredCaptureCandidate(0.4f, 0.9f, 43u,
            false, false, true, 0.25f, entryIdentity).status ==
            LayeredCaptureCandidateStatus::EntryWorkMismatch);
        CHECK(evaluateLayeredCaptureCandidate(0.2f, 0.9f, 42u,
            false, false, true, 0.25f, entryIdentity).status ==
            LayeredCaptureCandidateStatus::NotBehindEntry);
        CHECK(evaluateLayeredCaptureCandidate(0.4f, 0.9f,
            kMaximumLayeredWorkTableIndex + 1u, false, false, true,
            0.25f, entryIdentity).status ==
            LayeredCaptureCandidateStatus::WorkIndexExceeded);
        CHECK(evaluateLayeredCaptureCandidate(
            std::numeric_limits<float>::quiet_NaN(), 0.9f, 42u,
            true, false, false).status ==
            LayeredCaptureCandidateStatus::InvalidDepth);

        const PackedLayeredInterfaceIdentity nestedOuterEntry =
            evaluateLayeredPeelCandidate(0.2f, 0.9f, 40u,
                true, false, false).identity;
        CHECK(nestedOuterEntry.occupied());
        CHECK(nestedOuterEntry.orientation() ==
            LayeredFaceOrientation::Entry);
        const LayeredPeelCandidateResult nestedInnerEntry =
            evaluateLayeredPeelCandidate(0.3f, 0.9f, 41u,
                true, false, true, 0.2f, nestedOuterEntry);
        CHECK(nestedInnerEntry.accepted());
        CHECK(nestedInnerEntry.identity.workTableIndex() == 41u);
        CHECK(nestedInnerEntry.identity.orientation() ==
            LayeredFaceOrientation::Entry);
        const LayeredPeelCandidateResult nestedInnerExit =
            evaluateLayeredPeelCandidate(0.4f, 0.9f, 41u,
                false, false, true, 0.3f, nestedInnerEntry.identity);
        CHECK(nestedInnerExit.accepted());
        CHECK(nestedInnerExit.identity.orientation() ==
            LayeredFaceOrientation::Exit);
        const LayeredPeelCandidateResult nestedOuterExit =
            evaluateLayeredPeelCandidate(0.5f, 0.9f, 40u,
                false, false, true, 0.4f, nestedInnerExit.identity);
        CHECK(nestedOuterExit.accepted());
        CHECK(nestedOuterExit.identity.workTableIndex() == 40u);
        CHECK(nestedOuterExit.identity.orientation() ==
            LayeredFaceOrientation::Exit);
        CHECK(evaluateLayeredPeelCandidate(0.3f, 0.9f, 41u,
            true, false, true, 0.2f, {}).status ==
            LayeredPeelCandidateStatus::MissingPreviousInterface);
        CHECK(evaluateLayeredPeelCandidate(0.2f, 0.9f, 41u,
            true, false, true, 0.2f, nestedOuterEntry).status ==
            LayeredPeelCandidateStatus::NotBehindPreviousInterface);
        CHECK(evaluateLayeredPeelCandidate(0.95f, 0.9f, 41u,
            true, false, false).status ==
            LayeredPeelCandidateStatus::OpaqueOccluded);

        AssetGuid::Bytes primitiveBytes{};
        primitiveBytes.back() = 1;
        TransparentWorkIdentity work{
            .primitiveGuid = AssetGuid(primitiveBytes),
        };
        BoundedLayeredWorkTable<2> workTable;
        const LayeredWorkTableInsertResult firstInsert =
            workTable.insert(work);
        CHECK(firstInsert.status == LayeredWorkTableInsertStatus::Inserted);
        CHECK(firstInsert.workTableIndex == 0u);
        const LayeredWorkTableInsertResult duplicateInsert =
            workTable.insert(work);
        CHECK(duplicateInsert.status == LayeredWorkTableInsertStatus::Existing);
        CHECK(duplicateInsert.workTableIndex == firstInsert.workTableIndex);

        TransparentWorkIdentity secondWork = work;
        AssetGuid::Bytes secondPrimitiveBytes{};
        secondPrimitiveBytes.back() = 2;
        secondWork.primitiveGuid = AssetGuid(secondPrimitiveBytes);
        CHECK(workTable.insert(secondWork).status ==
            LayeredWorkTableInsertStatus::Inserted);
        TransparentWorkIdentity overflowWork = work;
        AssetGuid::Bytes overflowPrimitiveBytes{};
        overflowPrimitiveBytes.back() = 3;
        overflowWork.primitiveGuid = AssetGuid(overflowPrimitiveBytes);
        CHECK(workTable.insert(overflowWork).status ==
            LayeredWorkTableInsertStatus::CapacityExceeded);
        CHECK(workTable.size() == 2u);
        CHECK(workTable.identities()[0] == work);
        CHECK(workTable.identities()[1] == secondWork);
        CHECK(workTable.stats().uniqueWorkCount == 2u);
        CHECK(workTable.stats().duplicateWorkCount == 1u);
        CHECK(workTable.stats().rejectedWorkCount == 1u);
        CHECK(workTable.stats().hashProbeCount >= 4u);
        CHECK(workTable.stats().maximumProbeCount >= 1u);
        CHECK(hashTransparentWorkIdentity(work) ==
            hashTransparentWorkIdentity(work));
        CHECK(hashTransparentWorkIdentity(work) !=
            hashTransparentWorkIdentity(secondWork));
        workTable.reset();
        CHECK(workTable.size() == 0u);
        CHECK(workTable.stats().uniqueWorkCount == 0u);
        CHECK(workTable.insert(work).workTableIndex == 0u);

        const std::array interfaces{
            LayeredInterfaceSample{
                .work = work,
                .rayDistanceMeters = 4.0f,
                .orientation = LayeredFaceOrientation::Entry,
            },
            LayeredInterfaceSample{
                .work = work,
                .rayDistanceMeters = 4.75f,
                .orientation = LayeredFaceOrientation::Exit,
            },
        };
        const Ordinary2InterfacePair paired = pairOrdinary2Interfaces(
            interfaces, 0.5f);
        CHECK(paired.paired());
        CHECK(std::abs(paired.measuredChordMeters - 0.75f) < 0.0001f);
        CHECK(std::abs(paired.participatingPathMeters - 0.5f) < 0.0001f);
        CHECK(paired.rejectedInterfaceCount == 0);
        CHECK(layeredFaceOrientation(true, false) ==
            LayeredFaceOrientation::Entry);
        CHECK(layeredFaceOrientation(true, true) ==
            LayeredFaceOrientation::Exit);

        std::array mismatch = interfaces;
        AssetGuid::Bytes otherBytes{};
        otherBytes.back() = 2;
        mismatch[1].work.primitiveGuid = AssetGuid(otherBytes);
        CHECK(pairOrdinary2Interfaces(mismatch, 1.0f).status ==
            Ordinary2PairingStatus::WorkMismatch);

        std::array orientation = interfaces;
        orientation[1].orientation = LayeredFaceOrientation::Entry;
        CHECK(pairOrdinary2Interfaces(orientation, 1.0f).status ==
            Ordinary2PairingStatus::OrientationMismatch);

        std::array reversed = interfaces;
        reversed[1].rayDistanceMeters = 3.0f;
        CHECK(pairOrdinary2Interfaces(reversed, 1.0f).status ==
            Ordinary2PairingStatus::InvalidDistance);

        const std::array overflow{ interfaces[0], interfaces[1],
            interfaces[1] };
        const Ordinary2InterfacePair exceeded = pairOrdinary2Interfaces(
            overflow, 1.0f);
        CHECK(exceeded.status == Ordinary2PairingStatus::CapacityExceeded);
        CHECK(exceeded.rejectedInterfaceCount == 1);
        CHECK(pairOrdinary2Interfaces(interfaces, -1.0f).status ==
            Ordinary2PairingStatus::InvalidThicknessCap);
        return true;
    }

    bool testLayeredQualityTierAndOverflowContract() {
        const LayeredQualityTierContract ordinary =
            layeredQualityTierContract(TransparencyQuality::Ordinary2);
        const LayeredQualityTierContract hero =
            layeredQualityTierContract(TransparencyQuality::Hero4);
        const LayeredQualityTierContract cinematic =
            layeredQualityTierContract(TransparencyQuality::Cinematic8);
        CHECK(ordinary.maximumInterfaceCount == 2u);
        CHECK(hero.maximumInterfaceCount == 4u);
        CHECK(cinematic.maximumInterfaceCount == 8u);
        CHECK(!ordinary.explicitOnly);
        CHECK(hero.explicitOnly);
        CHECK(cinematic.explicitOnly);
        CHECK(ordinary.nonRefractiveResidualTail);
        CHECK(hero.nonRefractiveResidualTail);
        CHECK(cinematic.nonRefractiveResidualTail);
        CHECK(layeredAtlasAreaCapPixels(3840u, 2160u,
            TransparencyQuality::Ordinary2) == 2'073'600u);
        CHECK(layeredAtlasAreaCapPixels(3840u, 2160u,
            TransparencyQuality::Hero4) == 4'147'200u);
        CHECK(layeredAtlasAreaCapPixels(3840u, 2160u,
            TransparencyQuality::Cinematic8) == 8'294'400u);
        CHECK(layeredAtlasAreaCapPixels(0u, 2160u,
            TransparencyQuality::Hero4) == 0u);
        CHECK(layeredEarlyTerminationReached(0.0f));
        CHECK(layeredEarlyTerminationReached(
            kLayeredEarlyTerminationTransmittance));
        CHECK(!layeredEarlyTerminationReached(0.01f));
        CHECK(!layeredEarlyTerminationReached(
            std::numeric_limits<float>::quiet_NaN()));
        CHECK(kDeepLayeredEarlyTerminationTileSize == 16u);
        CHECK(kDeepLayeredWorkMask == 0x1fffu);
        CHECK(kDeepLayeredTransmissionMaximum == 16'383u);
        CHECK(kDeepLayeredTerminationThresholdQuantized == 16u);
        CHECK(deepLayeredTerminationMaskCount(4u) == 1u);
        CHECK(deepLayeredTerminationMaskCount(8u) == 3u);
        CHECK(deepLayeredTerminationInterface(1u, 4u));
        CHECK(!deepLayeredTerminationInterface(3u, 4u));
        CHECK(deepLayeredPriorTerminationInterface(2u) == 1u);
        CHECK(deepLayeredPriorTerminationInterface(3u) == 1u);
        CHECK(deepLayeredPriorTerminationInterface(6u) == 5u);
        const uint32_t packedDeepState = 2u |
            (16u << kDeepLayeredTransmissionShift) |
            (3u << kDeepLayeredOpenCountShift) |
            kLayeredInterfaceOrientationBit;
        CHECK(deepLayeredWorkIdentity(packedDeepState) == 2u);
        CHECK(deepLayeredTransmissionQuantized(packedDeepState) == 16u);
        CHECK(deepLayeredOpenCount(packedDeepState) == 3u);

        const auto work = [](uint8_t suffix) {
            AssetGuid::Bytes bytes{};
            bytes.back() = suffix;
            return TransparentWorkIdentity{
                .primitiveGuid = AssetGuid(bytes),
            };
        };
        const TransparentWorkIdentity workA = work(1u);
        const TransparentWorkIdentity workB = work(2u);
        const TransparentWorkIdentity workC = work(3u);
        const std::array nested{
            LayeredInterfaceSample{ workA, 1.0f,
                LayeredFaceOrientation::Entry },
            LayeredInterfaceSample{ workB, 2.0f,
                LayeredFaceOrientation::Entry },
            LayeredInterfaceSample{ workB, 3.0f,
                LayeredFaceOrientation::Exit },
            LayeredInterfaceSample{ workA, 4.0f,
                LayeredFaceOrientation::Exit },
        };
        const LayeredInterfaceStackReduction heroExact =
            reduceLayeredInterfaceStack(nested,
                TransparencyQuality::Hero4);
        CHECK(heroExact.accepted());
        CHECK(heroExact.status == LayeredInterfaceStackStatus::Exact);
        CHECK(heroExact.exactInterfaceCount == 4u);
        CHECK(heroExact.residualInterfaceCount == 0u);
        CHECK(heroExact.pairedVolumeCount == 2u);
        CHECK(heroExact.accountedInterfaceCount() == nested.size());

        const LayeredInterfaceStackReduction ordinaryOverflow =
            reduceLayeredInterfaceStack(nested,
                TransparencyQuality::Ordinary2);
        CHECK(ordinaryOverflow.accepted());
        CHECK(ordinaryOverflow.requiresResidualTail());
        CHECK(ordinaryOverflow.exactInterfaceCount == 2u);
        CHECK(ordinaryOverflow.residualInterfaceCount == 2u);
        CHECK(ordinaryOverflow.accountedInterfaceCount() == nested.size());
        CHECK(ordinaryOverflow.exactPrefixEndsInsideVolume);
        CHECK(ordinaryOverflow.residualIsNonRefractive);
        CHECK(ordinaryOverflow.residualStartMeters == 3.0f);
        CHECK(ordinaryOverflow.residualEndMeters == 4.0f);

        const std::array crossing{
            nested[0], nested[1],
            LayeredInterfaceSample{ workA, 3.0f,
                LayeredFaceOrientation::Exit },
            LayeredInterfaceSample{ workB, 4.0f,
                LayeredFaceOrientation::Exit },
        };
        const LayeredInterfaceStackReduction crossingHero =
            reduceLayeredInterfaceStack(crossing,
                TransparencyQuality::Hero4);
        CHECK(crossingHero.status == LayeredInterfaceStackStatus::Exact);
        CHECK(crossingHero.pairedVolumeCount == 2u);

        const std::array threeVolumes{
            LayeredInterfaceSample{ workA, 1.0f,
                LayeredFaceOrientation::Entry },
            LayeredInterfaceSample{ workA, 2.0f,
                LayeredFaceOrientation::Exit },
            LayeredInterfaceSample{ workB, 3.0f,
                LayeredFaceOrientation::Entry },
            LayeredInterfaceSample{ workB, 4.0f,
                LayeredFaceOrientation::Exit },
            LayeredInterfaceSample{ workC, 5.0f,
                LayeredFaceOrientation::Entry },
            LayeredInterfaceSample{ workC, 6.0f,
                LayeredFaceOrientation::Exit },
        };
        const LayeredInterfaceStackReduction heroOverflow =
            reduceLayeredInterfaceStack(threeVolumes,
                TransparencyQuality::Hero4);
        CHECK(heroOverflow.status ==
            LayeredInterfaceStackStatus::OverflowResidual);
        CHECK(heroOverflow.exactInterfaceCount == 4u);
        CHECK(heroOverflow.residualInterfaceCount == 2u);
        CHECK(!heroOverflow.exactPrefixEndsInsideVolume);
        CHECK(reduceLayeredInterfaceStack(threeVolumes,
            TransparencyQuality::Cinematic8).status ==
            LayeredInterfaceStackStatus::Exact);

        std::array repeatedEntry = nested;
        repeatedEntry[1].work = workA;
        CHECK(reduceLayeredInterfaceStack(repeatedEntry,
            TransparencyQuality::Hero4).status ==
            LayeredInterfaceStackStatus::RepeatedEntryWithoutExit);
        const std::array missingEntry{
            LayeredInterfaceSample{ workA, 1.0f,
                LayeredFaceOrientation::Exit },
        };
        CHECK(reduceLayeredInterfaceStack(missingEntry,
            TransparencyQuality::Ordinary2).status ==
            LayeredInterfaceStackStatus::MissingEntry);
        const std::array incomplete{
            LayeredInterfaceSample{ workA, 1.0f,
                LayeredFaceOrientation::Entry },
        };
        CHECK(reduceLayeredInterfaceStack(incomplete,
            TransparencyQuality::Ordinary2).status ==
            LayeredInterfaceStackStatus::IncompleteVolume);
        std::array invalidDistance = nested;
        invalidDistance[2].rayDistanceMeters = 2.0f;
        CHECK(reduceLayeredInterfaceStack(invalidDistance,
            TransparencyQuality::Hero4).status ==
            LayeredInterfaceStackStatus::InvalidDistance);
        CHECK(reduceLayeredInterfaceStack(nested,
            static_cast<TransparencyQuality>(3u)).status ==
            LayeredInterfaceStackStatus::InvalidQuality);
        return true;
    }

    bool testLayeredTierAwareAtlasPreparation() {
        CHECK((layeredAtlasCapacityExtent(3840u, 2160u,
            TransparencyQuality::Ordinary2) ==
            Ordinary2AtlasExtent{ 3840u, 528u }));
        CHECK((layeredAtlasCapacityExtent(3840u, 2160u,
            TransparencyQuality::Hero4) ==
            Ordinary2AtlasExtent{ 3840u, 1072u }));
        CHECK((layeredAtlasCapacityExtent(3840u, 2160u,
            TransparencyQuality::Cinematic8) ==
            Ordinary2AtlasExtent{ 3840u, 2160u }));
        CHECK(layeredAtlasCapacityExtent(1280u, 720u,
            static_cast<TransparencyQuality>(3u)).empty());

        const auto work = [](uint8_t suffix) {
            AssetGuid::Bytes bytes{};
            bytes.back() = suffix;
            return TransparentWorkIdentity{
                .primitiveGuid = AssetGuid(bytes),
            };
        };
        const TransparentWorkIdentity workA = work(1u);
        const TransparentWorkIdentity workB = work(2u);
        const TransparentWorkIdentity workC = work(3u);
        const TransparentWorkIdentity workD = work(4u);
        const TransparentWorkIdentity workInvalidRect = work(5u);
        const TransparentWorkIdentity workInvalidQuality = work(6u);
        const TransparentWorkIdentity workConflict = work(7u);
        const std::array requests{
            LayeredAtlasRequest{ workA, { 64u, 0u, 96u, 16u }, 10u, 0,
                TransparencyQuality::Ordinary2 },
            LayeredAtlasRequest{ workC, { 0u, 0u, 128u, 32u }, 30u, -1,
                TransparencyQuality::Cinematic8 },
            LayeredAtlasRequest{ workA, { 64u, 16u, 96u, 32u }, 11u, 0,
                TransparencyQuality::Ordinary2 },
            LayeredAtlasRequest{ workB, { 16u, 0u, 48u, 16u }, 20u, 5,
                TransparencyQuality::Hero4 },
            LayeredAtlasRequest{ workD, { 0u, 64u, 128u, 96u }, 40u, -2,
                TransparencyQuality::Ordinary2 },
            LayeredAtlasRequest{ workInvalidRect, { 5u, 5u, 5u, 9u },
                50u, 9, TransparencyQuality::Hero4 },
            LayeredAtlasRequest{ workInvalidQuality, { 0u, 0u, 16u, 16u },
                60u, 9, static_cast<TransparencyQuality>(3u) },
            LayeredAtlasRequest{ workConflict, { 0u, 0u, 16u, 16u },
                70u, 8, TransparencyQuality::Ordinary2 },
            LayeredAtlasRequest{ workConflict, { 16u, 0u, 32u, 16u },
                71u, 8, TransparencyQuality::Hero4 },
        };

        BoundedLayeredAtlasPlan<12> plan;
        CHECK(plan.prepare(requests, 128u, 128u) ==
            LayeredAtlasPreparationStatus::Prepared);
        CHECK(plan.decisions().size() == requests.size());
        CHECK(plan.decisions()[0].accepted());
        CHECK(plan.decisions()[2].accepted());
        CHECK(plan.decisions()[3].accepted());
        CHECK(plan.decisions()[1].accepted());
        CHECK(plan.decisions()[4].status ==
            LayeredAtlasDecisionStatus::TierAtlasCapacityExceeded);
        CHECK(plan.decisions()[5].status ==
            LayeredAtlasDecisionStatus::InvalidScreenRect);
        CHECK(plan.decisions()[6].status ==
            LayeredAtlasDecisionStatus::InvalidQuality);
        CHECK(plan.decisions()[7].status ==
            LayeredAtlasDecisionStatus::ConflictingWorkQuality);
        CHECK(plan.decisions()[8].status ==
            LayeredAtlasDecisionStatus::ConflictingWorkQuality);
        CHECK(plan.decisions()[0].placement.quality ==
            TransparencyQuality::Ordinary2);
        CHECK((plan.decisions()[0].placement.screenRect ==
            Ordinary2ScreenRect{ 64u, 0u, 96u, 32u }));
        CHECK(plan.decisions()[0].placement.atlasX == 0u);
        CHECK(plan.decisions()[3].placement.atlasX == 0u);
        CHECK(plan.decisions()[1].placement.atlasX == 0u);
        CHECK(plan.decisions()[0].placement.workTableIndex == 1u);
        CHECK(plan.decisions()[2].placement.workTableIndex == 1u);
        CHECK(plan.workIdentities().size() == 3u);
        CHECK(plan.workIdentities()[0] == workB);
        CHECK(plan.workIdentities()[1] == workA);
        CHECK(plan.workIdentities()[2] == workC);
        CHECK(plan.workTableStats().uniqueWorkCount == 3u);
        CHECK(plan.workTableStats().duplicateWorkCount == 0u);

        CHECK(plan.topology().size() == kLayeredQualityTierCount);
        CHECK((plan.topology()[0].capacityExtent ==
            Ordinary2AtlasExtent{ 128u, 32u }));
        CHECK((plan.topology()[1].capacityExtent ==
            Ordinary2AtlasExtent{ 128u, 64u }));
        CHECK((plan.topology()[2].capacityExtent ==
            Ordinary2AtlasExtent{ 128u, 128u }));
        CHECK(plan.topology()[0].maximumInterfaceCount == 2u);
        CHECK(plan.topology()[1].maximumInterfaceCount == 4u);
        CHECK(plan.topology()[2].maximumInterfaceCount == 8u);
        CHECK(plan.topology()[0].active);
        CHECK(plan.topology()[1].active);
        CHECK(plan.topology()[2].active);
        CHECK(plan.topology()[0].interfaceStorageTexelCount() == 8192u);
        CHECK(plan.topology()[1].interfaceStorageTexelCount() == 32768u);
        CHECK(plan.topology()[2].interfaceStorageTexelCount() == 131072u);

        CHECK(plan.tierStats()[0].acceptedPacketCount == 2u);
        CHECK(plan.tierStats()[0].acceptedIslandCount == 1u);
        CHECK(plan.tierStats()[0].duplicatePacketCount == 1u);
        CHECK(plan.tierStats()[0].capacityRejectedPacketCount == 1u);
        CHECK(plan.tierStats()[0].allocatedTexelCount == 1024u);
        CHECK(plan.tierStats()[0].allocatedInterfaceTexelCount == 2048u);
        CHECK(plan.tierStats()[1].allocatedTexelCount == 512u);
        CHECK(plan.tierStats()[1].allocatedInterfaceTexelCount == 2048u);
        CHECK(plan.tierStats()[2].allocatedTexelCount == 4096u);
        CHECK(plan.tierStats()[2].allocatedInterfaceTexelCount == 32768u);
        CHECK(plan.stats().requestCount == requests.size());
        CHECK(plan.stats().validRequestCount == 7u);
        CHECK(plan.stats().acceptedPacketCount == 4u);
        CHECK(plan.stats().acceptedIslandCount == 3u);
        CHECK(plan.stats().duplicatePacketCount == 1u);
        CHECK(plan.stats().invalidQualityCount == 1u);
        CHECK(plan.stats().invalidRectCount == 1u);
        CHECK(plan.stats().conflictingQualityPacketCount == 2u);
        CHECK(plan.stats().allocatedTexelCount == 5632u);
        CHECK(plan.stats().allocatedInterfaceTexelCount == 36864u);
        CHECK(plan.stats().activeTierMask == 0x7u);
        CHECK(plan.stats().maximumActiveInterfaceCount == 8u);

        const TransparentWorkIdentity nestedA = work(20u);
        const TransparentWorkIdentity nestedB = work(21u);
        const TransparentWorkIdentity nestedC = work(22u);
        const TransparentWorkIdentity disjoint = work(23u);
        const std::array overlapRequests{
            LayeredAtlasRequest{ nestedA, { 0u, 0u, 32u, 32u }, 100u, 3,
                TransparencyQuality::Hero4 },
            LayeredAtlasRequest{ nestedB, { 16u, 0u, 48u, 32u }, 101u, 2,
                TransparencyQuality::Hero4 },
            LayeredAtlasRequest{ nestedC, { 40u, 0u, 72u, 32u }, 102u, 1,
                TransparencyQuality::Hero4 },
            LayeredAtlasRequest{ disjoint, { 96u, 64u, 112u, 80u },
                103u, 0, TransparencyQuality::Hero4 },
        };
        BoundedLayeredAtlasPlan<8> overlapPlan;
        CHECK(overlapPlan.prepare(overlapRequests, 128u, 128u) ==
            LayeredAtlasPreparationStatus::Prepared);
        CHECK(overlapPlan.stats().acceptedPacketCount == 4u);
        CHECK(overlapPlan.stats().acceptedWorkCount == 4u);
        CHECK(overlapPlan.stats().acceptedIslandCount == 2u);
        CHECK(overlapPlan.stats().multiWorkIslandCount == 1u);
        CHECK(overlapPlan.stats().overlapMergedWorkCount == 2u);
        CHECK(overlapPlan.stats().allocatedTexelCount == 2816u);
        CHECK(overlapPlan.stats().allocatedInterfaceTexelCount == 11264u);
        CHECK(overlapPlan.tierStats()[1].acceptedWorkCount == 4u);
        CHECK(overlapPlan.tierStats()[1].multiWorkIslandCount == 1u);
        CHECK(overlapPlan.tierStats()[1].overlapMergedWorkCount == 2u);
        CHECK((overlapPlan.decisions()[0].placement.screenRect ==
            Ordinary2ScreenRect{ 0u, 0u, 72u, 32u }));
        CHECK(overlapPlan.decisions()[0].placement.atlasX == 0u);
        CHECK(overlapPlan.decisions()[1].placement.atlasX == 0u);
        CHECK(overlapPlan.decisions()[2].placement.atlasX == 0u);
        CHECK(overlapPlan.decisions()[0].placement.workTableIndex == 0u);
        CHECK(overlapPlan.decisions()[1].placement.workTableIndex == 1u);
        CHECK(overlapPlan.decisions()[2].placement.workTableIndex == 2u);
        CHECK(overlapPlan.decisions()[3].placement.atlasX == 80u);
        CHECK(overlapPlan.decisions()[3].placement.workTableIndex == 3u);
        const std::array overlapReordered{
            overlapRequests[3], overlapRequests[2],
            overlapRequests[1], overlapRequests[0],
        };
        BoundedLayeredAtlasPlan<8> overlapReorderedPlan;
        CHECK(overlapReorderedPlan.prepare(overlapReordered, 128u, 128u) ==
            LayeredAtlasPreparationStatus::Prepared);
        CHECK(overlapReorderedPlan.stats().acceptedIslandCount == 2u);
        CHECK(overlapReorderedPlan.stats().overlapMergedWorkCount == 2u);
        CHECK(std::equal(overlapReorderedPlan.workIdentities().begin(),
            overlapReorderedPlan.workIdentities().end(),
            overlapPlan.workIdentities().begin()));
        CHECK(overlapReorderedPlan.decisions()[3].placement.atlasX == 0u);
        CHECK(overlapReorderedPlan.decisions()[2].placement.atlasX == 0u);
        CHECK(overlapReorderedPlan.decisions()[1].placement.atlasX == 0u);
        CHECK(overlapReorderedPlan.decisions()[0].placement.atlasX == 80u);

        const auto makePacket = [&](uint8_t suffix,
                TransparencyQuality quality, glm::vec3 boundsMin,
                glm::vec3 boundsMax) {
            DrawPacket packet{};
            packet.worldTransform = glm::mat4(1.0f);
            packet.boundsMinWorld = boundsMin;
            packet.boundsMaxWorld = boundsMax;
            packet.transparentWorkFlags = TransparentWorkIntervalValid;
            packet.transparencyExecutionMode =
                TransparencyExecutionMode::Classified;
            packet.transparency.resolvedClass =
                TransparencyClass::LayeredGlass;
            packet.transparency.quality = quality;
            packet.primitiveGuid = work(suffix).primitiveGuid;
            return packet;
        };
        const std::array packets{
            makePacket(30u, TransparencyQuality::Hero4,
                { -0.8f, -0.2f, 0.1f }, { 0.1f, 0.2f, 0.3f }),
            makePacket(31u, TransparencyQuality::Hero4,
                { -0.1f, -0.2f, 0.1f }, { 0.8f, 0.2f, 0.3f }),
            makePacket(32u, TransparencyQuality::Cinematic8,
                { -0.4f, -0.2f, 0.1f }, { 0.4f, 0.2f, 0.3f }),
            makePacket(33u, TransparencyQuality::Ordinary2,
                { -0.2f, -0.2f, 0.1f }, { 0.2f, 0.2f, 0.3f }),
        };
        BoundedLayeredRequestCollector<8> collector;
        collector.collect(packets, glm::mat4(1.0f), 128u, 128u,
            (1u << layeredQualityTierIndex(TransparencyQuality::Hero4)) |
            (1u << layeredQualityTierIndex(
                TransparencyQuality::Cinematic8)));
        CHECK(collector.stats().inspectedPacketCount == 4u);
        CHECK(collector.stats().candidatePacketCount == 3u);
        CHECK(collector.stats().projectedPacketCount == 3u);
        CHECK(collector.requests().size() == 3u);
        BoundedLayeredAtlasPlan<8> collectedAtlas;
        CHECK(collectedAtlas.prepare(collector.requests(), 128u, 128u) ==
            LayeredAtlasPreparationStatus::Prepared);
        CHECK(collectedAtlas.stats().acceptedPacketCount == 3u);
        CHECK(collectedAtlas.stats().acceptedIslandCount == 2u);
        const std::array<Ordinary2AtlasExtent,
            kLayeredQualityTierCount> residentExtents{
            Ordinary2AtlasExtent{},
            Ordinary2AtlasExtent{ 128u, 64u },
            Ordinary2AtlasExtent{ 128u, 128u },
        };
        BoundedLayeredCaptureDrawPlan<8> drawPlan;
        CHECK(drawPlan.prepare(collectedAtlas.decisions(), packets,
            residentExtents) ==
            LayeredCaptureDrawPreparationStatus::Prepared);
        CHECK(drawPlan.stats().acceptedDecisionCount == 3u);
        CHECK(drawPlan.stats().preparedDrawCount == 3u);
        CHECK(drawPlan.draws()[0].quality == TransparencyQuality::Hero4);
        CHECK(drawPlan.draws()[1].quality == TransparencyQuality::Hero4);
        CHECK(drawPlan.draws()[2].quality ==
            TransparencyQuality::Cinematic8);
        CHECK(drawPlan.draws()[0].atlasX == drawPlan.draws()[1].atlasX);
        CHECK(drawPlan.draws()[0].workTableIndex !=
            drawPlan.draws()[1].workTableIndex);
        const std::array<Ordinary2AtlasExtent,
            kLayeredQualityTierCount> noResidentExtents{};
        CHECK(drawPlan.prepare(collectedAtlas.decisions(), packets,
            noResidentExtents) ==
            LayeredCaptureDrawPreparationStatus::Empty);
        CHECK(drawPlan.stats().invalidPlacementCount == 3u);

        beginCpuAllocationFrame();
        for (uint32_t iteration = 0u; iteration < 100u; ++iteration) {
            (void)plan.prepare(requests, 128u, 128u);
            collector.collect(packets, glm::mat4(1.0f), 128u, 128u, 0x6u);
            (void)collectedAtlas.prepare(collector.requests(), 128u, 128u);
            (void)drawPlan.prepare(collectedAtlas.decisions(), packets,
                residentExtents);
        }
        const CpuAllocationFrameSample allocationSample =
            endCpuAllocationFrame();
        CHECK(allocationSample.allocationCount == 0u);
        CHECK(allocationSample.requestedBytes == 0u);

        const std::array reordered{
            requests[8], requests[7], requests[6], requests[5], requests[4],
            requests[3], requests[2], requests[1], requests[0],
        };
        BoundedLayeredAtlasPlan<12> reorderedPlan;
        CHECK(reorderedPlan.prepare(reordered, 128u, 128u) ==
            LayeredAtlasPreparationStatus::Prepared);
        CHECK(std::equal(reorderedPlan.workIdentities().begin(),
            reorderedPlan.workIdentities().end(),
            plan.workIdentities().begin()));
        const auto placementForPacket = [](const auto& candidate,
                uint32_t packetIndex) {
            for (const LayeredAtlasDecision& decision : candidate.decisions()) {
                if (decision.accepted() &&
                    decision.placement.packetIndex == packetIndex) {
                    return decision.placement;
                }
            }
            return LayeredAtlasPlacement{};
        };
        CHECK(placementForPacket(reorderedPlan, 10u).atlasX ==
            placementForPacket(plan, 10u).atlasX);
        CHECK(placementForPacket(reorderedPlan, 20u).workTableIndex ==
            placementForPacket(plan, 20u).workTableIndex);
        CHECK(reorderedPlan.stats().allocatedInterfaceTexelCount ==
            plan.stats().allocatedInterfaceTexelCount);

        BoundedLayeredAtlasPlan<2> overflowPlan;
        CHECK(overflowPlan.prepare(
            std::span<const LayeredAtlasRequest>{ requests.data(), 3u },
            128u, 128u) ==
            LayeredAtlasPreparationStatus::RequestCapacityExceeded);
        CHECK(overflowPlan.stats().requestCapacityRejectedCount == 3u);
        CHECK(plan.prepare(requests, 0u, 128u) ==
            LayeredAtlasPreparationStatus::InvalidSceneExtent);
        CHECK(plan.prepare(requests, 15u, 15u) ==
            LayeredAtlasPreparationStatus::AtlasUnavailable);
        CHECK(plan.prepare({}, 128u, 128u) ==
            LayeredAtlasPreparationStatus::Empty);
        return true;
    }

    bool testOrdinary2BoundedAtlasPreparation() {
        CHECK((ordinary2AtlasCapacityExtent(3840u, 2160u) ==
            Ordinary2AtlasExtent{ 3840u, 528u }));
        CHECK((ordinary2AtlasCapacityExtent(1280u, 720u) ==
            Ordinary2AtlasExtent{ 1280u, 176u }));
        CHECK(ordinary2AtlasCapacityExtent(15u, 720u).empty());
        CHECK(ordinary2AtlasCapacityExtent(1280u, 15u).empty());
        CHECK(ordinary2AtlasCapacityExtent(3840u, 2160u).area() <=
            static_cast<uint64_t>(3840u) * 2160u / 4u);

        const auto work = [](uint8_t suffix) {
            AssetGuid::Bytes bytes{};
            bytes.back() = suffix;
            return TransparentWorkIdentity{
                .primitiveGuid = AssetGuid(bytes),
            };
        };
        const TransparentWorkIdentity workA = work(1u);
        const TransparentWorkIdentity workB = work(2u);
        const TransparentWorkIdentity workC = work(3u);
        const TransparentWorkIdentity workInvalid = work(4u);
        const std::array requests{
            Ordinary2AtlasRequest{ workA, { 64u, 0u, 96u, 16u }, 10u, 0 },
            Ordinary2AtlasRequest{ workC, { 0u, 0u, 128u, 32u }, 30u, -1 },
            Ordinary2AtlasRequest{ workA, { 64u, 16u, 96u, 32u }, 11u, 0 },
            Ordinary2AtlasRequest{ workB, { 16u, 0u, 48u, 16u }, 20u, 5 },
            Ordinary2AtlasRequest{ workInvalid, { 5u, 5u, 5u, 9u }, 40u, 9 },
        };
        BoundedOrdinary2AtlasPlan<8> plan;
        CHECK(plan.prepare(requests, 128u, 128u) ==
            Ordinary2AtlasPreparationStatus::Prepared);
        CHECK((plan.atlasExtent() == Ordinary2AtlasExtent{ 128u, 32u }));
        CHECK(plan.decisions().size() == requests.size());
        CHECK(plan.decisions()[0].accepted());
        CHECK(plan.decisions()[2].accepted());
        CHECK(plan.decisions()[3].accepted());
        CHECK(plan.decisions()[1].status ==
            Ordinary2AtlasDecisionStatus::AtlasCapacityExceeded);
        CHECK(plan.decisions()[4].status ==
            Ordinary2AtlasDecisionStatus::InvalidScreenRect);
        CHECK(plan.decisions()[0].placement.workTableIndex ==
            plan.decisions()[2].placement.workTableIndex);
        CHECK((plan.decisions()[0].placement.screenRect ==
            Ordinary2ScreenRect{ 64u, 0u, 96u, 32u }));
        CHECK(plan.decisions()[0].placement.viewportOffsetX == 32);
        CHECK(plan.decisions()[3].placement.workTableIndex == 0u);
        CHECK(plan.decisions()[3].placement.viewportOffsetX == 16);
        CHECK(plan.workIdentities().size() == 2u);
        CHECK(plan.workIdentities()[0] == workB);
        CHECK(plan.workIdentities()[1] == workA);
        CHECK(plan.workTableStats().uniqueWorkCount == 2u);
        CHECK(plan.workTableStats().duplicateWorkCount == 1u);
        CHECK(plan.stats().requestCount == 5u);
        CHECK(plan.stats().validRequestCount == 4u);
        CHECK(plan.stats().acceptedPacketCount == 3u);
        CHECK(plan.stats().acceptedIslandCount == 2u);
        CHECK(plan.stats().duplicatePacketCount == 1u);
        CHECK(plan.stats().invalidRectCount == 1u);
        CHECK(plan.stats().atlasRejectedPacketCount == 1u);
        CHECK(plan.stats().allocatedTexelCount == 1536u);

        beginCpuAllocationFrame();
        for (uint32_t iteration = 0; iteration < 100u; ++iteration)
            (void)plan.prepare(requests, 128u, 128u);
        const CpuAllocationFrameSample allocationSample =
            endCpuAllocationFrame();
        CHECK(allocationSample.allocationCount == 0u);
        CHECK(allocationSample.requestedBytes == 0u);

        const std::array reordered{
            requests[4], requests[3], requests[2], requests[1], requests[0] };
        BoundedOrdinary2AtlasPlan<8> reorderedPlan;
        CHECK(reorderedPlan.prepare(reordered, 128u, 128u) ==
            Ordinary2AtlasPreparationStatus::Prepared);
        CHECK(reorderedPlan.workIdentities().size() ==
            plan.workIdentities().size());
        CHECK(std::equal(reorderedPlan.workIdentities().begin(),
            reorderedPlan.workIdentities().end(),
            plan.workIdentities().begin()));
        const auto placementForPacket = [](const auto& candidate,
                uint32_t packetIndex) {
            for (const Ordinary2AtlasDecision& decision :
                    candidate.decisions())
                if (decision.accepted() &&
                    decision.placement.packetIndex == packetIndex)
                    return decision.placement;
            return Ordinary2AtlasPlacement{};
        };
        CHECK(placementForPacket(reorderedPlan, 10u).atlasX ==
            placementForPacket(plan, 10u).atlasX);
        CHECK(placementForPacket(reorderedPlan, 20u).workTableIndex ==
            placementForPacket(plan, 20u).workTableIndex);

        BoundedOrdinary2AtlasPlan<2> overflowPlan;
        CHECK(overflowPlan.prepare(
            std::span<const Ordinary2AtlasRequest>{ requests.data(), 3u },
            128u, 128u) ==
            Ordinary2AtlasPreparationStatus::RequestCapacityExceeded);
        CHECK(overflowPlan.stats().requestCapacityRejectedCount == 3u);
        return true;
    }

    bool testOrdinary2CaptureValidationResultContract() {
        Ordinary2CaptureValidationResult result{
            .validationId = 7u,
            .atlasWidth = 1280u,
            .atlasHeight = 176u,
            .expectedDrawCount = 1u,
            .workItemCount = 1u,
            .inspectedPixelCount = 225'280u,
            .entryPixelCount = 4'128u,
            .exitPixelCount = 4'128u,
            .pairedPixelCount = 4'128u,
            .localColorPixelCount = 4'128u,
            .minimumPairedDepthDelta = 0.0000026226f,
            .maximumPairedDepthDelta = 0.00340205f,
            .minimumLocalAlpha = 0.75f,
            .maximumLocalAlpha = 1.0f,
        };
        CHECK(result.passed());
        result.workMismatchPixelCount = 1u;
        CHECK(!result.passed());
        result.workMismatchPixelCount = 0u;
        result.nonIncreasingDepthPixelCount = 1u;
        CHECK(!result.passed());
        result.nonIncreasingDepthPixelCount = 0u;
        result.pairedPixelCount = result.exitPixelCount - 1u;
        CHECK(!result.passed());
        result.pairedPixelCount = result.exitPixelCount;
        result.localColorInvalidPixelCount = 1u;
        CHECK(!result.passed());
        return true;
    }

    bool testDeepLayeredCaptureValidationResultContract() {
        DeepLayeredCaptureValidationResult result{
            .validationId = 11u,
            .quality = TransparencyQuality::Hero4,
            .atlasWidth = 1280u,
            .atlasHeight = 352u,
            .interfaceCount = 4u,
            .expectedDrawCount = 2u,
            .sceneResolveDrawCount = 2u,
            .compatibilityForwardDrawCount = 0u,
            .workItemCount = 2u,
            .maximumObservedInterfaceCount = 4u,
            .inspectedPixelCount = 450'560u,
            .interfacePixelCounts = { 8'000u, 8'000u, 4'000u, 4'000u },
            .pairedPixelCount = 8'000u,
            .nestedFourInterfacePixelCount = 4'000u,
            .localColorPixelCount = 8'000u,
            .minimumDepthDelta = 0.000002f,
            .maximumDepthDelta = 0.004f,
            .minimumLocalAlpha = 0.50f,
            .maximumLocalAlpha = 1.0f,
        };
        CHECK(result.passed());
        result.crossingPairPixelCount = 512u;
        CHECK(result.passed());
        result.saturatedResidualPixelCount = 32u;
        CHECK(result.passed());
        result.interfaceGapPixelCount = 1u;
        CHECK(!result.passed());
        result.interfaceGapPixelCount = 0u;
        result.nestedFourInterfacePixelCount = 0u;
        CHECK(!result.passed());
        result.nestedFourInterfacePixelCount = 4'000u;
        result.maximumObservedInterfaceCount = 2u;
        result.interfacePixelCounts[3] = 0u;
        result.earlyTerminatedPixelCount = 4'000u;
        result.terminatedOccupiedTileCount = 16u;
        CHECK(result.passed());
        result.maximumObservedInterfaceCount = 4u;
        result.interfacePixelCounts[3] = 4'000u;
        result.earlyTerminatedPixelCount = 0u;
        result.terminatedOccupiedTileCount = 0u;
        result.localColorPixelCount = result.pairedPixelCount - 1u;
        CHECK(!result.passed());
        result.localColorPixelCount = result.pairedPixelCount;
        result.sceneResolveDrawCount = 1u;
        CHECK(!result.passed());
        result.sceneResolveDrawCount = 2u;
        result.compatibilityForwardDrawCount = 1u;
        CHECK(!result.passed());
        result.compatibilityForwardDrawCount = 0u;
        result.quality = TransparencyQuality::Cinematic8;
        result.interfaceCount = 8u;
        result.maximumObservedInterfaceCount = 8u;
        result.expectedDrawCount = 4u;
        result.sceneResolveDrawCount = 4u;
        result.workItemCount = 4u;
        result.interfacePixelCounts[7] = 1u;
        CHECK(result.passed());
        result.maximumObservedInterfaceCount = 7u;
        CHECK(!result.passed());
        result.maximumObservedInterfaceCount = 8u;
        result.interfacePixelCounts[7] = 0u;
        CHECK(!result.passed());
        result.quality = TransparencyQuality::Hero4;
        result.interfaceCount = 4u;
        result.maximumObservedInterfaceCount = 4u;
        result.expectedDrawCount = 2u;
        result.sceneResolveDrawCount = 2u;
        result.workItemCount = 2u;
        result.quality = TransparencyQuality::Ordinary2;
        CHECK(!result.passed());
        return true;
    }

    bool testOrdinary2ProjectionAndRequestCollection() {
        constexpr uint32_t WorkValid = TransparentWorkIntervalValid;
        const Ordinary2ProjectionResult centered =
            projectOrdinary2WorldBounds(glm::vec3(-0.5f), glm::vec3(0.5f),
                WorkValid, glm::mat4(1.0f), 100u, 100u);
        CHECK(centered.accepted());
        CHECK((centered.screenRect ==
            Ordinary2ScreenRect{ 24u, 24u, 76u, 76u }));
        CHECK(projectOrdinary2WorldBounds(
            { 2.0f, -0.5f, -0.5f }, { 3.0f, 0.5f, 0.5f }, WorkValid,
            glm::mat4(1.0f), 100u, 100u).status ==
            Ordinary2ProjectionStatus::Culled);
        CHECK(projectOrdinary2WorldBounds(glm::vec3(-0.5f),
            glm::vec3(0.5f), WorkValid | TransparentWorkNearClipped,
            glm::mat4(1.0f), 100u, 100u).status ==
            Ordinary2ProjectionStatus::NearPlaneFallback);
        CHECK(projectOrdinary2WorldBounds(glm::vec3(1.0f),
            glm::vec3(-1.0f), WorkValid, glm::mat4(1.0f), 100u, 100u).status ==
            Ordinary2ProjectionStatus::InvalidBounds);
        glm::mat4 unsafeProjection(1.0f);
        unsafeProjection[0][0] =
            std::numeric_limits<float>::quiet_NaN();
        CHECK(projectOrdinary2WorldBounds(glm::vec3(-0.5f),
            glm::vec3(0.5f), WorkValid, unsafeProjection, 100u, 100u).status ==
            Ordinary2ProjectionStatus::UnsafeProjection);

        const auto packet = [](uint8_t identitySuffix, int32_t priority) {
            DrawPacket result{};
            result.boundsMinWorld = glm::vec3(-0.5f);
            result.boundsMaxWorld = glm::vec3(0.5f);
            result.transparentWorkFlags = TransparentWorkIntervalValid;
            result.transparencyExecutionMode =
                TransparencyExecutionMode::Classified;
            result.transparency.resolvedClass =
                TransparencyClass::LayeredGlass;
            result.transparency.quality = TransparencyQuality::Ordinary2;
            result.transparency.priority = priority;
            AssetGuid::Bytes bytes{};
            bytes.back() = identitySuffix;
            result.primitiveGuid = AssetGuid(bytes);
            return result;
        };
        std::array packets{
            packet(1u, 1), packet(2u, 5), packet(3u, 3), packet(4u, 8),
            packet(5u, 9),
        };
        packets[3].transparency.resolvedClass =
            TransparencyClass::ThinGlass;
        packets[4].transparentWorkFlags |= TransparentWorkNearClipped;
        BoundedOrdinary2RequestCollector<2> collector;
        collector.collect(packets, glm::mat4(1.0f), 100u, 100u);
        CHECK(collector.stats().inspectedPacketCount == 5u);
        CHECK(collector.stats().candidatePacketCount == 4u);
        CHECK(collector.stats().projectedPacketCount == 3u);
        CHECK(collector.stats().nearPlaneFallbackCount == 1u);
        CHECK(collector.stats().requestCapacityFallbackCount == 1u);
        CHECK(collector.requests().size() == 2u);
        CHECK(std::ranges::count_if(collector.requests(),
            [](const Ordinary2AtlasRequest& request) {
                return request.priority == 5;
            }) == 1);
        CHECK(std::ranges::count_if(collector.requests(),
            [](const Ordinary2AtlasRequest& request) {
                return request.priority == 3;
            }) == 1);

        const auto acceptedDecision = [](uint32_t packetIndex,
                uint32_t workIndex, uint32_t atlasX, uint32_t atlasY) {
            Ordinary2AtlasDecision decision{};
            decision.status = Ordinary2AtlasDecisionStatus::Accepted;
            decision.placement = {
                .screenRect = { atlasX, atlasY, atlasX + 16u, atlasY + 16u },
                .atlasX = atlasX,
                .atlasY = atlasY,
                .width = 16u,
                .height = 16u,
                .viewportOffsetX = static_cast<int32_t>(atlasX),
                .viewportOffsetY = static_cast<int32_t>(atlasY),
                .workTableIndex = workIndex,
                .packetIndex = packetIndex,
            };
            return decision;
        };
        std::array captureDecisions{
            acceptedDecision(2u, 2u, 32u, 0u),
            acceptedDecision(1u, 1u, 0u, 0u),
            acceptedDecision(99u, 3u, 0u, 16u),
            acceptedDecision(3u, 4u, 16u, 16u),
            acceptedDecision(1u, 5u, 64u, 0u),
        };
        BoundedOrdinary2CaptureDrawPlan<8> capturePlan;
        CHECK(capturePlan.prepare(captureDecisions, packets,
            { 64u, 32u }) ==
            Ordinary2CaptureDrawPreparationStatus::Prepared);
        CHECK(capturePlan.draws().size() == 2u);
        CHECK(capturePlan.draws()[0].packetIndex == 1u);
        CHECK(capturePlan.draws()[1].packetIndex == 2u);
        CHECK(capturePlan.stats().acceptedDecisionCount == 5u);
        CHECK(capturePlan.stats().preparedDrawCount == 2u);
        CHECK(capturePlan.stats().invalidPacketIndexCount == 1u);
        CHECK(capturePlan.stats().incompatiblePacketCount == 1u);
        CHECK(capturePlan.stats().invalidPlacementCount == 1u);

        BoundedOrdinary2AtlasPlan<2> plan;
        beginCpuAllocationFrame();
        for (uint32_t iteration = 0; iteration < 100u; ++iteration) {
            collector.collect(packets, glm::mat4(1.0f), 100u, 100u);
            (void)plan.prepare(collector.requests(), 100u, 100u);
            (void)capturePlan.prepare(captureDecisions, packets,
                { 64u, 32u });
        }
        const CpuAllocationFrameSample allocationSample =
            endCpuAllocationFrame();
        CHECK(allocationSample.allocationCount == 0u);
        CHECK(allocationSample.requestedBytes == 0u);

        const std::filesystem::path backendPath =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "src/renderer/vulkan/VulkanVertexBackend.cpp";
        std::ifstream backendInput(backendPath, std::ios::binary);
        CHECK(backendInput.good());
        const std::string backendSource{
            std::istreambuf_iterator<char>(backendInput),
            std::istreambuf_iterator<char>() };
        const size_t functionBegin = backendSource.find(
            "void VulkanVertexBackend::submitForwardQueues(");
        const size_t functionEnd = backendSource.find(
            "void VulkanVertexBackend::captureCurrentFrame(", functionBegin);
        CHECK(functionBegin != std::string::npos);
        CHECK(functionEnd != std::string::npos);
        const std::string forwardFunction = backendSource.substr(
            functionBegin, functionEnd - functionBegin);
        CHECK(forwardFunction.find(
            "ordinary2CaptureTopologyActive || collectFrameCounters_") !=
            std::string::npos);
        CHECK(forwardFunction.find(
            "ordinary2AtlasResidency_.observe(requiresOrdinary2Atlas)") !=
            std::string::npos);
        CHECK(forwardFunction.find("ordinary2RequestCollector_.collect(") !=
            std::string::npos);
        CHECK(forwardFunction.find("ordinary2AtlasPlan_.prepare(") !=
            std::string::npos);
        CHECK(forwardFunction.find("recordOrdinary2Captures(") !=
            std::string::npos);
        CHECK(forwardFunction.find(
            "recordOrdinary2CaptureValidationReadback(captureDraws)") !=
            std::string::npos);
        CHECK(forwardFunction.find(
            "recordOrdinary2LocalComposition(compatibilityTransparentQueue") !=
            std::string::npos);
        CHECK(forwardFunction.find(
            "recordOrdinary2SceneResolve(compatibilityTransparentQueue") !=
            std::string::npos);
        const size_t captureRecordBegin = backendSource.find(
            "void VulkanVertexBackend::recordOrdinary2InterfaceCapture(");
        CHECK(captureRecordBegin != std::string::npos);
        const std::string captureRecord = backendSource.substr(
            captureRecordBegin, functionBegin - captureRecordBegin);
        CHECK(captureRecord.find("vkCmdBeginRenderPass") != std::string::npos);
        CHECK(captureRecord.find("vkCmdSetViewport") != std::string::npos);
        CHECK(captureRecord.find("vkCmdSetScissor") != std::string::npos);
        CHECK(captureRecord.find("kLayeredCaptureMirrored") !=
            std::string::npos);
        CHECK(captureRecord.find("kLayeredCaptureHasPrevious") !=
            std::string::npos);
        CHECK(captureRecord.find(
            "kLayeredCaptureRequirePairedOrientation") !=
            std::string::npos);
        CHECK(captureRecord.find("packLayeredViewportOffset") !=
            std::string::npos);
        const size_t pairedCaptureBegin = backendSource.find(
            "void VulkanVertexBackend::recordOrdinary2Captures(");
        CHECK(pairedCaptureBegin != std::string::npos);
        const std::string pairedCapture = backendSource.substr(
            pairedCaptureBegin, functionBegin - pairedCaptureBegin);
        const size_t entryCall = pairedCapture.find(
            "recordOrdinary2InterfaceCapture(packets, draws, false)");
        const size_t exitCall = pairedCapture.find(
            "recordOrdinary2InterfaceCapture(packets, draws, true)");
        CHECK(entryCall != std::string::npos);
        CHECK(exitCall != std::string::npos);
        CHECK(entryCall < exitCall);
        CHECK(backendSource.find(
            "transparent.ordinary2.fallback.near_plane_packets") !=
            std::string::npos);
        CHECK(backendSource.find(
            "transparent.ordinary2.atlas.allocated_texels") !=
            std::string::npos);
        return true;
    }

    bool testOrdinary2MaterialAwareCaptureShaderContract() {
        const std::filesystem::path shaderPath =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "assets/shaders/layered_interface_capture.frag";
        std::ifstream input(shaderPath, std::ios::binary);
        CHECK(input.good());
        const std::string source{ std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        CHECK(source.find("layout(location = 0) out uint") !=
            std::string::npos);
        CHECK(source.find("PackedMaterial materials[]") !=
            std::string::npos);
        CHECK(source.find("packedMaterialHasTexture") !=
            std::string::npos);
        CHECK(source.find("material.alphaMode == 1u") !=
            std::string::npos);
        CHECK(source.find("resolvedTransparencyClass !=") !=
            std::string::npos);
        CHECK(source.find("gl_FragCoord.z > opaqueSample") !=
            std::string::npos);
        CHECK(source.find("gl_FrontFacing != mirrored") !=
            std::string::npos);
        CHECK(source.find("IRIDIUM_LAYERED_CAPTURE_HAS_PREVIOUS") !=
            std::string::npos);
        CHECK(source.find(
            "IRIDIUM_LAYERED_CAPTURE_REQUIRE_PAIRED_ORIENTATION") !=
            std::string::npos);
        CHECK(source.find("requirePairedOrientation") !=
            std::string::npos);
        CHECK(source.find("gl_FragCoord.z <= previousDepth") !=
            std::string::npos);
        CHECK(source.find(
            "previousIdentity & IRIDIUM_LAYERED_WORK_MASK") !=
            std::string::npos);
        CHECK(source.find("outInterfaceIdentity = oneBasedWork") !=
            std::string::npos);
        CHECK(source.find(
            "!semanticEntry ? IRIDIUM_LAYERED_ORIENTATION_BIT") !=
            std::string::npos);
        CHECK(source.find("int unpackSigned16") != std::string::npos);
        CHECK(source.find("ivec2 viewportOffset") != std::string::npos);
        return true;
    }

    bool testOrdinary2MeasuredChordMaterialShaderContract() {
        const auto readSource = [](const char* relative) {
            const std::filesystem::path path =
                std::filesystem::path(PROJECT_ROOT_DIR) / relative;
            std::ifstream input(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        };
        const std::string wrapper = readSource(
            "assets/shaders/layered_ordinary2_material_indexed.frag");
        CHECK(wrapper.find("IRIDIUM_INDEXED_MATERIAL_TEXTURES") !=
            std::string::npos);
        CHECK(wrapper.find("IRIDIUM_LAYERED_ORDINARY2_COMPOSITION") !=
            std::string::npos);
        CHECK(wrapper.find("include/complex_material_body.glsl") !=
            std::string::npos);

        const std::string body = readSource(
            "assets/shaders/include/complex_material_body.glsl");
        CHECK(body.find("layout(set = 4, binding = 0) uniform sampler2D "
            "layeredEntryDepth") != std::string::npos);
        CHECK(body.find("layout(set = 4, binding = 3) uniform usampler2D "
            "layeredExitIdentity") != std::string::npos);
        CHECK(body.find("iridiumLayeredOrdinary2PairIsValid") !=
            std::string::npos);
        CHECK(body.find("entryIdentity != oneBasedWork") !=
            std::string::npos);
        CHECK(body.find("exitIdentity != (oneBasedWork |") !=
            std::string::npos);
        CHECK(body.find("exitDepth > entryDepth") != std::string::npos);
        CHECK(body.find("iridiumReconstructViewPosition") !=
            std::string::npos);
        CHECK(body.find("length(exitView - entryView)") !=
            std::string::npos);
        CHECK(body.find("min(measuredChordMeters, authoredMaximumMeters)") !=
            std::string::npos);
        CHECK(body.find("iridiumLayeredOrdinary2PathMeters(volumeThickness)") !=
            std::string::npos);
        CHECK(body.find("IRIDIUM_MATERIAL_SCENE_PIXEL") !=
            std::string::npos);
        CHECK(body.find("iridiumEvaluateStandardIbl") != std::string::npos);
        CHECK(body.find("iridiumProjectTransparencyRay") !=
            std::string::npos);
        return true;
    }

    bool testOrdinary2VulkanLocalCompositionObjectContract() {
        const auto readSource = [](const char* relative) {
            const std::filesystem::path path =
                std::filesystem::path(PROJECT_ROOT_DIR) / relative;
            std::ifstream input(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        };
        const std::string pass = readSource(
            "src/renderer/vulkan/VulkanLayeredLocalCompositionPass.cpp");
        CHECK(pass.find("VulkanSceneColorFormat") != std::string::npos);
        CHECK(pass.find("VK_ATTACHMENT_LOAD_OP_CLEAR") !=
            std::string::npos);
        CHECK(pass.find("std::array<VkDescriptorSetLayout, 5>") !=
            std::string::npos);
        CHECK(pass.find("std::array<VkDescriptorImageInfo, 4>") !=
            std::string::npos);
        CHECK(pass.find(
            "layered_ordinary2_material_indexed_frag.spv") !=
            std::string::npos);
        CHECK(pass.find("VK_CULL_MODE_NONE") != std::string::npos);
        CHECK(pass.find("sizeof(CanonicalMeshPushConstants)") !=
            std::string::npos);

        const std::string graph = readSource(
            "src/renderer/vulkan/VulkanProductionRenderGraph.cpp");
        CHECK(graph.find("scene.layered.local-color") !=
            std::string::npos);
        CHECK(graph.find("transparent.layered.local-compose") !=
            std::string::npos);
        CHECK(graph.find(
            "graph.write(localComposition, layeredLocalColor") !=
            std::string::npos);
        CHECK(graph.find(
            "graph.read(compositionContract, layeredLocalColor") !=
            std::string::npos);
        CHECK(graph.find(
            "graph.read(compositionContract, layeredEntryIdentity") !=
            std::string::npos);

        const std::string backend = readSource(
            "src/renderer/vulkan/VulkanVertexBackend.cpp");
        const size_t record = backend.find(
            "void VulkanVertexBackend::recordOrdinary2LocalComposition(");
        CHECK(record != std::string::npos);
        const size_t next = backend.find(
            "void VulkanVertexBackend::recordOrdinary2CaptureValidationReadback(",
            record);
        CHECK(next != std::string::npos);
        const std::string function = backend.substr(record, next - record);
        CHECK(function.find("draws.rbegin()") != std::string::npos);
        CHECK(function.find("layout, 3u, 1u, &sceneSet") !=
            std::string::npos);
        CHECK(function.find("layout, 4u, 1u, &interfaceSet") !=
            std::string::npos);
        CHECK(function.find("push.padding[0] = draw.workTableIndex") !=
            std::string::npos);
        CHECK(function.find("packLayeredViewportOffset") !=
            std::string::npos);
        CHECK(function.find("ordinary2LocalCompositionDraws") !=
            std::string::npos);

        const std::string resolvePass = readSource(
            "src/renderer/vulkan/VulkanLayeredSceneResolvePass.cpp");
        CHECK(resolvePass.find("layered_scene_resolve_frag.spv") !=
            std::string::npos);
        CHECK(resolvePass.find("VK_BLEND_FACTOR_ONE") !=
            std::string::npos);
        CHECK(resolvePass.find("VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA") !=
            std::string::npos);
        CHECK(resolvePass.find("depthWriteEnable = VK_FALSE") !=
            std::string::npos);
        CHECK(resolvePass.find("buildSet(hero4DescriptorSets_[frame]") !=
            std::string::npos);
        CHECK(resolvePass.find("frameTargets.integerSampler()") !=
            std::string::npos);
        const std::string resolveShader = readSource(
            "assets/shaders/layered_scene_resolve.frag");
        CHECK(resolveShader.find(
            "ivec2 atlasPixel = ivec2(gl_FragCoord.xy) - viewportOffset") !=
            std::string::npos);
        CHECK(resolveShader.find("if (gl_FrontFacing == mirrored)") !=
            std::string::npos);
        CHECK(resolveShader.find("uniform usampler2D layeredEntryIdentity") !=
            std::string::npos);
        CHECK(resolveShader.find(
            "(entryIdentity & workMask) != expectedEntryIdentity") !=
            std::string::npos);
        CHECK(resolveShader.find("push.padding0 != 0u") !=
            std::string::npos);
        CHECK(backend.find(
            "void VulkanVertexBackend::recordOrdinary2SceneResolve(") !=
            std::string::npos);
        CHECK(backend.find("isOrdinary2PacketResolved(packetIndex)") !=
            std::string::npos);
        return true;
    }

    bool testDeepLayeredLocalCompositionContract() {
        const auto readSource = [](const char* relative) {
            const std::filesystem::path path =
                std::filesystem::path(PROJECT_ROOT_DIR) / relative;
            std::ifstream input(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        };
        const std::string wrapper = readSource(
            "assets/shaders/layered_deep_material_indexed.frag");
        CHECK(wrapper.find("IRIDIUM_LAYERED_DEEP_COMPOSITION") !=
            std::string::npos);
        CHECK(wrapper.find("include/complex_material_body.glsl") !=
            std::string::npos);
        const std::string residualWrapper = readSource(
            "assets/shaders/layered_deep_residual_material_indexed.frag");
        CHECK(residualWrapper.find("IRIDIUM_LAYERED_DEEP_RESIDUAL") !=
            std::string::npos);
        CHECK(residualWrapper.find("include/complex_material_body.glsl") !=
            std::string::npos);
        const std::string body = readSource(
            "assets/shaders/include/complex_material_body.glsl");
        CHECK(body.find("layeredInterfaceDepth[8]") != std::string::npos);
        CHECK(body.find("layeredInterfaceIdentity[8]") != std::string::npos);
        CHECK(body.find("iridiumLayeredDeepFindPair") != std::string::npos);
        CHECK(body.find("abs(gl_FragCoord.z - entryDepth)") !=
            std::string::npos);
        CHECK(body.find("candidate < interfaceCount") !=
            std::string::npos);
        CHECK(body.find("iridiumLayeredDeepPathMeters(volumeThickness)") !=
            std::string::npos);
        CHECK(body.find("residualTransmission") != std::string::npos);
        CHECK(body.find("desired -") != std::string::npos);
        CHECK(body.find("iridiumLayeredDeepWorkOpenAtCapacity") !=
            std::string::npos);
        CHECK(body.find("iridiumLayeredDeepResidualEntryIsValid") !=
            std::string::npos);
        CHECK(body.find("layeredNonRefractiveResidual") !=
            std::string::npos);

        const std::string pass = readSource(
            "src/renderer/vulkan/VulkanLayeredLocalCompositionPass.cpp");
        CHECK(pass.find("deepBindings[binding].descriptorCount =") !=
            std::string::npos);
        CHECK(pass.find("layered_deep_material_indexed_frag.spv") !=
            std::string::npos);
        CHECK(pass.find(
            "layered_deep_residual_material_indexed_frag.spv") !=
            std::string::npos);
        CHECK(pass.find("premultipliedBlend") != std::string::npos);
        CHECK(pass.find("VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA") !=
            std::string::npos);
        CHECK(pass.find("buildDeepSet(target.hero4") != std::string::npos);
        CHECK(pass.find("buildDeepSet(target.cinematic8") !=
            std::string::npos);

        const std::string backend = readSource(
            "src/renderer/vulkan/VulkanVertexBackend.cpp");
        const size_t record = backend.find(
            "void VulkanVertexBackend::recordDeepLayeredLocalComposition(");
        CHECK(record != std::string::npos);
        const size_t next = backend.find(
            "void VulkanVertexBackend::recordOrdinary2LocalComposition(",
            record);
        CHECK(next != std::string::npos);
        const std::string function = backend.substr(record, next - record);
        CHECK(function.find("interfaceIndex-- > 0u") != std::string::npos);
        CHECK(function.find("deepDescriptorSet(frameIndex, quality)") !=
            std::string::npos);
        CHECK(function.find("interfaceIndex << 8u") != std::string::npos);
        CHECK(function.find("interfaceCount << 16u") != std::string::npos);
        CHECK(function.find("deepLayeredLocalCompositionDraws") !=
            std::string::npos);
        CHECK(function.find("deepResidualPipeline()") !=
            std::string::npos);
        CHECK(function.find("beginLayeredResidualQuery") !=
            std::string::npos);
        CHECK(function.find("deepLayeredResidualProbeDraws") !=
            std::string::npos);
        CHECK(backend.find(
            "recordDeepLayeredLocalComposition(compatibilityTransparentQueue") !=
            std::string::npos);
        CHECK(backend.find("recordDeepLayeredCaptureValidationReadback(") !=
            std::string::npos);
        CHECK(backend.find("collectDeepLayeredCaptureValidationsForSlot(") !=
            std::string::npos);
        CHECK(backend.find("crossingPairPixelCount") !=
            std::string::npos);
        CHECK(backend.find("requestDeepLayeredCaptureValidation(") !=
            std::string::npos);
        CHECK(backend.find(
            "void VulkanVertexBackend::recordDeepLayeredSceneResolve(") !=
            std::string::npos);
        CHECK(backend.find(
            "recordDeepLayeredSceneResolve(compatibilityTransparentQueue") !=
            std::string::npos);
        CHECK(backend.find("isLayeredPacketResolved(packetIndex)") !=
            std::string::npos);
        CHECK(backend.find("deepLayeredSceneResolveDraws") !=
            std::string::npos);
        CHECK(backend.find("deepResolvedDraws_") !=
            std::string::npos);
        CHECK(backend.find(
            "gpu.transparency.layered.cinematic8.scene-resolve") !=
            std::string::npos);
        CHECK(backend.find(
            "transparent.layered.deep.compose-hook") !=
            std::string::npos);
        const std::string graph = readSource(
            "src/renderer/vulkan/VulkanProductionRenderGraph.cpp");
        CHECK(graph.find(
            "graph.read(composition, tier.identity[0]") !=
            std::string::npos);
        const std::string scheduler = readSource(
            "src/renderer/vulkan/VulkanFrameScheduler.cpp");
        CHECK(scheduler.find("VK_QUERY_TYPE_OCCLUSION") !=
            std::string::npos);
        CHECK(scheduler.find(
            "transparent.layered.hero4.residual_samples") !=
            std::string::npos);
        CHECK(scheduler.find(
            "transparent.layered.cinematic8.residual_samples") !=
            std::string::npos);
        return true;
    }

    bool testOrdinary2VulkanCaptureObjectContract() {
        const auto readSource = [](const char* relative) {
            const std::filesystem::path path =
                std::filesystem::path(PROJECT_ROOT_DIR) / relative;
            std::ifstream input(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        };
        const std::string capture = readSource(
            "src/renderer/vulkan/VulkanLayeredInterfaceCapturePass.cpp");
        CHECK(capture.find("VK_FORMAT_R32_UINT") != std::string::npos);
        CHECK(capture.find("VK_FORMAT_D32_SFLOAT") != std::string::npos);
        CHECK(capture.find(
            "VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL") !=
            std::string::npos);
        CHECK(capture.find(
            "attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED") !=
            std::string::npos);
        CHECK(capture.find(
            "attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED") !=
            std::string::npos);
        CHECK(capture.find("VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER") !=
            std::string::npos);
        CHECK(capture.find("std::array<VkDescriptorSetLayout, 4>") !=
            std::string::npos);
        CHECK(capture.find("sizeof(LayeredInterfaceCapturePushConstants)") !=
            std::string::npos);
        CHECK(capture.find("pipeline_ = createPipeline()") !=
            std::string::npos);
        CHECK(capture.find("VK_CULL_MODE_NONE") != std::string::npos);
        CHECK(capture.find("target.layeredEntryDepth") !=
            std::string::npos);
        CHECK(capture.find("target.materialFlags") !=
            std::string::npos);
        CHECK(capture.find("const auto buildTier") !=
            std::string::npos);
        CHECK(capture.find("target.hero4.interfaceCount") !=
            std::string::npos);
        CHECK(capture.find("target.cinematic8.interfaceCount") !=
            std::string::npos);
        CHECK(capture.find("depthAt(interfaceIndex - 1u)") !=
            std::string::npos);
        CHECK(capture.find("descriptorInterfaceCount(") !=
            std::string::npos);

        const std::string targets = readSource(
            "src/renderer/vulkan/VulkanFrameTargets.cpp");
        CHECK(targets.find("depth.layered.entry") != std::string::npos);
        CHECK(targets.find("identity.layered.exit") != std::string::npos);
        CHECK(targets.find("layeredEntryFramebuffer") != std::string::npos);
        CHECK(targets.find("ordinary2AtlasExtent.width") !=
            std::string::npos);
        CHECK(targets.find("acquireDeepLayeredTier") !=
            std::string::npos);
        CHECK(targets.find("depth.layered.\" + suffix") !=
            std::string::npos);
        CHECK(targets.find("createDeepLayeredTierFramebuffers") !=
            std::string::npos);
        CHECK(targets.find("destroyDeepLayeredTier(target.cinematic8)") !=
            std::string::npos);

        const std::string targetHeader = readSource(
            "src/renderer/vulkan/VulkanFrameTargets.h");
        CHECK(targetHeader.find("struct DeepLayeredTier") !=
            std::string::npos);
        CHECK(targetHeader.find(
            "kMaximumLayeredInterfaceCount") != std::string::npos);
        CHECK(targetHeader.find("DeepLayeredTier hero4") !=
            std::string::npos);
        CHECK(targetHeader.find("DeepLayeredTier cinematic8") !=
            std::string::npos);

        const std::string backend = readSource(
            "src/renderer/vulkan/VulkanVertexBackend.cpp");
        const size_t prewarm = backend.find("layeredInterfaceCapture_.init");
        const size_t targetInit = backend.find("initFrameTargets();");
        CHECK(prewarm != std::string::npos);
        CHECK(targetInit != std::string::npos);
        CHECK(prewarm < targetInit);
        CHECK(backend.find(
            "layeredInterfaceCapture_.rebuildDescriptors(frameTargets)") !=
            std::string::npos);
        CHECK(backend.find("VulkanLayeredGraphConfig{ ordinary2AtlasExtent_,") !=
            std::string::npos);
        CHECK(backend.find("hero4AtlasExtent_, cinematic8AtlasExtent_") !=
            std::string::npos);
        CHECK(backend.find(
            "transparent.layered.validation-readback-hook") !=
            std::string::npos);
        CHECK(backend.find("commandList.copyImageToBuffer") !=
            std::string::npos);
        CHECK(backend.find("exitDepth > entryDepth") !=
            std::string::npos);
        const size_t clear = backend.find(
            "layeredInterfaceCapture_.clearDescriptors()");
        const size_t cleanup = backend.find("frameTargets.cleanup()", clear);
        CHECK(clear != std::string::npos);
        CHECK(cleanup != std::string::npos);
        CHECK(clear < cleanup);
        return true;
    }

    bool testContentDrivenFrameTopologyPrewarmContract() {
        ModelAsset model{};
        model.materials.push_back({
            .renderQueue = RenderQueue::Opaque,
        });
        model.materials.push_back({
            .renderQueue = RenderQueue::Transparent,
        });
        model.subMeshes.push_back({
            .materialIndex = 0,
        });
        CHECK(!modelRequiresRefractionPyramids(model));

        model.subMeshes.front().materialIndex = 1;
        CHECK(modelRequiresRefractionPyramids(model));
        CHECK(!modelRequiresOrdinary2LayeredInterfaces(model));

        model.transparencyExecutionMode =
            TransparencyExecutionMode::Classified;
        model.subMeshes.front().transparency.resolvedClass =
            TransparencyClass::SortedSurface;
        CHECK(!modelRequiresRefractionPyramids(model));
        model.subMeshes.front().transparency.resolvedClass =
            TransparencyClass::LayeredGlass;
        CHECK(modelRequiresRefractionPyramids(model));
        CHECK(modelRequiresOrdinary2LayeredInterfaces(model));

        model.subMeshes.front().transparency.quality =
            TransparencyQuality::Hero4;
        CHECK(!modelRequiresOrdinary2LayeredInterfaces(model));
        CHECK(modelRequiresHero4LayeredInterfaces(model));
        CHECK(!modelRequiresCinematic8LayeredInterfaces(model));
        model.subMeshes.front().transparency.quality =
            TransparencyQuality::Cinematic8;
        CHECK(!modelRequiresHero4LayeredInterfaces(model));
        CHECK(modelRequiresCinematic8LayeredInterfaces(model));
        model.subMeshes.front().transparency.quality =
            TransparencyQuality::Ordinary2;

        model.subMeshes.front().materialIndex = 99;
        CHECK(!modelRequiresRefractionPyramids(model));
        CHECK(!modelRequiresOrdinary2LayeredInterfaces(model));

        const auto readSource = [](const char* relative) {
            const std::filesystem::path path =
                std::filesystem::path(PROJECT_ROOT_DIR) / relative;
            std::ifstream input(path, std::ios::binary);
            return std::string{ std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        };
        const std::string application = readSource("src/core/Application.cpp");
        const size_t prepare = application.find(
            "renderBackend->prepareFrameTopology(");
        const size_t sceneStart = application.find(
            "const auto sceneStart", prepare);
        const size_t initRendererDefinition = application.find(
            "void Application::initRenderer(");
        const size_t mainLoopDefinition = application.find(
            "void Application::mainLoop(", initRendererDefinition);
        const size_t initRendererCall = application.find("initRenderer();");
        const size_t mainLoopCall = application.find(
            "mainLoop();", initRendererCall);
        CHECK(prepare != std::string::npos);
        CHECK(sceneStart != std::string::npos);
        CHECK(initRendererDefinition != std::string::npos);
        CHECK(mainLoopDefinition != std::string::npos);
        CHECK(initRendererCall != std::string::npos);
        CHECK(mainLoopCall != std::string::npos);
        CHECK(initRendererDefinition < prepare);
        CHECK(prepare < mainLoopDefinition);
        CHECK(prepare < sceneStart);
        CHECK(initRendererCall < mainLoopCall);
        CHECK(application.find(".ordinary2LayeredInterfaces = mainModel") !=
            std::string::npos);
        CHECK(application.find(".hero4LayeredInterfaces = mainModel") !=
            std::string::npos);
        CHECK(application.find(".cinematic8LayeredInterfaces = mainModel") !=
            std::string::npos);

        const std::string backend = readSource(
            "src/renderer/vulkan/VulkanVertexBackend.cpp");
        const size_t functionBegin = backend.find(
            "FrameTopologyPreparation VulkanVertexBackend::prepareFrameTopology(");
        const size_t functionEnd = backend.find(
            "// ==============================================================================",
            functionBegin);
        CHECK(functionBegin != std::string::npos);
        CHECK(functionEnd != std::string::npos);
        const std::string function = backend.substr(
            functionBegin, functionEnd - functionBegin);
        CHECK(function.find("if (frameOpen_)") != std::string::npos);
        CHECK(function.find("transparencyPyramidResidency_.observe(true)") !=
            std::string::npos);
        CHECK(function.find("requirements.ordinary2LayeredInterfaces") !=
            std::string::npos);
        CHECK(function.find("requirements.hero4LayeredInterfaces") !=
            std::string::npos);
        CHECK(function.find("requirements.cinematic8LayeredInterfaces") !=
            std::string::npos);
        CHECK(function.find("layeredAtlasCapacityExtent(") !=
            std::string::npos);
        CHECK(function.find("applyTransparencyPyramidTopologyChange(") !=
            std::string::npos);
        CHECK(function.find("durationNanoseconds") != std::string::npos);
        CHECK(backend.find("Hero4PassNames") != std::string::npos);
        CHECK(backend.find("Cinematic8PassNames") !=
            std::string::npos);
        CHECK(backend.find("recordDeepLayeredCaptures(") !=
            std::string::npos);
        CHECK(backend.find("deepLayeredRequestCollector_.collect(") !=
            std::string::npos);
        CHECK(backend.find("deepLayeredAtlasPlan_.prepare(") !=
            std::string::npos);
        CHECK(backend.find("deepLayeredCaptureDrawPlan_.prepare(") !=
            std::string::npos);
        CHECK(backend.find(
            "layeredInterfaceCapture_.descriptorSet(frameIndex, quality,") !=
            std::string::npos);
        CHECK(backend.find("push.flags |= kLayeredCaptureHasPrevious") !=
            std::string::npos);
        CHECK(backend.find(
            "recordDeepLayeredSceneResolve(compatibilityTransparentQueue") !=
            std::string::npos);
        CHECK(backend.find("prepareDeepResolvedPacketIndices(") !=
            std::string::npos);
        CHECK(backend.find("draw.quality != lastQuality") !=
            std::string::npos);
        return true;
    }

    bool testWeightedOitBoundedReferenceContract() {
        CHECK(weightedOitWeight(0.0f, 0.0f) == 0.0f);
        const float nearWeight = weightedOitWeight(1.0f, 0.0f);
        const float farWeight = weightedOitWeight(1.0f, 1.0f);
        CHECK(nearWeight == WeightedOitMaximumWeight);
        CHECK(farWeight >= WeightedOitMinimumWeight);
        CHECK(farWeight < nearWeight);
        bool sanitized = false;
        CHECK(weightedOitWeight(std::numeric_limits<float>::infinity(),
            -1.0f, &sanitized) == 0.0f);
        CHECK(sanitized);
        CHECK(weightedOitLogicalStorageBytes(3840u, 2160u) ==
            82'944'000u);

        WeightedOitAccumulator empty{};
        const WeightedOitResolveResult emptyResult =
            resolveWeightedOit(empty);
        CHECK(emptyResult.finite);
        CHECK(emptyResult.coverage == 0.0f);
        CHECK(emptyResult.premultipliedRadiance == glm::vec3(0.0f));

        WeightedOitAccumulator opaque{};
        accumulateWeightedOit(opaque, {
            .premultipliedRadiance = { 4.0f, 2.0f, 1.0f },
            .coverage = 1.0f,
            .normalizedLinearDepth = 0.0f,
        });
        const WeightedOitResolveResult opaqueResult =
            resolveWeightedOit(opaque);
        CHECK(opaqueResult.finite);
        CHECK(opaqueResult.coverage == 1.0f);
        CHECK(glm::all(glm::lessThan(glm::abs(
            opaqueResult.premultipliedRadiance - glm::vec3(4.0f, 2.0f, 1.0f)),
            glm::vec3(1.0e-6f))));

        const std::array<WeightedOitContribution, 3> contributions{{
            {{ 0.4f, 0.1f, 0.0f }, 0.5f, 0.2f},
            {{ 0.0f, 0.3f, 0.2f }, 0.35f, 0.6f},
            {{ 0.1f, 0.0f, 0.6f }, 0.7f, 0.9f},
        }};
        const auto accumulateOrder = [&](std::array<uint32_t, 3> order) {
            WeightedOitAccumulator accumulator{};
            for (uint32_t index : order)
                accumulateWeightedOit(accumulator, contributions[index]);
            return resolveWeightedOit(accumulator);
        };
        const WeightedOitResolveResult forward = accumulateOrder({0, 1, 2});
        const WeightedOitResolveResult reverse = accumulateOrder({2, 1, 0});
        CHECK(std::abs(forward.coverage - reverse.coverage) < 1.0e-7f);
        CHECK(glm::all(glm::lessThan(glm::abs(forward.premultipliedRadiance -
            reverse.premultipliedRadiance), glm::vec3(1.0e-6f))));

        WeightedOitAccumulator qualifiedMaximum{};
        for (uint32_t index = 0u;
            index < WeightedOitQualifiedMaximumFragments; ++index) {
            accumulateWeightedOit(qualifiedMaximum, {
                .premultipliedRadiance = glm::vec3(
                    WeightedOitMaximumPremultipliedRadiance),
                .coverage = 1.0f,
                .normalizedLinearDepth = 0.0f,
            });
        }
        CHECK(qualifiedMaximum.weightedPremultipliedRadiance.x == 32768.0);
        CHECK(weightedOitWithinQualifiedFp16Envelope(qualifiedMaximum));
        accumulateWeightedOit(qualifiedMaximum, {
            .premultipliedRadiance = glm::vec3(1.0f),
            .coverage = 1.0f,
            .normalizedLinearDepth = 0.0f,
        });
        CHECK(!weightedOitWithinQualifiedFp16Envelope(qualifiedMaximum));

        WeightedOitAccumulator clamped{};
        accumulateWeightedOit(clamped, {
            .premultipliedRadiance = { -1.0f, 129.0f,
                std::numeric_limits<float>::quiet_NaN() },
            .coverage = 0.5f,
            .normalizedLinearDepth = 0.5f,
        });
        CHECK(clamped.radianceClamped);
        CHECK(clamped.sanitized);
        CHECK(!weightedOitWithinQualifiedFp16Envelope(clamped));
        return true;
    }

    bool testArtistFacingTransparencyPolicyControls() {
        const std::filesystem::path panelPath =
            std::filesystem::path(PROJECT_ROOT_DIR) /
            "src/editor/panels/windows/AssetBrowserPanel.cpp";
        std::ifstream input(panelPath, std::ios::binary);
        CHECK(input.good());
        const std::string source{ std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        CHECK(source.find("Transparency renderer") != std::string::npos);
        CHECK(source.find("Classified hybrid (recommended)") !=
            std::string::npos);
        CHECK(source.find("Override inherited transparency policy") !=
            std::string::npos);
        CHECK(source.find("Transparency class") != std::string::npos);
        CHECK(source.find("Layer budget") != std::string::npos);
        CHECK(source.find("Ordinary - 2 interfaces (one shell)") !=
            std::string::npos);
        CHECK(source.find("Hero - 4 interfaces (nested glass)") !=
            std::string::npos);
        CHECK(source.find("Cinematic - 8 interfaces (explicit)") !=
            std::string::npos);
        CHECK(source.find("nested capture, local composition, and scene resolve are active") !=
            std::string::npos);
        CHECK(source.find("bounded capture, local composition, and scene resolve are active") !=
            std::string::npos);
        CHECK(source.find("Reset to inherited Auto") !=
            std::string::npos);
        CHECK(source.find("thin_sheet_thickness_m") !=
            std::string::npos);
        CHECK(source.find("target.guid.toString()") != std::string::npos);
        CHECK(source.find("policies.erase(targetGuid)") !=
            std::string::npos);
        CHECK(source.find("itemTooltip(") != std::string::npos);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "RenderHandle/ResourcePool", testRenderHandleAndResourcePool },
        { "GeometryDesc", testGeometryDesc },
        { "canonical material GPU contract", testCanonicalMaterialGpuContract },
        { "canonical material scale contract", testCanonicalMaterialScaleContract },
        { "PipelineStateDesc", testPipelineStateDescHash },
        { "render queue depth coverage", testRenderQueueDepthCoverage },
        { "ResourceState mapping", testResourceStateMapping },
        { "VulkanCommandList invalid wrapper", testInvalidCommandList },
        { "GBuffer candidate contracts", testGBufferCandidateContracts },
        { "packed GBuffer numeric bounds", testPackedGBufferNumericBounds },
        { "transparent work intervals and deterministic ordering",
            testTransparentWorkIntervalsAndDeterministicOrder },
        { "Ordinary2 interface pairing contract",
            testOrdinary2InterfacePairingContract },
        { "Layered quality tier and overflow contract",
            testLayeredQualityTierAndOverflowContract },
        { "Layered tier-aware bounded atlas preparation",
            testLayeredTierAwareAtlasPreparation },
        { "Ordinary2 bounded atlas preparation",
            testOrdinary2BoundedAtlasPreparation },
        { "Ordinary2 capture validation result contract",
            testOrdinary2CaptureValidationResultContract },
        { "deep layered capture validation result contract",
            testDeepLayeredCaptureValidationResultContract },
        { "Ordinary2 projection and bounded request collection",
            testOrdinary2ProjectionAndRequestCollection },
        { "Ordinary2 material-aware capture shader contract",
            testOrdinary2MaterialAwareCaptureShaderContract },
        { "Ordinary2 measured-chord material shader contract",
            testOrdinary2MeasuredChordMaterialShaderContract },
        { "Ordinary2 Vulkan local-composition object contract",
            testOrdinary2VulkanLocalCompositionObjectContract },
        { "deep layered local-composition contract",
            testDeepLayeredLocalCompositionContract },
        { "Ordinary2 Vulkan capture object contract",
            testOrdinary2VulkanCaptureObjectContract },
        { "content-driven frame-topology prewarm",
            testContentDrivenFrameTopologyPrewarmContract },
        { "WeightedOIT bounded reference contract",
            testWeightedOitBoundedReferenceContract },
        { "artist-facing transparency policy controls",
            testArtistFacingTransparencyPolicyControls },
        { "swapchain rebuild clustered descriptor lifecycle",
            testSwapchainRebuildRetiresClusterDescriptors },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            } else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
