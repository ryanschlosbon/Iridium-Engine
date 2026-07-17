#pragma once

#include <cstdint>
#include <type_traits>

namespace Iridium {

    enum class ResourceState : uint8_t {
        Undefined,
        CopySource,
        CopyDestination,
        VertexBuffer,
        IndexBuffer,
        ConstantBuffer,
        ShaderResource,
        ColorAttachment,
        DepthWrite,
        DepthRead,
        Present,
    };

    enum class IndexFormat : uint8_t {
        UInt16,
        UInt32,
    };

    struct GeometryDesc {
        uint32_t vertexStride = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
    };

    enum class FrameStatus : uint8_t {
        Ready,
        RecreateSwapchain,
    };

    constexpr uint32_t indexElementSize(IndexFormat format) noexcept {
        switch (format) {
        case IndexFormat::UInt16:
            return 2;
        case IndexFormat::UInt32:
            return 4;
        }

        return 0;
    }

    static_assert(sizeof(ResourceState) == 1);
    static_assert(sizeof(IndexFormat) == 1);
    static_assert(sizeof(FrameStatus) == 1);
    static_assert(std::is_trivially_copyable_v<GeometryDesc>);

} // namespace Iridium
