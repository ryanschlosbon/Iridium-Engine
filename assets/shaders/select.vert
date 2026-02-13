#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushConsts {
    mat4 renderMatrix;
} push;

void main() {
    // 1. Calculate Clip Space Position
    vec4 clipPos = ubo.proj * ubo.view * push.renderMatrix * vec4(inPosition, 1.0);

    // 2. Transform Normal to View Space
    mat3 normalMatrix = mat3(ubo.view * push.renderMatrix);
    vec3 viewNormal = normalize(normalMatrix * inPosition);

    // 3. Project Normal to 2D (Screen Space Direction)
    // We use the projection matrix's top-left 2x2 to affect the X/Y direction
    vec2 offset = mat2(ubo.proj) * viewNormal.xy;

    // --- FIX 1: SAFE NORMALIZE ---
    // If viewNormal is (0,0,1) [facing camera], offset is (0,0).
    // normalize(0,0) is undefined/NaN, causing stretching.
    if (length(offset) > 0.0001) {
        offset = normalize(offset);
    } else {
        offset = vec2(0.0);
    }

    // 4. Apply Extrusion
    // 0.004 is the thickness. 
    float outlineWidth = 0.004; 

    // Aspect Ratio Correction (Approximate for 16:9)
    offset.x /= 1.77;

    clipPos.xy += offset * outlineWidth * clipPos.w;

    // --- FIX 2: MANUAL DEPTH BIAS ---
    // Push the outline slightly "deeper" into the screen (increase Z).
    // This prevents the outline from clipping through the front of the grill.
    clipPos.z += 0.001 * clipPos.w;

    gl_Position = clipPos;
}