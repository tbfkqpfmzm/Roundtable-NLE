#version 450
// Fullscreen quad fragment shader — samples a texture.
// Step 2: Proof-of-life rendering.

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

void main()
{
    // The compositor writes BGRA byte order (result.bgra swizzle in
    // composite.comp) for Qt CPU readback compatibility.  When displaying
    // on-screen via VulkanViewport we need to undo that swizzle.
    vec4 s = texture(texSampler, fragTexCoord);
    outColor = s.bgra;
}
