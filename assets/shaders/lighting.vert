#version 450

layout(location = 0) out vec2 fragTexCoord;

void main() {
    // Generate a full-screen triangle using just the vertex ID (0, 1, or 2)
    fragTexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    
    // Convert the UV coordinates (0 to 1) to NDC coordinates (-1 to 1)
    gl_Position = vec4(fragTexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
}