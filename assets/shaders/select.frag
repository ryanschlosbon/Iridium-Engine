#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 2) uniform sampler2D gAlbedoEmissive;

// 1. CRANK THIS UP to see it easily!
#define LINE_WEIGHT 4.0

void main() {
    vec2 texSize = textureSize(gAlbedoEmissive, 0);
    float dx = (1.0 / texSize.x) * LINE_WEIGHT;
    float dy = (1.0 / texSize.y) * LINE_WEIGHT;

    vec2 uvCenter   = fragTexCoord;
    vec2 uvRight    = vec2(uvCenter.x + dx, uvCenter.y);
    vec2 uvTop      = vec2(uvCenter.x,      uvCenter.y - dy);
    vec2 uvTopRight = vec2(uvCenter.x + dx, uvCenter.y - dy);

    // =========================================================
    // VISUAL PRINTF: Uncomment these two lines. 
    // If your selected car doesn't turn solid red, your C++ is broken!
    // =========================================================
    //float alpha = texture(gAlbedoEmissive, fragTexCoord).a;
    //if (alpha < 0.0) { outColor = vec4(1.0, 0.0, 0.0, 1.0); return; }

    // 2. SAFER MASK: Check for < 0.0 to prevent Bilinear Smudging!
    #define GET_MASK(uv) (texture(gAlbedoEmissive, uv).a < 0.0 ? 1.0 : 0.0)

    float mCenter   = GET_MASK(uvCenter);
    float mTop      = GET_MASK(uvTop);
    float mRight    = GET_MASK(uvRight);
    float mTopRight = GET_MASK(uvTopRight);
   
    float dT  = abs(mCenter - mTop);
    float dR  = abs(mCenter - mRight);
    float dTR = abs(mCenter - mTopRight);
   
    float delta = max(max(dT, dR), dTR);

    if (delta > 0.0) {
        // Bright Neon Cyan so it pops against the red car!
        outColor = vec4(0.0, 1.0, 1.0, 1.0); 
    } else {
        discard; 
    }
}