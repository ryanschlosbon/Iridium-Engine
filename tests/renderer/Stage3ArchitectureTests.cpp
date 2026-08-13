#include "renderer/rhi/PipelineTypes.h"
#include "renderer/rhi/Mesh.h"
#include "renderer/rhi/ResourcePool.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/rhi/MaterialTableCapacity.h"
#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanPipelineLibrary.h"
#include "renderer/vulkan/VulkanResourceState.h"
#include "renderer/vulkan/VulkanGBufferLayout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
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
        CHECK(PackedGpuMaterial::SchemaVersion == 2);
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
