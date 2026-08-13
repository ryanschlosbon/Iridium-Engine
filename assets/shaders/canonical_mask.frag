#version 450

layout(location = 0) out vec4 outNormalF90;
layout(location = 1) out vec4 outDiffuseAo;
layout(location = 2) out vec4 outEmissive;
layout(location = 3) out vec4 outF0Roughness;
layout(location = 4) out uint outMaterialFlags;

void main() {
    outNormalF90 = vec4(0.0);
    outDiffuseAo = vec4(0.0);
    outEmissive = vec4(0.0, 0.0, 0.0, -1.0);
    outF0Roughness = vec4(0.0);
    outMaterialFlags = 0u;
}
