#version 450

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outAlbedo;

void main() {
    // We must declare the normal output, even though colorWriteMask discards it
    outNormal = vec4(0.0);
    
    // Write the -1.0 "invisible tag" to the Albedo Alpha channel
    outAlbedo = vec4(0.0, 0.0, 0.0, -1.0); 
}