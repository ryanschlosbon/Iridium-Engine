#version 450

layout(set = 1, binding = 0) uniform sampler2D texSampler;
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {

    vec4 texColor = texture(texSampler, fragTexCoord);

    if(texColor.a < 0.1) {
        discard;
    }

    outColor = texColor * vec4(fragColor, 1.0);

}