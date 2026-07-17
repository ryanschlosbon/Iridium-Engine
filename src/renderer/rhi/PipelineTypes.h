#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include <glm/glm.hpp>
#include "renderer/rhi/RenderHandles.h" 

namespace Iridium {

    enum class ShaderProgram : uint8_t { PbrGBuffer, PbrForward };
    enum class RenderPassClass : uint8_t { GBuffer, Forward };
    enum class PrimitiveTopology : uint8_t { TriangleList };
    enum class PolygonMode : uint8_t { Fill, Line };
    enum class CullMode : uint8_t { None, Front, Back };
    enum class FrontFace : uint8_t { Clockwise, CounterClockwise };
    enum class RenderQueue : uint8_t { Opaque, Transparent };
    enum class BlendMode : uint8_t { Opaque, AlphaBlend, Additive };
    enum class DepthCompare : uint8_t { Less, LessOrEqual, Greater, Always };

    constexpr uint8_t ColorWriteR = 0x01;
    constexpr uint8_t ColorWriteG = 0x02;
    constexpr uint8_t ColorWriteB = 0x04;
    constexpr uint8_t ColorWriteA = 0x08;
    constexpr uint8_t ColorWriteAll = 0x0F;

    // Defines exactly how the GPU should render this specific material
    struct PipelineStateDesc {
        ShaderProgram shaderProgram = ShaderProgram::PbrGBuffer;
        RenderPassClass renderPass = RenderPassClass::GBuffer;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        PolygonMode polygonMode = PolygonMode::Fill;
        CullMode cullMode = CullMode::Back;
        FrontFace frontFace = FrontFace::Clockwise;
        BlendMode blendMode = BlendMode::Opaque;
        DepthCompare depthCompare = DepthCompare::Less;
        uint8_t colorWriteMask = ColorWriteAll;
        bool depthTest = true;
        bool depthWrite = true;

        constexpr bool operator==(const PipelineStateDesc&) const noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<PipelineStateDesc>);

    // The complete package handed from the AssetManager to the Backend
    struct MaterialAsset {
        std::string name;

        // Visual Data
        TextureHandle albedoMap;
        TextureHandle normalMap;
        TextureHandle pbrMap;
        TextureHandle emissiveMap;
        TextureHandle transmissionMap;

        glm::vec4 baseColor = glm::vec4(1.0f);
        glm::vec4 emissiveFactor = glm::vec4(0.0f);
        float metallic = 0.0f;
        float roughness = 1.0f;
        float normalScale = 1.0f;
        RenderQueue renderQueue = RenderQueue::Opaque;
        float alphaCutoff = 0.0f;
        float transmissionFactor = 0.0f;

        // Pipeline State Data
        PipelineStateDesc pipelineState;
    };

    struct MaterialBinding {
        MaterialHandle material;
        PipelineHandle pipeline;
        RenderQueue renderQueue;
        uint64_t opaqueSortKey;
    };

    constexpr uint64_t makeOpaqueSortKey(PipelineHandle pipeline, MaterialHandle material) noexcept {
        return (static_cast<uint64_t>(pipeline.id) << 32) | material.id;
    }

}
