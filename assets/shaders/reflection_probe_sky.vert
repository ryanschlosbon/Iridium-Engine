#version 450

layout(location = 0) out vec2 clipPosition;

void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2,
        gl_VertexIndex & 2) * 2.0 - 1.0;
    clipPosition = position;
    gl_Position = vec4(position, 1.0, 1.0);
}
