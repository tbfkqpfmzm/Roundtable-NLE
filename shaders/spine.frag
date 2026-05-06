#version 450
// Spine mesh fragment shader.
// Step 9: GPU Spine rendering.
//
// Samples the atlas texture and multiplies by vertex color.
// Uses premultiplied alpha blending (standard for Spine).

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D atlasTexture;

void main()
{
    vec4 texel = texture(atlasTexture, fragTexCoord);
    
    // Premultiplied alpha: color channels are already multiplied by alpha
    outColor = texel * fragColor;
}
