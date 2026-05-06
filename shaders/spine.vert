#version 450
// Spine mesh vertex shader.
// Step 9: GPU Spine rendering.
//
// Receives per-vertex data from spine-cpp mesh extraction:
//   - position (2D)
//   - texture coordinates
//   - vertex color (RGBA, premultiplied alpha from Spine slots)

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;           // Model-View-Projection matrix
    float opacity;      // Layer opacity (0–1)
} pc;

void main()
{
    gl_Position = pc.mvp * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
    fragColor = inColor * pc.opacity;
}
