#include "renderer/rhi/PipelineTypes.h"
#include "renderer/rhi/ResourcePool.h"
#include "renderer/rhi/RhiResourceTypes.h"
#include "renderer/vulkan/VulkanCommandList.h"
#include "renderer/vulkan/VulkanPipelineLibrary.h"
#include "renderer/vulkan/VulkanResourceState.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>

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
        changed.shaderProgram = ShaderProgram::PbrForward;
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
            const VulkanStateInfo actual = getVulkanStateInfo(expected.state, VK_IMAGE_ASPECT_DEPTH_BIT);
            CHECK(actual.stages == expected.stages);
            CHECK(actual.access == expected.access);
            CHECK(actual.layout == expected.layout);
        }

        const VulkanStateInfo depthWrite = getVulkanStateInfo(ResourceState::DepthWrite, VK_IMAGE_ASPECT_DEPTH_BIT);
        const VulkanStateInfo depthRead = getVulkanStateInfo(ResourceState::DepthRead, VK_IMAGE_ASPECT_DEPTH_BIT);
        CHECK(depthWrite.layout != depthRead.layout);
        return true;
    }

    bool testInvalidCommandList() {
        const VulkanCommandList commandList;
        CHECK(!commandList.isValid());
        CHECK(commandList.native() == VK_NULL_HANDLE);
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
        { "PipelineStateDesc", testPipelineStateDescHash },
        { "ResourceState mapping", testResourceStateMapping },
        { "VulkanCommandList invalid wrapper", testInvalidCommandList },
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
