#pragma once
#include <string>
#include <glm/glm.hpp>
#include "renderer/rhi/RenderHandles.h" 

namespace Iridium {

    enum class BlendMode { Opaque, AlphaBlend, Additive };
    enum class DepthCompare { Less, LessOrEqual, Greater, Always };

    // Defines exactly how the GPU should render this specific material
    struct PipelineStateDesc {
        std::string vertexShaderPath;
        std::string fragmentShaderPath;
        BlendMode blendMode = BlendMode::Opaque;
        DepthCompare depthCompare = DepthCompare::Less;
        bool depthWrite = true;
        bool cullBackFace = true;
    };

    // The complete package handed from the AssetManager to the Backend
    struct MaterialAsset {
        std::string name;

        // Visual Data
        TextureHandle albedoMap;
        TextureHandle normalMap;
        TextureHandle pbrMap;

        glm::vec4 baseColor = glm::vec4(1.0f);
        float metallic = 0.0f;
        float roughness = 1.0f;
        float emissive = 0.0f;

        // Pipeline State Data
        PipelineStateDesc pipelineState;
    };

}